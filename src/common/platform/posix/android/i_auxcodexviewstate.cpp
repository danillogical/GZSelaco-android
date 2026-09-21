/*
** i_auxcodexviewstate.cpp
** The view-state manager: sample what the player was looking at, put it back on the second screen.
**
**---------------------------------------------------------------------------
** Copyright 2026 Selaco Android port contributors
** All rights reserved.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions
** are met:
**
** 1. Redistributions of source code must retain the above copyright
**    notice, this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**    notice, this list of conditions and the following disclaimer in the
**    documentation and/or other materials provided with the distribution.
** 3. The name of the author may not be used to endorse or promote products
**    derived from this software without specific prior written permission.
**
** THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
** IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
** OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
** IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
** INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
** NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
** THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**---------------------------------------------------------------------------
**
** What this file is FOR is in i_auxcodexviewstate.h. What is here is the mechanism, and why the handful of
** script symbols are reached by VM reflection rather than by a compile-time type: see WHY THIS IS C++ AND
** NOT ZSCRIPT at the top of i_auxstandbycodex.cpp, and the safety argument for the resolvers themselves in
** i_auxvmreflect.h.
*/

// dobjtype.h and dobjgc.h are not self-contained (they lean on FName, FString and the DObject declaration
// being in scope already), so dobject.h leads - it is the header that pulls that chain in, and every engine
// translation unit that touches PClass reaches it the same way.
#include "dobject.h"
#include "dobjtype.h"
#include "printf.h"
#include "symbols.h"
#include "vm.h"
#include "zstring.h"

#include "i_auxcodexviewstate.h"
#include "i_auxvmreflect.h"   // the resolvers, shared with both mode files

using namespace AuxView;

