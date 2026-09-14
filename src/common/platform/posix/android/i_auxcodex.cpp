/*
** i_auxcodex.cpp
** M1 probe: can C++ read Selaco's codex unlock state directly out of ZScript?
**
** This file publishes NOTHING. It resolves the unlock map, computes a bitmask over the gate names
** the shipped MANUAL.json actually uses, and logs one line when that mask changes. No JNI, no Java,
** no panel content, no JSON. It exists to prove or kill the bridge before any UI is built, because
** every design that reached this point died in review when it bundled unproven mechanisms together.
**
** WHY A DIRECT READ IS LEGITIMATE HERE, not a hack:
** ZScript's Map<Name,Int> is ZSMap<uint32_t,uint32_t>, which derives PUBLICLY from TMap
** (scripting/core/maps.h:19) and is placement-constructed straight into the field's storage
** (scripting/core/types.cpp:2513, reached from InitializeValue at :2530). The engine already does
** this in both directions: vmnatives.cpp:63 declares `static ZSMap<FName, DObject*> AllServices;`
** in C++, inserts and iterates it from C++, and hands the same bytes to ZScript as
** Map<Name, Service> (wadsrc/static/zscript/engine/base.zs:184). So this is the established in-tree
** representation, not an assumption about layout.
**
** The field-by-name lookup follows the precedent at maploader.cpp:1256, which reads a
** ZScript-declared field off an actor's class the same way.
**
** THE SAFETY RULE THAT GOVERNS EVERYTHING HERE:
** A wrong read does not merely fail, it can invent unlock bits - and a nonzero bit publishes content
** the player has not earned. Spoiling the game is the one outcome that was ruled out. So every
** validation failure sets the mask to ZERO and latches off, and the mask is only ever built from a
** map this code has positively identified as Map<Name,Int> at the expected size. Uncertainty resolves
** to "locked", never to "unlocked".
*/

#include <stdint.h>

#include "actor.h"
#include "d_player.h"
#include "doomstat.h"
#include "dobjtype.h"
#include "printf.h"
#include "vm.h"          // maps.h needs X_FORMAT_ERROR from here
#include "maps.h"
#include "symbols.h"
#include "types.h"

// The gate names the shipped MANUAL.json actually uses: 16 on entries, plus "safesect" on a section.
// Extracted from the real 42,172-byte lump rather than guessed.
//
// HARDCODED ON PURPOSE, for this probe only. The real implementation must read these from
// MANUAL.json so it cannot drift from the player's own copy of the game - but parsing that lump is a
// SEPARATE unproven mechanism, and bundling it here would mean a failure could not be attributed.
// One variable at a time.
static const char *const GateNames[] = {
	"armor", "bandolier", "cabinet", "capacity", "clearance", "credits", "enemymine", "gwyn",
	"invasiontier", "keypad", "milestone", "parts", "safesect", "tech", "wallcrack", "weaponkit",
	"yellowdecal"
};
static const int NumGates = (int)(sizeof(GateNames) / sizeof(GateNames[0]));

static bool ProbeBroken = false;      // latched; a bridge that cannot work will not start working
static uint64_t LastMask = 0;
static bool LastReported = false;
static int LastState = -1;            // 0 no class, 1 no player, 2 no item, 3 bad field, 4 ok

static void ReportOnce(int state, uint64_t mask, int openCount)
{
	if (LastReported && state == LastState && mask == LastMask)
		return;   // edge-triggered: silence unless something actually changed

	LastReported = true;
	LastState = state;
	LastMask = mask;

	switch (state)
	{
	case 0: Printf("AuxCodex: bridge=absent (no ManualItem class - not Selaco, or renamed)\n"); break;
	case 1: Printf("AuxCodex: bridge=waiting (no player pawn yet)\n"); break;
	case 2: Printf("AuxCodex: bridge=waiting (player has no ManualItem yet)\n"); break;
	case 3: Printf("AuxCodex: bridge=FAILED (unlocks field missing or not Map<Name,Int>)\n"); break;
	default:
		Printf("AuxCodex: bridge=ok gates=%d open=%d mask=0x%016llx\n",
			NumGates, openCount, (unsigned long long)mask);
		break;
	}
}

// Read the unlock map and build a bitmask, one bit per known gate name. Returns false and leaves
// mask zeroed on any doubt whatsoever.
static bool ReadUnlockMask(uint64_t &mask, int &openCount, int &state)
{
	mask = 0;
	openCount = 0;

	// noCreate: if Selaco never registered this name, do not add it to the name table just to look
	// it up. Index 0 means "no such name has ever existed", which is the graceful absent case.
	FName itemName("ManualItem", true);
	if (itemName == NAME_None)
	{
		state = 0;
		return false;
	}

	PClassActor *cls = PClass::FindActor(itemName);
	if (cls == nullptr)
	{
		state = 0;
		return false;
	}

	if (!playeringame[consoleplayer] || players[consoleplayer].mo == nullptr)
	{
		// The frame hook runs before screen->BeginFrame() with no gamestate guard, so this is the
		// normal case at the title screen and during startup, not an error.
		state = 1;
		return false;
	}

	AActor *item = players[consoleplayer].mo->FindInventory(cls, true);
	if (item == nullptr)
	{
		state = 2;
		return false;
	}

	PField *field = dyn_cast<PField>(item->GetClass()->FindSymbol(FName("unlocks"), true));
	if (field == nullptr || (field->Flags & (VARF_Native | VARF_Static)))
	{
		state = 3;
		return false;
	}

	// Strict type validation. This is the spoiler-critical check: casting a field that is NOT a
	// Map<Name,Int> would read arbitrary object bytes as a hash table and could fabricate open
	// gates. Verify the kind, the key and value types, and the storage size before touching it.
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

	const ZSMap<uint32_t, uint32_t> *unlocks =
		reinterpret_cast<const ZSMap<uint32_t, uint32_t> *>(reinterpret_cast<const uint8_t *>(item) + field->Offset);

	for (int i = 0; i < NumGates && i < 64; i++)
	{
		// noCreate again: a gate Selaco has never used cannot be unlocked, and looking it up must
		// not mutate the name table from the render path.
		FName gate(GateNames[i], true);
		if (gate == NAME_None)
			continue;

		const uint32_t *val = unlocks->CheckKey((uint32_t)gate.GetIndex());

		// Selaco's Unlock() stores 2 for newly-unlocked and 1 for already-seen (manual_handler.zs:30),
		// so any value > 0 means unlocked. Absent means locked. Treating "present" as unlocked
		// without checking the value would be wrong if a future version ever stored 0.
		if (val != nullptr && *val > 0)
		{
			mask |= (uint64_t)1 << i;
			openCount++;
		}
	}

	state = 4;
	return true;
}

void I_AuxCodexProbe()
{
	if (ProbeBroken)
		return;

	uint64_t mask = 0;
	int openCount = 0;
	int state = -1;

	ReadUnlockMask(mask, openCount, state);

	// A hard field/type failure is permanent: latch off after reporting it once, so a broken bridge
	// costs one line and then nothing.
	ReportOnce(state, mask, openCount);
	if (state == 3)
		ProbeBroken = true;
}
