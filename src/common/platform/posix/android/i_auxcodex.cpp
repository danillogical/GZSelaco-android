/*
** i_auxcodex.cpp
** Unlock bridge for the second-screen panel: reads Selaco's unlock state and exposes a generation
** counter that bumps whenever it changes.
**
** This file used to also parse MANUAL.json into a node tree and format a table of contents for
** direct display (BuildToc/CountVisible), but nothing ever read that output - AuxPanel.sCodex was
** write-only on the Java side, since the text overlay it fed was replaced by the readback/splash
** path. That milestone is gone; what remains is the one piece i_auxcodexview.cpp actually consumes,
** I_AuxCodexGeneration, as a rebuild trigger for the dashboard it bakes from the same unlock state.
**
** There is no longer a separate per-frame probe call: I_AuxCodexGeneration() re-checks the unlock
** state itself on every call (see Probe(), below), because the reflection it does is memoised
** against the item's class and therefore cheap enough to poll lazily. i_auxcodexview.cpp already
** calls I_AuxCodexGeneration() at the cadence that needs to observe it move, so a second hook from
** i_auxpanel.cpp would only be a second thing that could disagree about when to look.
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
** SPOILER SAFETY: a failed or not-yet-available bridge yields a checksum of 0, never a guess. The strict
** type validation below (isMap/Size/KeyType/ValueType) is not cosmetic - casting a field that is
** NOT actually a Map<Name,Int> would read arbitrary object bytes as a hash table and could fabricate
** open gates, so it runs every time the field is (re)resolved, never skipped as an optimisation.
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

// ---------------------------------------------------------------------------------------------
// The unlock bridge (M1, proven on device).
// ---------------------------------------------------------------------------------------------

static bool ProbeBroken = false;
static uint64_t LastChecksum = 0;
static int LastState = -1;   // 0 no class, 1 no player, 2 no item, 3 bad field, 4 ok

// Memoised so the reflection below - FindActor, FindInventory, FindSymbol, and the isMap/Size/
// KeyType/ValueType validation - runs once per class rather than every frame in a level. Keyed on
// GetClass() rather than cached forever: a wrong cast fabricating unlock state is worse than a
// once-per-class-change re-resolve, so a class we have not seen before still gets the full
// validation, never a stale field pointer reused across classes.
static PClass *CachedItemClass = nullptr;
static PField *CachedUnlocksField = nullptr;
static bool CachedFieldValid = false;

// Bumped once whenever the unlock checksum changes. Published rather than the checksum itself
// because no caller needs to know WHICH gate changed, only that something did.
//
// The consumer is i_auxcodexview.cpp, which bakes the unlock state into a widget tree at build
// time and therefore has to rebuild when this moves.
//
// Frozen if ProbeBroken latches (a missing or wrong-typed unlocks field): the consumer then builds
// once and never rebuilds, which is the correct degradation - we cannot read unlocks at all, so we
// cannot know when they change.
static unsigned CodexGeneration = 0;

// Re-reads the unlock state and bumps CodexGeneration on any edge. Called from
// I_AuxCodexGeneration() itself rather than from a per-frame hook: the reflection it does is
// memoised (see CachedItemClass above), so there is no separate cost to pay for polling it lazily,
// and every caller of I_AuxCodexGeneration() already runs at the cadence that needs to see it move.
static void Probe();

unsigned I_AuxCodexGeneration()
{
	Probe();
	return CodexGeneration;
}

static bool ReadUnlockChecksum(uint64_t &checksum, int &state)
{
	checksum = 0;

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

	PClass *itemClass = item->GetClass();
	if (itemClass != CachedItemClass)
	{
		// Class changed (or first resolve): redo the reflection and its validation from scratch.
		CachedItemClass = itemClass;
		CachedFieldValid = false;
		CachedUnlocksField = nullptr;

		PField *field = dyn_cast<PField>(itemClass->FindSymbol(FName("unlocks"), true));
		if (field != nullptr && !(field->Flags & (VARF_Native | VARF_Static)))
		{
			// Strict type validation, and it is spoiler-critical: casting a field that is NOT a
			// Map<Name,Int> would read arbitrary object bytes as a hash table and could fabricate
			// open gates. NewMap() is deliberately NOT used as the comparison - it mutates the
			// global type table (types.cpp:2849-2859), which must not happen from the render path.
			PType *type = field->Type;
			if (type != nullptr && type->isMap() && type->Size == sizeof(ZSMap<uint32_t, uint32_t>))
			{
				PMap *mapType = static_cast<PMap *>(type);
				if (mapType->KeyType == TypeName && mapType->ValueType != nullptr && mapType->ValueType->isInt())
				{
					CachedUnlocksField = field;
					CachedFieldValid = true;
				}
			}
		}
	}

	if (!CachedFieldValid) { state = 3; return false; }

	// Straight-line from here: no allocation and nothing that can collect, so the map cannot be
	// reconstructed underneath us mid-read.
	const ZSMap<uint32_t, uint32_t> *unlocks = reinterpret_cast<const ZSMap<uint32_t, uint32_t> *>(
		reinterpret_cast<const uint8_t *>(item) + CachedUnlocksField->Offset);

	// A change-detection checksum, not a positional bitmask - there is no manual parse any more to
	// hand out a fixed, small gate-name-to-bit assignment, and raw FName indices run into the
	// thousands, far past what a 64-bit mask could hold. XOR of a per-key mix is order-independent
	// (map iteration order is not a contract worth depending on) and reacts to a key's value crossing
	// zero either way, which is all a caller comparing generations for inequality needs.
	TMapConstIterator<uint32_t, uint32_t> it(*unlocks);
	const ZSMap<uint32_t, uint32_t>::ConstPair *pair;
	while (it.NextPair(pair))
	{
		// Selaco stores 2 for newly-unlocked and 1 for seen (manual_handler.zs:30). Test != 0 rather
		// than presence, so a future version storing 0 still reads locked.
		if (pair->Value > 0)
		{
			uint64_t h = (uint64_t)pair->Key * 0x9E3779B97F4A7C15ULL;
			h ^= h >> 32;
			checksum ^= h;
		}
	}

	state = 4;
	return true;
}

static void Probe()
{
	if (ProbeBroken)
		return;

	uint64_t checksum = 0;
	int state = -1;
	ReadUnlockChecksum(checksum, state);

	if (state == 3)
	{
		if (LastState != 3)
			Printf("AuxCodex: bridge=FAILED (unlocks field missing or not Map<Name,Int>)\n");
		LastState = 3;
		ProbeBroken = true;
		return;
	}

	// Edge-triggered: silence unless the verdict actually changed.
	if (state == LastState && checksum == LastChecksum)
		return;
	LastState = state;
	LastChecksum = checksum;

	// One bump per edge, so a consumer rebuilding off this counter always matches the log line
	// about to be printed for the same edge.
	CodexGeneration++;

	switch (state)
	{
	case 0: Printf("AuxCodex: bridge=absent (no ManualItem class - not Selaco, or renamed)\n"); break;
	case 1: Printf("AuxCodex: bridge=waiting (no player pawn yet)\n"); break;
	case 2: Printf("AuxCodex: bridge=waiting (player has no ManualItem yet)\n"); break;
	default:
		Printf("AuxCodex: bridge=ok checksum=0x%016llx\n", (unsigned long long)checksum);
		break;
	}
}
