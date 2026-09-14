/*
** i_auxcodex.cpp
** Codex bridge for the second-screen panel: reads Selaco's manual and its unlock state.
**
** This file publishes NOTHING to the panel. It resolves the unlock map, parses MANUAL.json,
** evaluates which nodes the player is allowed to see, and logs one line when the answer changes.
** No JNI, no Java, no UI. Content handling and the panel are separate steps, kept separate so a
** failure can be attributed to one mechanism - three earlier designs died in review because they
** bundled several unproven pieces together.
**
** M1 (the unlock bridge) is PROVEN ON DEVICE: bridge=ok gates=17, mask tracked a real unlock.
** M2 (this) adds the manual itself.
**
** WHY READING THE ZSCRIPT MAP DIRECTLY IS LEGITIMATE, not a layout hack:
** ZScript's Map<Name,Int> is ZSMap<uint32_t,uint32_t>, which derives PUBLICLY from TMap
** (scripting/core/maps.h:19) and is placement-constructed into the field's storage
** (scripting/core/types.cpp:2513, via InitializeValue at :2530). The engine does this in both
** directions already: vmnatives.cpp:63 declares `static ZSMap<FName, DObject*> AllServices;` in
** C++, inserts and iterates it there, and hands the same bytes to ZScript as Map<Name, Service>
** (wadsrc/static/zscript/engine/base.zs:184). The field-by-name lookup follows maploader.cpp:1256.
**
** CheckKey, never Map.Get. Get is the INSERTING accessor: calling it would mutate the player's
** unlock map, corrupt their save, and make Selaco's own codex start printing '???' for sections it
** should show. The const non-inserting overload is mandatory (tarray.h:1319-1323).
**
** ===========================================================================================
** SPOILER SAFETY IS THE ACCEPTANCE CRITERION, AND IT SHAPES EVERY DECISION BELOW.
**
** Selaco is a story game. Showing a locked codex entry is worse than showing nothing, so every
** branch here resolves uncertainty to LOCKED. Specifically:
**   - The manual is refused entirely unless "version" is exactly the schema this code was written
**     against. A future Selaco that changes the format gets a blank panel, not a guess.
**   - Every key on every node is checked against a whitelist. An unrecognised key means Selaco may
**     have introduced a gating mechanism we do not understand, so that node and its whole subtree
**     are treated as locked. This is the one defence against fail-OPEN drift: without it, a new
**     gate key such as "requires" would read as "no gate" and publish locked content.
**   - Visibility is CONJUNCTIVE: a node is visible only if it and every ancestor is open. Three
**     entries in the shipped file (Overview, Workbench, Safe Room Extension) carry no unlock key of
**     their own and are hidden solely by their section's 'safesect' gate. Filtering per-node would
**     leak "Safe Room Extension", which names a mechanic the player has not found.
**   - A failed or not-yet-available bridge yields an all-locked verdict, never an all-open one.
**
** The gate test is `!= 0`, matching Selaco's real predicate at manual.zs:105-109
** (`hasSection = GetIfExists(section.unlock); if(!hasSection) return;` - an early cut BEFORE the
** recursion) and :176-184 (`if(!entryUnlockStatus) continue;`). Three other sites look like the
** predicate and are not: :86 (`> 1`) drives the NEW badge, :121 accumulates `hasEntries` for
** `showLocked`, and :124 accumulates `sectionHasLockedItems` for the '???' teaser.
** ===========================================================================================
*/

#include <stdint.h>

#include "actor.h"
#include "cmdlib.h"
#include "d_player.h"
#include "doomstat.h"
#include "dobjtype.h"
#include "filesystem.h"
#include "printf.h"
#include "vm.h"          // maps.h needs X_FORMAT_ERROR from here
#include "maps.h"
#include "symbols.h"
#include "types.h"
#include "zstring.h"

// Match the tree's existing rapidjson configuration (see src/common/engine/serializer.cpp:35-38)
// rather than inventing a second one.
#define RAPIDJSON_48BITPOINTER_OPTIMIZATION 0
#define RAPIDJSON_HAS_CXX11_RVALUE_REFS 1
#define RAPIDJSON_HAS_CXX11_RANGE_FOR 1
#include "rapidjson/document.h"
#include "rapidjson/rapidjson.h"