namespace AuxCodexView
{

// The reader app, the scroll view inside it and the scrollbar inside that - the whole class chain this file
// walks. PDAReaderWindow is the one app of the six whose view state is worth carrying: it is the only one
// the player READS rather than glances at, and the passcode buried three screens down a datalog is the
// concrete case this feature exists for.
static const char *const ReaderClassName = "PDAReaderWindow";
static const char *const ScrollClassName = "UIVerticalScroll";
static const char *const SliderClassName = "UISlider";

// One latch, and it is deliberately NOT split into absent/broken the way the two mode files' are. Those two
// have to tell "this is not the full Selaco" apart from "Selaco does not look the way this was written
// against", because one is silent and the other is a diagnostic. This file cannot be reached at all on a
// game with no PDAMenu3 - both callers already have a live one - so every failure here IS the diagnostic
// case, and one latch says it once and then costs nothing.
//
// Never retried, for the same reason the mode files never retry: a class that does not exist will not
// appear and a signature that does not match will not change, so retrying would put a symbol lookup on the
// per-frame sampling path forever.
static bool ViewStateBroken = false;
static bool ViewStateResolved = false;

static PClass *ReaderClass = nullptr;
static PClass *ScrollClass = nullptr;
static PClass *SliderClass = nullptr;

static PField *FldCurrentArea = nullptr;
static PField *FldCurrentItem = nullptr;
static PField *FldMailScroll = nullptr;
static PField *FldScrollbar = nullptr;
static PField *FldSliderValue = nullptr;
static PField *FldSliderMin = nullptr;
static PField *FldSliderMax = nullptr;

static VMFunction *FuncOpenEntry = nullptr;
static VMFunction *FuncSetNormalized = nullptr;
static VMFunction *FuncUpdateScrollbar = nullptr;
static int OpenEntryRegs = 0;
static int SetNormalizedRegs = 0;
static int UpdateScrollbarRegs = 0;

// ---------------------------------------------------------------------------------------------
// THE STATE ITSELF. Three pieces, each with its own validity flag.
// ---------------------------------------------------------------------------------------------
//
// SEPARATE FLAGS RATHER THAN SENTINEL VALUES, because every value these can hold is a legitimate one: area
// 0 is a real area, and a scroll fraction of 0.0 is the top of the document, which is exactly the position
// a player who scrolled back up meant to be at. A sentinel would make "at the top" indistinguishable from
// "never sampled" and silently stop restoring the one case that is easiest to get wrong.
static PClass *StateAppClass = nullptr;

static bool StateEntryValid = false;
static int StateArea = 0;
static int StateItem = 0;

static bool StateScrollValid = false;
static double StateScroll = 0.0;

static unsigned StateGeneration = 0;

// Advance the apply trigger. Skips 0 on wrap so that the caller's "nothing sampled yet" test can stay a
// single compare against 0 rather than a second flag; a wrap needs four billion changes and will not
// happen, which is precisely why it is cheaper to make it impossible than to reason about it.
static void BumpGeneration()
{
	if (++StateGeneration == 0)
		StateGeneration = 1;
}

// Resolve everything both directions need. Runs once, lazily, on whichever of sampling or applying happens
// first - never at startup, because PClass::FindClass cannot answer before the scripts are compiled and a
// premature lookup would latch this off forever on a game that does have the classes.
static bool ViewStateResolve()
{
	PClass *readerCls = PClass::FindClass(ReaderClassName);
	PClass *scrollCls = PClass::FindClass(ScrollClassName);
	PClass *sliderCls = PClass::FindClass(SliderClassName);
	if (readerCls == nullptr || scrollCls == nullptr || sliderCls == nullptr)
	{
		Printf(TEXTCOLOR_YELLOW "AuxCodexView: no %s/%s/%s class, the standby codex will not keep the "
			"player's place in a datalog\n", ReaderClassName, ScrollClassName, SliderClassName);
		ViewStateBroken = true;
		return false;
	}

	// openEntry(int area, int item, bool immediateRead = false) - reader.zs:971 - is declared PRIVATE, and it
	// is reachable for exactly the same reason PDAMenu3.switchToAppWindow (pda_menu.zs:718) and
	// UIControl.disabled (view.zs:1356) already are in i_auxstandbycodex.cpp: private and protected are
	// COMPILE-TIME checks in ZScript and nothing about the symbol itself changes. Every use of VARF_Private
	// and VARF_Protected in this engine is in the compiler front end or the code generator
	// (zcc_compile.cpp:1500,2449,2866, codegen.cpp:6938,6949,8390,8395, zcc_compile_doom.cpp:969), in the
	// MENUDEF parser (menudef.cpp:462,1065,1353) or in the DECORATE user-variable writers
	// (p_actionfunctions.cpp:3284,3333, maploader.cpp:1258) - there is NOT ONE reference in vmexec.cpp,
	// vmframe.cpp or vm.h, so the VM has no access check to fail. FindSymbol does not filter on it either,
	// and ResolveMethod does not test it, so this is validated exactly as strictly as every public symbol
	// here and no more loosely: VMFillParams walks the CALLEE's NumArgs regardless of how it was declared.
	//
	// Three declared arguments and four registers with self. The default on immediateRead does not change
	// NumArgs - the caller fills omitted arguments in, so the callee still declares and reads four.
	static const EArgKind OpenEntryArgs[] = { Arg_Int, Arg_Int, Arg_Bool };
	static const EArgKind SetNormalizedArgs[] = { Arg_Float, Arg_Bool };
	static const EArgKind UpdateScrollbarArgs[] = { Arg_Float };

	// The subsystem prefix is this file's own, so a failure does not announce itself as the standby codex and
	// does not tell the player the second-screen view is off - it is not, only the place-keeping is.
	static const char *const Subsystem = "AuxCodexView";
	static const char *const Note = "the standby codex will not keep the player's place in a datalog";

	int openEntryRegs = 0, setNormalizedRegs = 0, updateScrollbarRegs = 0;

	VMFunction *funcOpenEntry = ResolveMethod(readerCls, "openEntry", Subsystem, OpenEntryArgs, 3,
		&openEntryRegs, nullptr, Note);

	// setNormalizedValue(double t, bool moveButt = false) - slider.zs:293. Resolved on UISlider because that
	// is EXACTLY what UIVerticalScroll.initFromConfig constructs the scrollbar as (vertical_scroll.zs:93), and
	// it is `virtual`, so the apply below requires the instance to be exactly this class rather than merely a
	// kind of it - a subclass override would be something this code has not read.
	VMFunction *funcSetNormalized = funcOpenEntry != nullptr
		? ResolveMethod(sliderCls, "setNormalizedValue", Subsystem, SetNormalizedArgs, 2, &setNormalizedRegs,
			nullptr, Note) : nullptr;

	// updateScrollbar(double scrollNormalized = -1) - vertical_scroll.zs:229 - is NON-VIRTUAL and declared
	// once in the whole tree, so resolving it on UIVerticalScroll is what runs for PDAMailScroll too. Same
	// argument, and the same reason, as i_auxstandbycodex.cpp resolving PDAAppWindow.close on the base.
	VMFunction *funcUpdateScrollbar = funcSetNormalized != nullptr
		? ResolveMethod(scrollCls, "updateScrollbar", Subsystem, UpdateScrollbarArgs, 1, &updateScrollbarRegs,
			nullptr, Note) : nullptr;

	// currentArea and currentItem are plain ints (reader.zs:330), which is the one field kind the resolvers
	// did not already have - see Field_Int in i_auxvmreflect.h. Every ResolveField below is handed this file's
	// own subsystem and note for the same reason ResolveMethod is: a missing field here loses the
	// place-keeping, not the second-screen view, and must not say otherwise.
	PField *fldArea = funcUpdateScrollbar != nullptr
		? ResolveField(readerCls, "currentArea", Field_Int, readerCls, Subsystem, Note) : nullptr;
	PField *fldItem = fldArea != nullptr
		? ResolveField(readerCls, "currentItem", Field_Int, readerCls, Subsystem, Note) : nullptr;

	// mailScroll (reader.zs:321), not listScroll. The mail pane is where the document the player is READING
	// lives, and it is the only one of the two whose position is not already implied by something else: the
	// entry list's scroll follows from which entry is selected, because navigateToActiveEmail scrolls the
	// list to it (reader.zs:1145-1146), so restoring the entry restores the list position for free.
	//
	// Proved to point at a UIVerticalScroll rather than merely at a UIView, which is tighter than
	// Field_ViewPtr's usual use and is what makes the IsKindOf on the instance below a proof.
	PField *fldMailScroll = fldItem != nullptr
		? ResolveField(readerCls, "mailScroll", Field_ViewPtr, scrollCls, Subsystem, Note) : nullptr;
	PField *fldScrollbar = fldMailScroll != nullptr
		? ResolveField(scrollCls, "scrollbar", Field_ViewPtr, sliderCls, Subsystem, Note) : nullptr;

	// The three doubles getNormalizedValue is made of (slider.zs:85-87, :285-287). Read as FIELDS rather than
	// through the getter because sampling must not call script at all - see SAMPLING IS READ-ONLY in the
	// header - and because all three are needed anyway to know the range is usable.
	PField *fldValue = fldScrollbar != nullptr
		? ResolveField(sliderCls, "value", Field_Float, sliderCls, Subsystem, Note) : nullptr;
	PField *fldMin = fldValue != nullptr
		? ResolveField(sliderCls, "minVal", Field_Float, sliderCls, Subsystem, Note) : nullptr;
	PField *fldMax = fldMin != nullptr
		? ResolveField(sliderCls, "maxVal", Field_Float, sliderCls, Subsystem, Note) : nullptr;

	if (fldMax == nullptr)
	{
		// ResolveMethod/ResolveField already printed which symbol failed; all that is left is to record it
		// against this file. Neither resolver latches anything of its own - see i_auxvmreflect.h.
		Printf(TEXTCOLOR_YELLOW "AuxCodexView: %s does not look the way this code was written against, the "
			"standby codex will not keep the player's place in a datalog\n", ReaderClassName);
		ViewStateBroken = true;
		return false;
	}

	ReaderClass = readerCls;
	ScrollClass = scrollCls;
	SliderClass = sliderCls;
	FldCurrentArea = fldArea;
	FldCurrentItem = fldItem;
	FldMailScroll = fldMailScroll;
	FldScrollbar = fldScrollbar;
	FldSliderValue = fldValue;
	FldSliderMin = fldMin;
	FldSliderMax = fldMax;
	FuncOpenEntry = funcOpenEntry;
	FuncSetNormalized = funcSetNormalized;
	FuncUpdateScrollbar = funcUpdateScrollbar;
	OpenEntryRegs = openEntryRegs;
	SetNormalizedRegs = setNormalizedRegs;
	UpdateScrollbarRegs = updateScrollbarRegs;
	ViewStateResolved = true;

	Printf("AuxCodexView: %s resolved, the standby codex will follow the live codex's entry and scroll\n",
		ReaderClassName);
	return true;
}

// The mail pane's scrollbar, or null if this window's chain is not what it was proved to be.
//
// EXACTLY UISlider FOR THE BAR, not merely a kind of it: setNormalizedValue is `virtual` (slider.zs:293) and
// a subclass could override it with something this code has not read. A KIND OF UIVerticalScroll for the
// scroll view, because mailScroll genuinely is a subclass - PDAMailScroll, reader.zs:257 - and
// updateScrollbar is non-virtual, so the base's is what runs for it.
static DObject *ReadMailScrollbar(DObject *window, DObject **outScroll)
{
	DObject *scroll = ReadObjectField(window, FldMailScroll);
	if (scroll == nullptr || !scroll->IsKindOf(ScrollClass))
		return nullptr;

	DObject *bar = ReadObjectField(scroll, FldScrollbar);
	if (bar == nullptr || bar->GetClass() != SliderClass)
		return nullptr;

	if (outScroll != nullptr)
		*outScroll = scroll;
	return bar;
}

// getNormalizedValue's own arithmetic (slider.zs:285-287) in C++, clamped to a usable 0..1.
//
// THE CLAMP IS NOT TIDINESS, IT IS WHAT STOPS A NaN COSTING A READBACK EVERY FRAME. An empty range makes
// this 0/0, and the caller compares the result against the stored value to decide whether anything changed -
// NaN != NaN is true, so a stored NaN would be a change on every single frame, a generation bump on every
// frame, and therefore a ~30 ms canvas readback on every frame (see i_auxpanel.cpp). The `!(x >= 0.0)` form
// is what catches NaN as well as a negative, the same shape and for the same reason as CodexZoom's.
static bool ReadScrollFraction(DObject *window, double *outFraction)
{
	DObject *bar = ReadMailScrollbar(window, nullptr);
	if (bar == nullptr)
		return false;

	const uint8_t *base = (const uint8_t *)bar;
	const double value = *(const double *)(base + FldSliderValue->Offset);
	const double minVal = *(const double *)(base + FldSliderMin->Offset);
	const double maxVal = *(const double *)(base + FldSliderMax->Offset);

	if (!(maxVal > minVal))
		return false;

	double fraction = (value - minVal) / (maxVal - minVal);
	if (!(fraction >= 0.0))
		fraction = 0.0;
	else if (fraction > 1.0)
		fraction = 1.0;

	*outFraction = fraction;
	return true;
}

// The entry the live reader has open, or false if it has not opened one yet.
//
// BOTH BOUNDS ARE SELACO'S OWN TESTS, NOT ONES INVENTED HERE. buildEmailList populates the entry list with
// `for(int y = 1; y < PDAEntry.NUM_ENTRY; y++)` (reader.zs:773), so items are ONE-BASED, and
// getEntryContent returns "" for item 0 (reader.zs:1352) - so item 0 is the "nothing open" state and applying
// it would blank the mail pane and throw away the scroll for nothing. getActiveControl guards on
// `currentArea >= 0 && currentItem >= 0` (reader.zs:1163), which is where the area test comes from.
//
// THE UPPER BOUNDS ARE GUARANTEED BY PROVENANCE RATHER THAN CHECKED, and that is worth being explicit about
// because openEntry is not bounds-safe: it reaches getEntryStatus, which does `entry.entries[area][item]`
// (reader.zs:1384) on a fixed PDAEntryInfo[NUM_AREA][NUM_ENTRY] (pda.zs:28), so an out-of-range pair is a VM
// abort. NUM_AREA and NUM_ENTRY cannot be read back at runtime - they are file-scope ZScript enum constants
// and RemoveUnusedSymbols strips them, the same finding the PDA_APP_ID table in i_auxstandbycodex.cpp
// records - but every value that reaches here came out of the LIVE reader's currentArea/currentItem, which
// that same bounded loop is the only thing that writes. The caller's try/catch is the backstop if that ever
// stops being true.
static bool ReadEntry(DObject *window, int *outArea, int *outItem)
{
	const int area = *(const int *)((const uint8_t *)window + FldCurrentArea->Offset);
	const int item = *(const int *)((const uint8_t *)window + FldCurrentItem->Offset);

	if (area < 0 || item <= 0)
		return false;

	*outArea = area;
	*outItem = item;
	return true;
}

void SampleAppWindow(DObject *appWindow)
{
	if (appWindow == nullptr)
		return;

	// THE APP, and the one piece that is sampled for EVERY window rather than only for the reader. Its class
	// is what is stored, because the class is what i_auxstandbycodex.cpp's StandbyApps table keys on and
	// because a class pointer stays valid for the life of the process while the instance does not.
	//
	// Outside the resolve gate on purpose: this needs none of the reader symbols, so a game whose
	// PDAReaderWindow does not look the way this file expects still gets the app mirroring it had before the
	// entry and the scroll were added to it. Losing all three because one failed would be a regression.
	StateAppClass = appWindow->GetClass();

	if (ViewStateBroken)
		return;

	if (!ViewStateResolved && !ViewStateResolve())
		return;

	// Not the reader, so there is no reading position to take. The last one STANDS rather than being
	// invalidated: switching to OBJECTIVES says nothing about which datalog the player was in, and they may
	// well switch back before they close the PDA.
	if (!appWindow->IsKindOf(ReaderClass))
		return;

	bool changed = false;

	int area = 0, item = 0;
	if (ReadEntry(appWindow, &area, &item) && (!StateEntryValid || area != StateArea || item != StateItem))
	{
		StateEntryValid = true;
		StateArea = area;
		StateItem = item;
		changed = true;
	}

	double fraction = 0.0;
	if (ReadScrollFraction(appWindow, &fraction) && (!StateScrollValid || fraction != StateScroll))
	{
		StateScrollValid = true;
		StateScroll = fraction;
		changed = true;
	}

	if (changed)
		BumpGeneration();
}

PClass *WantedAppClass()
{
	return StateAppClass;
}

unsigned Generation()
{
	return StateGeneration;
}

// Put the scroll fraction back. The caller owns the projection scope and the try/catch.
//
// TWO CALLS IN THIS ORDER, AND updateScrollbar ALONE IS NOT ENOUGH - which is the one thing here that had to
// be read out of the script rather than assumed, because it is the difference between a restore that holds
// and one that is silently undone before the player sees it.
//
//   updateScrollbar(fraction) moves the CONTENT: it writes layoutTopPin.offset and mLayout.frame.pos.y from
//   the fraction (vertical_scroll.zs:276-279). It does NOT write scrollbar.value. So on a tree that is
//   already sized this is what makes the restore visible immediately, on this frame's draw.
//
//   setNormalizedValue(fraction) LAST, because the value is what the next layout pass re-reads and therefore
//   what has to be the last word. UIVerticalScroll.layout takes `scroll = scrollbar.getNormalizedValue()`,
//   lays out, then calls updateScrollbar(scroll) (vertical_scroll.zs:290-297), and onAdjustedPostLayout does
//   the same (:299). With the bar still at 0 that pass would snap the content straight back to the top - and
//   the pass is GUARANTEED, because openEntry sets mailScroll.requiresLayout (reader.zs:1019) and
//   UIView.drawSubviews calls layoutIfNecessary on every subview it walks (view.zs:464) all the way down.
//   Putting the value in last is also what survives updateScrollbar's own `scrollbar.value = 0` on a tree
//   that is NOT sized yet (:252), which is exactly the state a freshly constructed reader window is in.
//
// Between them they are what Selaco's own scrollNormalized(val, animated: false, sendEvt: false) does
// (vertical_scroll.zs:310-320) minus the event. scrollNormalized is deliberately not called instead: it is
// `virtual` and PDAMailScroll overrides it with a declared arity one argument short of the base's
// (reader.zs:271 against :310), so which register count the callee actually wants is a question these two
// unambiguous, singly-declared functions do not raise. Not sending the event also matters on its own - it
// would route back into PDAReaderWindow.handleSubControl on a menu nobody is driving.
//
// moveButt false. updateScrollbar ends in scrollbar.layoutButton() (vertical_scroll.zs:287), so the thumb
// graphic is already moved, and UISlider.draw catches a stale one anyway (slider.zs:319-323).
static bool ApplyScroll(DObject *window, double fraction)
{
	DObject *scroll = nullptr;
	DObject *bar = ReadMailScrollbar(window, &scroll);
	if (bar == nullptr)
		return false;

	{
		VMValue params[] = { scroll, fraction };
		VMCall(FuncUpdateScrollbar, params, UpdateScrollbarRegs, nullptr, 0);
	}

	{
		VMValue params[] = { bar, fraction, (int)0 };
		VMCall(FuncSetNormalized, params, SetNormalizedRegs, nullptr, 0);
	}

	return true;
}

bool ApplyToWindow(DObject *window)
{
	if (ViewStateBroken || window == nullptr)
		return false;

	if (!ViewStateResolved && !ViewStateResolve())
		return false;

	// The standby codex is showing something other than the reader, which is not a failure - the player is
	// entitled to close their PDA on STATS. Answered quietly, because the caller asks on every generation
	// change and a line here would be one per scroll of a datalog the panel is not showing.
	if (!window->IsKindOf(ReaderClass))
		return false;

	if (!StateEntryValid && !StateScrollValid)
		return false;

	bool entryDone = false;

	if (StateEntryValid)
	{
		// immediateRead FALSE, and that is the whole of what keeps this read-only. It is the ONLY branch in
		// openEntry that records anything: `if(immediateRead) SendNetworkEvent("pdaEntrySet", area, item,
		// STATUS_LAST_OPEN)` at reader.zs:1050. The caller's FProjectionScope would refuse that write anyway,
		// which is why the scope is required rather than optional here, but passing false means the panel never
		// even ASKS to mark a datalog read merely because it is mirroring one - the player marks their own.
		VMValue params[] = { window, StateArea, StateItem, (int)0 };
		VMCall(FuncOpenEntry, params, OpenEntryRegs, nullptr, 0);
		entryDone = true;
	}

	// AFTER openEntry, NEVER BEFORE, and this is an ordering requirement rather than a preference: openEntry
	// resets the mail pane to the top itself - scrollbar.setNormalizedValue(0) then scrollNormalized(0)
	// (reader.zs:1017-1018) - so a scroll installed first would be thrown away by the very next line.
	const bool scrollDone = StateScrollValid && ApplyScroll(window, StateScroll);

	if (!entryDone && !scrollDone)
		return false;

	// ONE LINE PER APPLY, naming all three pieces, because on a device "the panel did not keep my place" and
	// "the panel restored a place that happened to be the top" are otherwise the same observation. Each piece
	// says so when it could not be restored rather than being omitted, so a partial restore is legible as one.
	//
	// The parenthesised token is a REVISION SENTINEL, deliberately unique to this change so it can be grepped
	// out of the built .so to prove which revision is actually packaged - a size match has twice passed
	// against a stale APK on this project.
	FString entryNote, scrollNote;
	if (entryDone)
		entryNote.Format("entry area %d item %d", StateArea, StateItem);
	else
		entryNote = "entry not restored";

	if (scrollDone)
		scrollNote.Format("scroll %.1f%%", StateScroll * 100.0);
	else
		scrollNote = "scroll not restored";

	Printf("AuxCodexView: standby codex restored to %s, %s, %s (aux-viewstate r1)\n",
		window->GetClass()->TypeName.GetChars(), entryNote.GetChars(), scrollNote.GetChars());

	return true;
}

}   // namespace AuxCodexView