// The schema this code was written against. Refuse anything else - see the spoiler note above.
static const char *const SupportedVersion = "1.0";

// Every key that legitimately appears on a node in the shipped MANUAL.json. Anything else means
// Selaco may have added semantics we do not model, so the node is treated as locked.
//
// Counts in the shipped 42,172-byte file, for reference: sections carry title(10) sections(2)
// entries(10) id(9) unlock(1); entries carry title(40) content(40) unlock(16) noheader(3).
static const char *const SectionKeys[] = { "title", "sections", "entries", "id", "unlock" };
static const char *const EntryKeys[]   = { "title", "content", "unlock", "noheader" };

static bool KeyAllowed(const char *key, const char *const *allowed, size_t count)
{
	for (size_t i = 0; i < count; i++)
	{
		if (strcmp(key, allowed[i]) == 0)
			return true;
	}
	return false;
}

// ---------------------------------------------------------------------------------------------
// The parsed manual. Gate names come from the FILE, never from a hardcoded list, so this cannot
// drift from the player's own copy of the game.
// ---------------------------------------------------------------------------------------------

struct CodexNode
{
	FString Title;
	int GateBit = -1;         // -1 = ungated; otherwise an index into Gates
	bool IsEntry = false;
	bool Trusted = true;      // false if this node had an unrecognised key
	int ContentLen = 0;
	TArray<int> Children;     // indices into Nodes
};

static TArray<CodexNode> Nodes;
static TArray<FName> Gates;
static int RootNode = -1;
static bool ManualLoaded = false;
static bool ManualRefused = false;   // latched; a manual we cannot trust is never retried

// Returns the gate's bit index, allocating one on first sight. -1 means "ungated".
static int GateBitFor(const char *name)
{
	if (name == nullptr || *name == '\0' || stricmp(name, "none") == 0)
		return -1;

	FName gate(name);
	for (unsigned i = 0; i < Gates.Size(); i++)
	{
		if (Gates[i] == gate)
			return (int)i;
	}
	if (Gates.Size() >= 64)
		return -2;      // out of bits: caller treats this as permanently locked, never as open
	Gates.Push(gate);
	return (int)Gates.Size() - 1;
}

static int BuildNode(const rapidjson::Value &obj, bool isEntry)
{
	CodexNode node;
	node.IsEntry = isEntry;

	const char *const *allowed = isEntry ? EntryKeys : SectionKeys;
	const size_t allowedCount = isEntry
		? sizeof(EntryKeys) / sizeof(EntryKeys[0])
		: sizeof(SectionKeys) / sizeof(SectionKeys[0]);

	for (auto it = obj.MemberBegin(); it != obj.MemberEnd(); ++it)
	{
		if (!KeyAllowed(it->name.GetString(), allowed, allowedCount))
		{
			// Unrecognised key: this node might be gated by something we do not understand.
			node.Trusted = false;
		}
	}

	if (obj.HasMember("title") && obj["title"].IsString())
		node.Title = obj["title"].GetString();

	if (obj.HasMember("content") && obj["content"].IsString())
		node.ContentLen = (int)strlen(obj["content"].GetString());

	if (obj.HasMember("unlock"))
	{
		if (obj["unlock"].IsString())
			node.GateBit = GateBitFor(obj["unlock"].GetString());
		else
			node.Trusted = false;   // an unlock we cannot read is not an absent unlock
	}

	const int self = (int)Nodes.Size();
	Nodes.Push(node);

	if (!isEntry)
	{
		if (obj.HasMember("sections") && obj["sections"].IsArray())
		{
			for (auto &child : obj["sections"].GetArray())
			{
				if (child.IsObject())
				{
					// The recursive call pushes into Nodes and can REALLOCATE it, so the child index
					// must be computed into a local FIRST. Pushing the call's result directly would
					// evaluate the Nodes[self] subscript into a reference before the argument runs,
					// and that reference then dangles - which silently corrupted every child link
					// instead of crashing, and showed up as visible=0 on device.
					const int childIndex = BuildNode(child, false);
					Nodes[self].Children.Push(childIndex);
				}
				else
				{
					Nodes[self].Trusted = false;
				}
			}
		}
		if (obj.HasMember("entries") && obj["entries"].IsArray())
		{
			for (auto &child : obj["entries"].GetArray())
			{
				if (child.IsObject())
				{
					const int childIndex = BuildNode(child, true);   // see the note above
					Nodes[self].Children.Push(childIndex);
				}
				else
				{
					Nodes[self].Trusted = false;
				}
			}
		}
	}
	return self;
}