// See I_AuxForgetScriptState in i_auxvmreflect.cpp for the full reasoning. Every line here is a store to a
// file static: no VMCall, no Destroy(), no sound and nothing that can throw, which is the whole contract.
//
// THE STATE IS CLEARED ALONGSIDE THE RESOLVES, unlike the in-session behaviour where "the last app this
// session" is never forgotten. A restart can bring up an entirely different wad set, so both the app class -
// a PClass* that StaticShutdown is about to free, which is the one place its "never cleared" argument stops
// holding - and the entry and scroll are about a game that is no longer running.
void I_AuxCodexViewStateForgetScriptState()
{
	AuxCodexView::ViewStateBroken = false;
	AuxCodexView::ViewStateResolved = false;

	AuxCodexView::ReaderClass = nullptr;
	AuxCodexView::ScrollClass = nullptr;
	AuxCodexView::SliderClass = nullptr;

	// The five raw byte offsets this file reads through, which are the widest hazard in it: left set, the
	// first post-restart sample would read arbitrary object bytes out of freed PFields rather than merely
	// miss a call.
	AuxCodexView::FldCurrentArea = nullptr;
	AuxCodexView::FldCurrentItem = nullptr;
	AuxCodexView::FldMailScroll = nullptr;
	AuxCodexView::FldScrollbar = nullptr;
	AuxCodexView::FldSliderValue = nullptr;
	AuxCodexView::FldSliderMin = nullptr;
	AuxCodexView::FldSliderMax = nullptr;

	AuxCodexView::FuncOpenEntry = nullptr;
	AuxCodexView::FuncSetNormalized = nullptr;
	AuxCodexView::FuncUpdateScrollbar = nullptr;
	AuxCodexView::OpenEntryRegs = 0;
	AuxCodexView::SetNormalizedRegs = 0;
	AuxCodexView::UpdateScrollbarRegs = 0;

	AuxCodexView::StateAppClass = nullptr;
	AuxCodexView::StateEntryValid = false;
	AuxCodexView::StateArea = 0;
	AuxCodexView::StateItem = 0;
	AuxCodexView::StateScrollValid = false;
	AuxCodexView::StateScroll = 0.0;

	// Back to the "nothing sampled yet" value, so the first post-restart update does not see a stale
	// generation and try to apply state belonging to the previous session's savegame.
	AuxCodexView::StateGeneration = 0;
}