// Parse MANUAL.json once. Any doubt refuses the whole manual rather than part of it.
static bool LoadManual()
{
	if (ManualRefused)
		return false;
	if (ManualLoaded)
		return true;

	const int lump = fileSystem.CheckNumForFullName("MANUAL.json");
	if (lump < 0)
	{
		Printf("AuxCodex: MANUAL.json not found - codex panel unavailable\n");
		ManualRefused = true;
		return false;
	}

	FString text = GetStringFromLump(lump);

	// Default parse flags do not throw; errors surface through HasParseError. That matters because
	// this must never take the process down over a data file we do not control.
	rapidjson::Document doc;
	doc.Parse(text.GetChars(), text.Len());
	if (doc.HasParseError() || !doc.IsObject())
	{
		Printf("AuxCodex: MANUAL.json failed to parse (error %d) - codex panel unavailable\n",
			(int)doc.GetParseError());
		ManualRefused = true;
		return false;
	}

	// Version gate. A format change is exactly when our assumptions about gating stop holding, so
	// an unknown version means show nothing at all.
	if (!doc.HasMember("version") || !doc["version"].IsString()
		|| strcmp(doc["version"].GetString(), SupportedVersion) != 0)
	{
		Printf("AuxCodex: MANUAL.json version is not %s - refusing it rather than guessing\n",
			SupportedVersion);
		ManualRefused = true;
		return false;
	}

	if (!doc.HasMember("sections") || !doc["sections"].IsArray())
	{
		Printf("AuxCodex: MANUAL.json has no sections array - codex panel unavailable\n");
		ManualRefused = true;
		return false;
	}

	// A synthetic ungated root so the conjunctive walk has a single entry point.
	CodexNode root;
	root.Title = "Manual";
	RootNode = (int)Nodes.Size();
	Nodes.Push(root);
	for (auto &child : doc["sections"].GetArray())
	{
		if (child.IsObject())
		{
			const int childIndex = BuildNode(child, false);   // see the reallocation note in BuildNode
			Nodes[RootNode].Children.Push(childIndex);
		}
		else
		{
			Nodes[RootNode].Trusted = false;
		}
	}

	ManualLoaded = true;
	int untrusted = 0;
	for (unsigned i = 0; i < Nodes.Size(); i++)
		if (!Nodes[i].Trusted) untrusted++;

	Printf("AuxCodex: manual loaded version=%s nodes=%u gates=%u untrusted=%d\n",
		SupportedVersion, Nodes.Size(), Gates.Size(), untrusted);
	return true;
}

// ---------------------------------------------------------------------------------------------
// The unlock bridge (M1, proven on device).
// ---------------------------------------------------------------------------------------------

static bool ProbeBroken = false;
static uint64_t LastMask = 0;
static int LastVisible = -1;
static int LastState = -1;   // 0 no class, 1 no player, 2 no item, 3 bad field, 4 ok

static bool ReadUnlockMask(uint64_t &mask, int &state)
{
	mask = 0;

	// noCreate: never add a name to the table just to look it up, and index 0 means Selaco has
	// never registered it - the graceful "not this game" case.
	FName itemName("ManualItem", true);
	if (itemName == NAME_None) { state = 0; return false; }

	PClassActor *cls = PClass::FindActor(itemName);
	if (cls == nullptr) { state = 0; return false; }

	if (!playeringame[consoleplayer] || players[consoleplayer].mo == nullptr)
	{
		// Normal at the title screen and during startup: the frame hook runs before BeginFrame with
		// no gamestate guard.
		state = 1;
		return false;
	}

	AActor *item = players[consoleplayer].mo->FindInventory(cls, true);
	if (item == nullptr) { state = 2; return false; }

	PField *field = dyn_cast<PField>(item->GetClass()->FindSymbol(FName("unlocks"), true));
	if (field == nullptr || (field->Flags & (VARF_Native | VARF_Static))) { state = 3; return false; }

	// Strict type validation, and it is spoiler-critical: casting a field that is NOT a
	// Map<Name,Int> would read arbitrary object bytes as a hash table and could fabricate open
	// gates. NewMap() is deliberately NOT used as the comparison - it mutates the global type table
	// (types.cpp:2849-2859), which must not happen from the render path.
	PType *type = field->Type;
	if (type == nullptr || !type->isMap() || type->Size != sizeof(ZSMap<uint32_t, uint32_t>))
	{
		state = 3;
		return false;
	}
	PMap *mapType = static_cast<PMap *>(type);
	if (mapType->KeyType != TypeName || mapType->ValueType == nullptr || !mapType->ValueType->isInt())
	{
		state = 3;
		return false;
	}

	// Straight-line from here: no allocation and nothing that can collect, so the map cannot be
	// reconstructed underneath us mid-read.
	const ZSMap<uint32_t, uint32_t> *unlocks = reinterpret_cast<const ZSMap<uint32_t, uint32_t> *>(
		reinterpret_cast<const uint8_t *>(item) + field->Offset);

	for (unsigned i = 0; i < Gates.Size() && i < 64; i++)
	{
		const uint32_t *val = unlocks->CheckKey((uint32_t)Gates[i].GetIndex());
		// Selaco stores 2 for newly-unlocked and 1 for seen (manual_handler.zs:30). Absent means
		// locked. Test != 0 rather than presence, so a future version storing 0 still reads locked.
		if (val != nullptr && *val > 0)
			mask |= (uint64_t)1 << i;
	}

	state = 4;
	return true;
}

// ---------------------------------------------------------------------------------------------
// Visibility. Conjunctive, and closed on any doubt.
// ---------------------------------------------------------------------------------------------

static bool NodeOpen(const CodexNode &node, uint64_t mask, bool bridgeOk)
{
	if (!node.Trusted)  return false;    // unrecognised key: assume it might be a gate
	if (node.GateBit == -2) return false; // more than 64 distinct gates
	if (node.GateBit < 0)  return true;   // genuinely ungated
	if (!bridgeOk)         return false;  // gated, and we cannot prove it is open
	return ((mask >> node.GateBit) & 1) != 0;
}

static void CountVisible(int index, uint64_t mask, bool bridgeOk, int &entries, int &sections)
{
	const CodexNode &node = Nodes[index];
	if (!NodeOpen(node, mask, bridgeOk))
		return;                            // prunes the whole subtree - the conjunction

	if (node.IsEntry) entries++;
	else if (index != RootNode) sections++;

	for (unsigned i = 0; i < node.Children.Size(); i++)
		CountVisible(node.Children[i], mask, bridgeOk, entries, sections);
}

void I_AuxCodexProbe()
{
	if (ProbeBroken)
		return;

	uint64_t mask = 0;
	int state = -1;
	const bool bridgeOk = ReadUnlockMask(mask, state);

	if (state == 3)
	{
		if (LastState != 3)
			Printf("AuxCodex: bridge=FAILED (unlocks field missing or not Map<Name,Int>)\n");
		LastState = 3;
		ProbeBroken = true;
		return;
	}

	// Load the manual only once there is a game to read it from, so a title-screen frame does not
	// pay for it. Gate names are discovered here, which is why the mask is read again below.
	if (state >= 2 && !ManualLoaded && !ManualRefused)
	{
		if (LoadManual())
			ReadUnlockMask(mask, state);   // re-read now that Gates is populated
	}

	int entries = 0, sections = 0;
	if (ManualLoaded && RootNode >= 0)
		CountVisible(RootNode, mask, bridgeOk, entries, sections);

	// Edge-triggered: silence unless the verdict actually changed.
	if (state == LastState && mask == LastMask && entries == LastVisible)
		return;
	LastState = state;
	LastMask = mask;
	LastVisible = entries;

	switch (state)
	{
	case 0: Printf("AuxCodex: bridge=absent (no ManualItem class - not Selaco, or renamed)\n"); break;
	case 1: Printf("AuxCodex: bridge=waiting (no player pawn yet)\n"); break;
	case 2: Printf("AuxCodex: bridge=waiting (player has no ManualItem yet)\n"); break;
	default:
		Printf("AuxCodex: bridge=ok mask=0x%016llx visible=%d entries, %d sections\n",
			(unsigned long long)mask, entries, sections);
		break;
	}
}
