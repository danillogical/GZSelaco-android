/*
** i_auxstandbycodex.cpp
** Draws Selaco's whole PDA desktop onto the AUXCANVAS offscreen canvas, non-modally.
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
** WHY THIS IS C++ AND NOT ZSCRIPT, which is the whole reason the file exists.
**
** Selaco's UI tree is rooted at `class UIView ui { ... }` - no engine base class at all. Every
** entry point we need (add, draw, drawSubviews, layout) is virtual, so dispatching to one requires
** a compile-time static type, and there is no type in gzdoom.pk3 that can supply it. A static
** reference to UIView from an always-loaded lump is a FATAL script error on Doom. Gating the lump
** on the IWAD does not help either: autoname/LumpFilterIWAD is "Selaco" for BOTH the demo and the
** full game (byte-identical iwadinfo.txt), and the demo's UIView has no setCanvas at all, so the
** gate would only move the fatal error from Doom to the demo.
**
** VM reflection has no compile step to fail. PClass::FindClass returns nullptr on Doom and on the
** demo alike, we latch off, and nothing is ever tried again. Failing soft is a property of the
** mechanism here, not a check we remembered to write.
**
** Every symbol reached below is proved before it is called, by the resolvers in i_auxvmreflect.cpp.
** Why that validation is the safety argument rather than defensive padding is written up in
** i_auxvmreflect.h.
**
** ---------------------------------------------------------------------------------------------
** MODE 3: THE WHOLE PDA DESKTOP (PDAMenu3), the standby codex.
**
** THE STANDBY CODEX CANNOT DODGE THE PLAYER-STATE WRITES BY CONSTRUCTION, so the engine has to refuse them.
** Its app windows are necessarily parented to desktopView, and PDAReaderWindow.init ends with
** EventHandler.SendNetworkEvent("pdaUnreadClear") (reader.zs:538) with no branch that skips it - so
** merely CONSTRUCTING the desktop wipes the player's unread-datalog badges. Hence FProjectionScope
** (projectionscope.h): the engine refuses the write instead of us avoiding the call. Every VMCall below
** that can reach script that records something is inside one.
**
** THE SAME SCOPE ALSO REFUSES THE DESKTOP'S UI SOUNDS, which matters more in practice than the writes do
** because the player hears them. init() plays MenuSound("codex/open") (pda_menu.zs:90) and each app window
** the selection sweep closes plays Menu.MenuSound("codex/closeWindow") (app_window.zs:136), and this
** desktop is rebuilt on every stat change - so before that suppression the panel chirped on every secret
** found and every pickup, during gameplay, for a screen the player never asked for.
**
** Two more consequences follow from PDAMenu3 being a UIMenu rather than a UIView:
**
**   We draw its mainView, never the menu. UIMenu.drawSubviews (menu.zs:284-288) wraps the tree in
**   Screen.EnableStencil/ClearStencil, which would push stencil state into the MAIN screen's twod,
**   and it also calls animator.step()/testMouse(), which mutate the tree during a draw.
**   PDAMenu3.drawer() (pda_menu.zs:1168-1197) hardcodes Screen.GetWidth/setClipRect/DrawTexture.
**   mainView.draw() + mainView.drawSubviews() reaches the same widget tree with none of that.
**
**   It is built LATE, not at the title screen. PDAMenu3.init dereferences players[consoleplayer].mo
**   unguarded (pda_menu.zs:320,368,372,385,390), so it is all or nothing - there is no half-built state
**   to put up in the meantime. The preconditions are tested every frame and the whole thing is built on
**   the first frame they hold.
**
** Three field writes make it visible, and each is a suppression of something ticker() would otherwise
** have done for us. UIScrollBG.draw() is the only unguarded Screen.* draw site reachable from a
** UIView subtree in all of CockUI/ and pda2/, so background.hidden = true disposes of it - nulling
** the field does NOT work, because pda_menu.zs:110 puts it in innerView's subviews array and
** drawSubviews iterates that array (view.zs:462-488), not the menu's field. And innerView plus all
** six tabs are created at alpha 0 (pda_menu.zs:105,236,256,275,294,313,342) and animated up only in
** ticker() at :826-841, which we must not call (manual.zs:432-433 aside, ticking is what makes a
** projection stop being passive), so without those writes the entire desktop draws at alpha 0.
**
** AND ONE THING THE GAME'S OWN RELAYOUT GETS WRONG FOR US. layoutChange -> calcScale scales by HEIGHT
** ONLY (menu.zs:129, pda_menu.zs:791), which on a 1240x1080 panel makes the logical box 1240 wide
** against a design that assumes 1920 (innerView.maxSize.x, pda_menu.zs:103) - the tab bar loses
** characters off both ends. calcScale takes the baseline resolution as a third argument, so it is
** called directly with a baseline height derived from the canvas constants (DesktopBaselineHeight)
** rather than through layoutChange, and layoutChange's remaining two calls are made by hand. The panel
** is then 1920 x 1672.3 logical at scale 0.6458: full design width, extra vertical room, no clipping.
**
** AND THAT IS STILL TOO SMALL TO READ on a panel this size, which is a physical problem and not a layout
** one - so aux_codex_size divides that baseline height, deliberately trading the un-clipped layout for
** legibility, and it retunes the LIVE desktop without a rebuild. See the block above StandbyRetune for the
** arithmetic and for where the usable range really ends.
*/

#include <exception>
#include <math.h>        // fabs, for reporting which of calcScale's limits a zoom hit
#include <string.h>      // memcpy, for the stat-total hash

// dobjtype.h and dobjgc.h are not self-contained (they lean on FName, FString and the DObject
// declaration being in scope already), so dobject.h leads - it is the header that pulls that chain
// in, and every engine translation unit that touches PClass reaches it the same way.
#include "dobject.h"
#include "actor.h"       // AActor::FindInventory, for the Stats precondition
#include "c_cvars.h"
#include "d_player.h"    // players[], playeringame[]
#include "doomstat.h"    // gamestate
#include "dobjtype.h"
#include "i_music.h"     // I_SetMusicVolume, the native PDAMenu3.onDestroy reaches
#include "i_time.h"      // I_msTimeF, for the rebuild debounce
#include "menu.h"        // DMenu, the self type PDAMenu3.init is declared against
#include "menustate.h"   // menuactive, which PDAMenu3.init writes
#include "printf.h"
#include "projectionscope.h"
#include "s_music.h"     // relative_volume, snapshotted across the desktop's destroy
#include "symbols.h"
#include "types.h"
#include "v_2ddrawer.h"  // FCanvas and its F2DDrawer, which the desktop is drawn into
#include "vm.h"
#include "zstring.h"

#include "i_auxvmreflect.h"   // the resolvers, the panel's geometry and GetTextureCanvas

using namespace AuxView;

// Mode 3's root: the whole PDA desktop. A UIMenu subclass, not a UIView, which is why it is hosted
// through its mainView rather than directly - see MODE 3 in the header.
static const char *const StandbyClassName = "PDAMenu3";

// The tab that selects one of the desktop's apps (tabs.zs:1). Only mode 3's app selection needs it;
// see SelectStandbyApp.
static const char *const TabClassName = "PDATab";

// Mode 3's two latches, deliberately distinct.
//
// Absent is the EXPECTED outcome on Doom and on the demo: there is no such class, nothing is wrong,
// and it must be silent after one line. Broken means Selaco is here but does not look the way this
// code was written against, which is a real diagnostic. Neither is ever retried - a class that does
// not exist will not appear, and a signature that does not match will not change, so retrying would
// put a symbol lookup on the render path forever.
//
// The shared resolvers latch NEITHER of them, which is why they can be used by mode 4 and by the two
// optional groups here without any of those being able to disable the desktop. Every
// ResolveMethod/ResolveField failure that genuinely should disable it latches StandbyBroken at the
// call site instead; see THE RESOLVERS LATCH NOTHING in i_auxvmreflect.h.
static bool StandbyAbsent = false;
static bool StandbyBroken = false;

// ---------------------------------------------------------------------------------------------
// MODE 3: the whole PDA desktop. One construction, three field writes.
// ---------------------------------------------------------------------------------------------

// The live desktop and the view we actually draw. Rooted by the marker registered in
// BuildStandbyView; mainView is a field of the menu so the GC would reach it anyway, but it is marked
// explicitly so that does not have to be reasoned about.
static DObject *StandbyMenu = nullptr;
static DObject *StandbyRootView = nullptr;

// The codex generation the current desktop was built against.
static unsigned StandbyGeneration = 0;

static VMFunction *FuncStandbyDraw = nullptr;
static VMFunction *FuncStandbyDrawSubviews = nullptr;

// The AUXCANVAS handle, held here rather than shared with mode 4's so the two paths have no mutable
// state in common and a failure in one cannot disturb the other.
static FCanvas *StandbyCanvas = nullptr;

// Selaco's PDAMenu3.init writes the engine's menuactive global (pda_menu.zs:82; menuactive is
// menu.cpp:97, exported at :1059) because on the main screen it really is opening a menu. The engine
// itself would not eat our input - M_Responder gates on CurrentMenu != nullptr && menuactive !=
// MENU_Off (menu.cpp:663) and we are never CurrentMenu - but Selaco's own scripts read it unguarded
// and misbehave: the look-at prompt vanishes (hud/lookat.zs:100), the achievement queue stalls
// (global_stat_handler.zs:95,109,123), and the weapon wheel and input handler both branch on it
// (weapon_wheel.zs:136,606, InputHandler.zsc:130,228,282).
//
// RAII rather than a pair of assignments because a VM abort unwinds as a C++ exception straight past
// the restore, and a leaked MENU_OnNoPause would break the player's HUD for the rest of the session.
struct FMenuActiveKeeper
{
	EMenuState Saved = menuactive;
	~FMenuActiveKeeper() { menuactive = Saved; }
};

// Raise one view out of the alpha-0 state ticker() would have animated it out of. The IsKindOf is the
// assertion that matters: the field's DECLARED type was proved to be a UIView subclass when it was
// resolved, and this proves the object actually in it is one, so the UIView-relative offset is valid
// for this instance.
static bool SetViewAlpha(DObject *view, PClass *viewCls, const PField *alphaField, double value)
{
	if (view == nullptr || !view->IsKindOf(viewCls))
		return false;

	*(double *)((uint8_t *)view + alphaField->Offset) = value;
	return true;
}

// ---------------------------------------------------------------------------------------------
// MODE 3: WHICH APP THE STANDBY CODEX SHOWS - it follows the player's real PDA, and falls back to a cvar.
// ---------------------------------------------------------------------------------------------
//
// WHAT THE PANEL IS FOR. The standby codex is a read-only view of the same PDA the player opens on the second
// screen in mode 4, so when they close the real PDA on OBJECTIVES the panel should be showing OBJECTIVES.
// The two are meant to agree.
//
// WHAT WAS ACTUALLY WRONG, and it is not the coupling. PDAMenu3.init restores the open-app set from the
// savegame (pda_menu.zs:408-471) - every PDAEntry.appSettings[x] with order > 0 - and then :465-470 picks
// which one is current by SORT ORDER:
//
//     PDAAppWindow topApp = apps.size() > 0 ? PDAAppWindow(apps[apps.size() - 1]) : null;
//
// Sort order is the desktop's z-stacking, not "the app the player was last looking at". So the standby codex
// WAS coupled to the real PDA, through the savegame, but to the wrong property of it - and on top of that
// the restore trusts `order > 0` without ever asking whether the app is UNLOCKED, which is how a save with
// no invasion tiers ended up displaying INVASION TIERS. That last part is the real defect.
//
// SO THIS DOES THREE THINGS, in precedence order:
//
//   1. Follow the live codex. Mode 4 samples PDAMenu3.currentAppWindow every frame the player's own PDA is
//      open (see LiveSampleCurrentApp in i_auxlivecodex.cpp) and leaves its CLASS in
//      AuxView::LiveLastAppClass(). That is the app the player
//      last switched to, because switchToAppWindow (pda_menu.zs:735) is the single funnel every switch goes
//      through - the six openX() helpers, the tab handler at :880-900, and the click-to-raise at :961.
//
//   2. Otherwise aux_standby_app, defaulting to Datalogs. This is the INITIAL value - what the panel
//      shows before the player has opened their real PDA at all this session.
//
//   3. Availability applies to whichever of those wins. A locked app is never displayed; the first
//      available entry in StandbyApps order is substituted and the substitution is logged.
//
// AND IT IS APPLIED WITHOUT A REBUILD. The standby codex's PDAMenu3 is a SEPARATE INSTANCE from the engine's
// CurrentMenu, and it is still alive and already built when the player closes theirs - so switching the
// panel to the app they closed on is a switchToAppWindow plus one redraw, not a fresh init(). See the
// re-select block in StandbyViewUpdate.
//
// THE FOURTH THING MODE 3 INHERITS FROM init() WHETHER IT WANTS IT OR NOT, after the alpha-0 views, the
// `usingGP` local and the frozen stat totals, and they all have one cause: the real PDA is opened, read and
// thrown away in seconds, so nothing in it was written to be re-decided from outside.
//
// READ-ONLY, AND THAT IS NOT OPTIONAL. Nothing below writes PDAEntry.appSettings or any other player
// state. What is overridden is what the standby codex DISPLAYS after init() has restored it, never what was
// restored FROM: FProjectionScope stops the netevent path, but a direct field write through reflection
// would go straight past it and corrupt the layout of the player's real PDA.


// The PDA_APP_ID values (pda_menu.zs:16-27), which is what aux_standby_app is set to.
//
// HARDCODED BECAUSE THE ENUM IS NOT THERE TO ASK, and this was checked rather than assumed. PDA_APP_ID is
// a file-scope ZScript enum, so its members are PSymbolConstNumeric in a PNamespace's symbol table - and
// d_main.cpp:3744 calls RemoveUnusedSymbols(), whose first act is Namespaces.RemoveSymbols()
// (symbols.cpp:562-571): it empties every namespace symbol table, and then strips everything that is not
// a PField, PFunction or PPropFlag out of the class tables too (:579-600). That runs during startup, long
// before mode 3 can build anything, so by the time this code runs there is no PDA_APP_READER to look up.
// Fields and methods survive it, which is exactly why everything else in this file is reachable by
// reflection and these constants are not.
//
// What IS validated is the pairing: StandbyApps names the window class and the tab that each number is
// supposed to mean, and both are resolved and type-checked. A renumbered enum would therefore show up as
// the wrong app being displayed rather than as a crash - and note the numbers are not free to move
// anyway, because PDAEntry.appSettings[] is indexed by appID and lives in the savegame (pda.zs:29,
// :173-177). That is the same save-compatibility constraint stats.zs:297 spells out for StatType, except
// that here nothing in Selaco's source writes it down.
static const int PdaAppReader     = 1;   // PDA_APP_READER
static const int PdaAppStats      = 2;   // PDA_APP_STATS
static const int PdaAppInvasion   = 4;   // PDA_APP_INVASION
static const int PdaAppObjectives = 6;   // PDA_APP_OBJECTIVES
static const int PdaAppChallenges = 7;   // PDA_APP_CHALLENGES
static const int PdaAppManual     = 8;   // PDA_APP_MANUAL

// Which app the standby codex shows BEFORE the player has opened their own PDA this session, and the fallback
// whenever the live codex has not told us anything usable.
//
// DEFAULT PDA_APP_READER - Datalogs, not the Manual. The panel is glanceable rather than read, and
// datalogs are the thing that changes as the player plays; the manual is static reference text that the
// player goes looking for deliberately, which is what mode 4 is for.
//
// Flags 0, not CVAR_ARCHIVE, the same reasoning as aux_codex_mode: an archived
// version of this would make the panel's starting contents depend on whatever the last profile set.
CVAR(Int, aux_standby_app, PdaAppReader, 0)

// The apps the standby codex can show, in FALLBACK PREFERENCE ORDER - if the requested one is locked, the
// first available entry from the top wins.
//
// The six with a tab in the strip, and only those. PDA_APP_MAP (3) is excluded because APP_CLASSES[3] is
// null (pda_menu.zs:65) - it has no window class at all, and init() would abort on `new(null)` if a save
// ever had it open - while PDA_APP_BORGIR and PDA_APP_LEVELINFO are transient tool windows with no tab,
// so there is no availability flag to read for them.
struct FStandbyApp
{
	int AppID;                  // the PDA_APP_ID value aux_standby_app is compared against
	const char *EnumName;       // the constant that value IS, for the log line and for grep
	const char *ClassName;      // PDAMenu3.APP_CLASSES[AppID] (pda_menu.zs:61-72)
	const char *TabFieldName;   // the PDATab whose disabled flag IS this app's availability
};

static const FStandbyApp StandbyApps[] =
{
	{ PdaAppReader,     "PDA_APP_READER",     "PDAReaderWindow",     "readerButt" },
	{ PdaAppObjectives, "PDA_APP_OBJECTIVES", "ObjectivesWindow",    "objectivesButt" },
	{ PdaAppStats,      "PDA_APP_STATS",      "StatReaderWindow",    "statsButt" },
	{ PdaAppChallenges, "PDA_APP_CHALLENGES", "PDAChallengesWindow", "mapButt" },
	{ PdaAppInvasion,   "PDA_APP_INVASION",   "PDAInvasionWindow",   "tiersButt" },
	{ PdaAppManual,     "PDA_APP_MANUAL",     "PDAManualWindow",     "manualButt" },
};

// A THIRD latch, with the same split of meaning as the others but a much softer failure: on a broken
// selection the desktop is left showing exactly what init() restored, which is the behaviour that
// shipped. That is why no ResolveMethod/ResolveField below latches StandbyBroken - losing the app choice
// must not lose the desktop.
static bool AppSelectResolved = false;
static bool AppSelectBroken = false;

// Resolved ONCE for the life of the process, unlike everything else mode 3 resolves. None of it can
// change between rebuilds, and the stats trigger rebuilds often, so re-resolving per build would put two
// dozen FindSymbols on a path that already re-runs the whole of PDAMenu3.init.
static PClass *AppWindowClass = nullptr;
static PClass *AppTabClass = nullptr;
static PField *FldStandbyView = nullptr;
static PField *FldViewParentMenu = nullptr;
static PField *FldTabDisabled = nullptr;
static VMFunction *FuncNumSubviews = nullptr;
static VMFunction *FuncViewAt = nullptr;
static VMFunction *FuncViewAdd = nullptr;
static VMFunction *FuncAppClose = nullptr;
static VMFunction *FuncSwitchToApp = nullptr;
static VMFunction *FuncTabSetSelected = nullptr;
static int ViewAtRegs = 0;
static int ViewAddRegs = 0;
static int SwitchToAppRegs = 0;
static int SetSelectedRegs = 0;

static PClass *AppClasses[countof(StandbyApps)] = {};
static VMFunction *AppVInit[countof(StandbyApps)] = {};
static int AppVInitRegs[countof(StandbyApps)] = {};
static PField *AppTabFields[countof(StandbyApps)] = {};

// The menu and view classes the selection was resolved against, cached so the re-select path in
// StandbyViewUpdate can run without looking them up again or being handed them by its caller.
static PClass *AppMenuClass = nullptr;
static PClass *AppViewClass = nullptr;

// THE ONE DELIBERATE CHANNEL BETWEEN MODE 4 AND MODE 3 is AuxView::LiveLastAppClass(), read by
// StandbyWantedIndex below. The storage, the accessor pair and the whole argument for why it is
// harmless in both directions are in i_auxvmreflect.h.

// The index into StandbyApps the current selection was COMPUTED FROM, and the one it settled on.
//
// The first is what the re-select trigger compares against, and it has to be the WANTED index rather than
// the shown one: a locked request is substituted, so comparing the wanted app against what is displayed
// would differ forever and re-select on every single frame. -1 means nothing has been selected yet.
static int StandbyAppWantedIndex = -1;
static int StandbyAppShownIndex = -1;

// Where a wanted app came from, for the log line only.
enum EAppSource
{
	AppSource_LiveCodex,
	AppSource_Cvar,
	AppSource_CvarInvalid,
};

// The app the standby codex SHOULD show, before availability is considered. Silent and allocation-free: it is
// called every frame by the re-select trigger, so it must not log and must not look anything up.
//
// Returns an index into StandbyApps, never negative - an unrecognised cvar value resolves to entry 0 and
// says so through outSource, which is what makes the "not selectable" warning fire once per selection
// rather than once per frame.
static int StandbyWantedIndex(EAppSource *outSource)
{
	// 1. THE LIVE CODEX. Matched by class rather than by PDAMenu3.currentApp, and that is not a stylistic
	//    choice - see LiveSampleCurrentApp (i_auxlivecodex.cpp) for why currentApp cannot be used.
	PClass *const lastApp = LiveLastAppClass();
	if (lastApp != nullptr)
	{
		for (unsigned i = 0; i < countof(StandbyApps); i++)
		{
			if (AppClasses[i] == lastApp)
			{
				if (outSource != nullptr)
					*outSource = AppSource_LiveCodex;
				return (int)i;
			}
		}

		// A BBWindow or a LevelInfoWindow: real PDAAppWindows the player can have been looking at, but with
		// no tab and therefore no availability flag, so they are not selectable and do not count as a
		// choice. Falls through to the cvar rather than being treated as an error.
	}

	// 2. THE CVAR, which is the starting value until the player opens their own PDA.
	const int wanted = aux_standby_app;
	for (unsigned i = 0; i < countof(StandbyApps); i++)
	{
		if (StandbyApps[i].AppID == wanted)
		{
			if (outSource != nullptr)
				*outSource = AppSource_Cvar;
			return (int)i;
		}
	}

	if (outSource != nullptr)
		*outSource = AppSource_CvarInvalid;
	return 0;
}

// Rooted only between CreateNew and desktopView.add. vInit allocates dozens of DObjects and any of those
// allocations can run a GC step, so a window that is not yet in anyone's subviews array would be white,
// unreferenced and swept part way through its own construction - the same hazard, and the same fix, as
// the menu itself. Marked by the marker function BuildStandbyView registers.
static DObject *StandbyPendingApp = nullptr;

static bool AppSelectResolve(PClass *menuCls, PClass *viewCls)
{
	PClass *appCls = PClass::FindClass(AppWindowClassName);
	PClass *tabCls = PClass::FindClass(TabClassName);
	const bool classesOk = appCls != nullptr && tabCls != nullptr
		&& appCls->IsDescendantOf(viewCls) && tabCls->IsDescendantOf(viewCls);

	static const EArgKind IntArg[] = { Arg_Int };
	static const EArgKind ObjArg[] = { Arg_ObjectOf };
	static const EArgKind BoolBoolArgs[] = { Arg_Bool, Arg_Bool };
	static const EArgKind VInitArgs[] = { Arg_Vector2, Arg_Vector2 };

	int numRegs = 0, closeRegs = 0;

	// numSubviews() and viewAt(int) are the game's OWN accessors for desktopView.subviews (view.zs:993,
	// :997), used in preference to walking the TArray from C++ so that nothing here depends on its layout
	// - and viewAt does a raw `subviews[idx]`, so every call below is bounded by numSubviews().
	VMFunction *funcNum = classesOk ? ResolveMethod(viewCls, "numSubviews", "AuxStandbyCodex", nullptr, 0, &numRegs) : nullptr;
	VMFunction *funcAt = funcNum != nullptr ? ResolveMethod(viewCls, "viewAt", "AuxStandbyCodex", IntArg, 1, &ViewAtRegs) : nullptr;
	// add(UIView) is virtual (view.zs:936); desktopView is proved to be EXACTLY UIView before it is used
	// as self, the same argument the file already makes for mainView.
	VMFunction *funcAdd = funcAt != nullptr
		? ResolveMethod(viewCls, "add", "AuxStandbyCodex", ObjArg, 1, &ViewAddRegs, viewCls) : nullptr;
	// close() is PDAAppWindow's own, takes no arguments, and is non-virtual and unique in the whole tree
	// (app_window.zs:139) - so resolving it on the base is what runs for every subclass.
	VMFunction *funcClose = funcAdd != nullptr ? ResolveMethod(appCls, "close", "AuxStandbyCodex", nullptr, 0, &closeRegs) : nullptr;
	// switchToAppWindow is declared `private` (pda_menu.zs:718). private and protected are COMPILE-TIME
	// checks in ZScript - the compiler refuses the access at parse time and nothing about the symbol
	// itself changes - so PClass::FindSymbol still returns it and VMCall still invokes it. Validated
	// exactly as strictly as every public symbol here, because VMFillParams does not care how it was
	// declared: it walks the CALLEE's NumArgs regardless.
	VMFunction *funcSwitch = funcClose != nullptr
		? ResolveMethod(menuCls, "switchToAppWindow", "AuxStandbyCodex", ObjArg, 1, &SwitchToAppRegs, appCls) : nullptr;
	// setSelected(bool s = true, bool sound = true) - button.zs:743. Both optional, so it is two declared
	// arguments and three registers with self.
	VMFunction *funcSel = funcSwitch != nullptr
		? ResolveMethod(tabCls, "setSelected", "AuxStandbyCodex", BoolBoolArgs, 2, &SetSelectedRegs) : nullptr;

	PField *fldStandby = funcSel != nullptr ? ResolveField(menuCls, "desktopView", Field_ViewPtr, viewCls) : nullptr;
	// parentMenu takes the MENU class as the expected pointee ancestor, not viewCls - see Field_MenuPtr.
	PField *fldParentMenu = fldStandby != nullptr
		? ResolveField(viewCls, "parentMenu", Field_MenuPtr, menuCls) : nullptr;
	// UIControl.disabled (view.zs:1356) is `protected`, reachable for exactly the same reason
	// switchToAppWindow is. This is the availability flag; see SelectStandbyApp.
	PField *fldDisabled = fldParentMenu != nullptr ? ResolveField(tabCls, "disabled", Field_Bool, viewCls) : nullptr;

	bool ok = fldDisabled != nullptr;

	for (unsigned i = 0; ok && i < countof(StandbyApps); i++)
	{
		PClass *cls = PClass::FindClass(StandbyApps[i].ClassName);
		if (cls == nullptr || !cls->IsDescendantOf(appCls) || cls->bAbstract || cls->ConstructNative == nullptr)
		{
			// CreateNew calls I_Error on the last two rather than returning null (dobjtype.cpp:433-437).
			Printf(TEXTCOLOR_YELLOW "AuxStandbyCodex: %s is not an instantiable %s\n",
				StandbyApps[i].ClassName, AppWindowClassName);
			ok = false;
			break;
		}

		AppClasses[i] = cls;
		// vInit(Vector2, Vector2) is the virtual constructor init() itself uses (app_window.zs:14,
		// pda_menu.zs:419), so a subclass override is what runs - ResolveMethod dispatches through the
		// instance's own vtable for exactly that reason.
		AppVInit[i] = ResolveMethod(cls, "vInit", "AuxStandbyCodex", VInitArgs, 2, &AppVInitRegs[i]);
		// The tab is expected to be a PDATab, which is what the disabled read below assumes.
		AppTabFields[i] = AppVInit[i] != nullptr
			? ResolveField(menuCls, StandbyApps[i].TabFieldName, Field_ViewPtr, tabCls) : nullptr;
		if (AppTabFields[i] == nullptr)
		{
			ok = false;
			break;
		}
	}

	if (!ok)
	{
		// ResolveMethod/ResolveField already printed which symbol failed; all that is left is to record it
		// against the app selection. Neither resolver latches anything of its own, so nothing here has to
		// be undone - see i_auxvmreflect.h.
		Printf(TEXTCOLOR_YELLOW "AuxStandbyCodex: %s does not expose the app switch this was written "
			"against - the standby codex will show whatever the savegame had open\n", StandbyClassName);
		AppSelectBroken = true;
		return false;
	}

	AppWindowClass = appCls;
	AppTabClass = tabCls;
	AppMenuClass = menuCls;
	AppViewClass = viewCls;
	FldStandbyView = fldStandby;
	FldViewParentMenu = fldParentMenu;
	FldTabDisabled = fldDisabled;
	FuncNumSubviews = funcNum;
	FuncViewAt = funcAt;
	FuncViewAdd = funcAdd;
	FuncAppClose = funcClose;
	FuncSwitchToApp = funcSwitch;
	FuncTabSetSelected = funcSel;
	AppSelectResolved = true;

	Printf("AuxStandbyCodex: standby app switch resolved on %s, %u apps selectable\n",
		StandbyClassName, (unsigned)countof(StandbyApps));
	return true;
}

// Is this app unlocked in the save that is loaded?
//
// READ OFF THE TAB, NOT RECOMPUTED, which is the whole point. PDAMenu3.init is the only thing that
// decides availability - tiersButt.setDisabled(numInvasionTiers <= 0) at pda_menu.zs:326, from the
// player's InvasiontierItem at :319-325, and mapButt.setDisabled on countInv("ChallengesUnlocked") at
// :259 - and UIControl.disabled is where the answer lands and what greys the tab out on the panel.
// Reading that field means the standby codex and the tab strip are physically incapable of disagreeing, and
// it needs no second copy of Selaco's unlock rules to rot. The other four tabs are never disabled, so
// they read available, which is correct.
//
// A missing or wrong-typed tab reads UNAVAILABLE. Fail-closed: showing a locked app is the bug.
static bool StandbyAppIsAvailable(DObject *menu, unsigned index)
{
	DObject *tab = ReadObjectField(menu, AppTabFields[index]);
	if (tab == nullptr || !tab->IsKindOf(AppTabClass))
		return false;

	return !*(const bool *)((const uint8_t *)tab + FldTabDisabled->Offset);
}

static int CallNumSubviews(DObject *view)
{
	int count = 0;
	VMReturn ret;
	ret.IntAt(&count);
	VMValue params[] = { view };
	VMCall(FuncNumSubviews, params, 1, &ret, 1);
	return count;
}

static DObject *CallViewAt(DObject *view, int index)
{
	void *sub = nullptr;
	VMReturn ret;
	ret.PointerAt(&sub);
	VMValue params[] = { view, index };
	VMCall(FuncViewAt, params, ViewAtRegs, &ret, 1);
	return (DObject *)sub;
}

// Point the standby codex at exactly one app: the one the player last had open in their real PDA if there is
// one, otherwise aux_standby_app, and in either case only if it is unlocked.
//
// TWO CALLERS, AND THEY ARE DIFFERENT SHAPES.
//
//   BuildStandbyView, after setCanvas and before the relayout. Both halves of that matter: add()
//   propagates desktopView's canvas to a window added after it (view.zs:941), and the relayout that
//   follows sizes whatever changed.
//
//   StandbyViewUpdate, on an ALREADY BUILT desktop, when the wanted app has changed - which is the falling
//   edge of the player closing their PDA. No init(), no rebuild; the new window's own requiresLayout is
//   what gets it laid out by the next draw (view.zs:797, :464).
//
// Returns false if nothing was changed, which is not a failure state - the desktop is valid and drawable
// either way, it just keeps whatever app set it already had.
static bool SelectStandbyApp(DObject *menu, PClass *menuCls, PClass *viewCls)
{
	if (AppSelectBroken)
		return false;

	if (!AppSelectResolved && !AppSelectResolve(menuCls, viewCls))
		return false;

	EAppSource source = AppSource_Cvar;
	const int index = StandbyWantedIndex(&source);

	if (source == AppSource_CvarInvalid)
	{
		// Logged here rather than in StandbyWantedIndex, which runs every frame.
		Printf(TEXTCOLOR_YELLOW "AuxStandbyCodex: aux_standby_app %d is not a selectable standby app, "
			"using %s\n", (int)aux_standby_app, StandbyApps[0].EnumName);
	}

	DObject *desktop = ReadObjectField(menu, FldStandbyView);
	if (desktop == nullptr || desktop->GetClass() != viewCls)
	{
		// Exactly UIView, not merely a subclass: add/viewAt/numSubviews were resolved against UIView's
		// vtable and a subclass could override any of them with something this code has not read.
		Printf(TEXTCOLOR_YELLOW "AuxStandbyCodex: %s.desktopView is not a plain %s, "
			"the standby codex will show whatever the savegame had open\n", StandbyClassName, ViewClassName);
		AppSelectBroken = true;
		return false;
	}

	int chosen = StandbyAppIsAvailable(menu, (unsigned)index) ? index : -1;
	if (chosen < 0)
	{
		for (unsigned i = 0; i < countof(StandbyApps); i++)
		{
			if ((int)i != index && StandbyAppIsAvailable(menu, i))
			{
				chosen = (int)i;
				break;
			}
		}

		if (chosen < 0)
		{
			// Cannot happen with Selaco as it stands - readerButt is never disabled - but "cannot happen"
			// is not the same as "need not be handled", and leaving the app set alone is the right answer
			// if it ever does.
			//
			// The wanted index IS advanced, so this says its line once rather than once per frame.
			// Availability is fixed for the life of one desktop instance - PDAMenu3.init is the only thing
			// that calls setDisabled on the tabs - so retrying against the same menu could only ever produce
			// the same answer. A rebuild resets it to -1 and the question is asked again, and so does the
			// player changing aux_standby_app to something else.
			Printf(TEXTCOLOR_YELLOW "AuxStandbyCodex: no standby app is available in this save, "
				"leaving the standby codex's app set alone\n");
			StandbyAppWantedIndex = index;
			return false;
		}

		// THE USER'S ORIGINAL BUG, and the one thing here that must never regress: a save with no unlocked
		// invasion tiers must not display INVASION TIERS, however the choice arrived.
		Printf("AuxStandbyCodex: %s is not unlocked in this save, showing %s instead\n",
			StandbyApps[index].EnumName, StandbyApps[chosen].EnumName);
	}

	// ALREADY THERE. Reached when the wanted app moved but resolves to what is on screen anyway - the same
	// app arriving from the live codex instead of from the cvar, or two different locked requests both
	// substituting to Datalogs. The sweep and the switch would be idempotent, but the redraw they would ask
	// for costs a ~30 ms readback (see i_auxpanel.cpp), so the wanted index is advanced and nothing is done.
	if (StandbyAppShownIndex >= 0 && chosen == StandbyAppShownIndex)
	{
		StandbyAppWantedIndex = index;
		return false;
	}

	bool constructed = false;

	try
	{
		// NESTED WHEN BuildStandbyView CALLS US, AND THE ONLY ONE ON THE RE-SELECT PATH. FProjectionScope is
		// depth-counted precisely so the first is harmless, and the second is load-bearing: close() reaches
		// savePos(0) and a newly added window's first layout reaches savePos(), both of which are
		// SendNetworkEvents that would write PDA geometry into the player's savegame.
		FProjectionScope projection;

		// Nothing here is known to touch menuactive, but the cost of covering it is one stack word and the
		// failure it prevents - a permanently wrong menuactive - lasts the rest of the session.
		FMenuActiveKeeper keepMenuState;

		// Did the desktop already have the app we want? Exact class rather than IsKindOf: APP_CLASSES names
		// the concrete class (pda_menu.zs:61-72) and so does the construction below, so the one we are
		// looking for is always exactly this class, and an unexpected subclass is something to construct
		// past rather than adopt.
		DObject *target = nullptr;
		{
			const int count = CallNumSubviews(desktop);
			for (int i = 0; i < count; i++)
			{
				DObject *v = CallViewAt(desktop, i);
				if (v != nullptr && v->GetClass() == AppClasses[chosen])
				{
					target = v;
					break;
				}
			}
		}

		if (target == nullptr)
		{
			// init() only constructs the apps the savegame had open (pda_menu.zs:416-419), so the one we
			// want may not exist at all. Built the way init() builds it, in its order: vInit, then
			// parentMenu, then desktopView.add. vInit runs before parentMenu is set there too (:419-420),
			// which is fine because the only app that reads getMenu() during construction reads it from
			// onAddedToParent (reader.zs:614-622), and add() calls that last (view.zs:943).
			//
			// sortOrder is deliberately NOT set: it is read only by insertAppWind while init() stacks the
			// restored apps (pda_menu.zs:739-754) and by nothing afterwards.
			//
			// BEFORE the close sweep, not after, so that a failure here leaves the app set init() restored
			// completely untouched - which is what the log line then claims.
			DObject *view = AppClasses[chosen]->CreateNew();
			StandbyPendingApp = view;
			GC::WriteBarrier(view);

			void *returned = nullptr;
			VMReturn ret;
			ret.PointerAt(&returned);
			VMValue params[] = { view, 100.0, 100.0, 800.0, 450.0 };
			VMCall(AppVInit[chosen], params, AppVInitRegs[chosen], &ret, 1);

			if (returned != view)
			{
				// vInit returns self on every path in the game's source; anything else means it took a
				// branch this code has not read. Nothing has been added or closed yet, so dropping the root
				// is the whole cleanup - the window is collected and the desktop is exactly as init() left it.
				StandbyPendingApp = nullptr;
				Printf(TEXTCOLOR_YELLOW "AuxStandbyCodex: %s.vInit did not return self, "
					"the standby codex will show whatever the savegame had open\n", StandbyApps[chosen].ClassName);
				AppSelectBroken = true;
				return false;
			}

			*(DObject **)((uint8_t *)view + FldViewParentMenu->Offset) = menu;
			GC::WriteBarrier(view, menu);

			VMValue addParams[] = { desktop, view };
			VMCall(FuncViewAdd, addParams, ViewAddRegs, nullptr, 0);

			// In desktopView.subviews now, so the menu's own graph keeps it alive.
			StandbyPendingApp = nullptr;
			target = view;
			constructed = true;
		}

		// CLOSE EVERY OTHER APP WINDOW, BACKWARDS, AND DO NOT LEAVE IT TO switchToAppWindow.
		//
		// All six tab apps are PDAFullscreenAppWindows, so switchToAppWindow's first branch
		// (pda_menu.zs:720-733) is meant to close the others for us. It does not reliably: it walks
		// desktopView.subviews FORWARDS with an incrementing index while close() -> removeFromSuperview
		// deletes from that same array (view.zs:961-985), so with three or more other apps open it skips
		// every second one, and then moveToBack()s our target UNDERNEATH a window it failed to close.
		// Walking backwards cannot skip: closing index i leaves every index below it exactly where it was,
		// and at each step the array still holds at least i+1 entries.
		//
		// It also covers the two apps switchToAppWindow ignores entirely. BBWindow and LevelInfoWindow are
		// PDAAppWindows but NOT fullscreen ones (bb_window.zs:1, level_info_window.zs:1), so its cast
		// yields null for them and either would sit on top of the standby codex at its saved position purely
		// because the player left it open - the same savegame coupling by another route. The one case not
		// covered is a BBWindow under `developer > 1`, which init() puts in innerView instead of
		// desktopView (pda_menu.zs:422); that is a dev-only state and only desktopView is swept.
		//
		// close() is safe to reach from a projection: onClose is a plain Super.onClose() in all five
		// overrides, and savePos(0) is a SendNetworkEvent that FProjectionScope refuses - it shows up in the
		// suppressed-write count this build logs. It does play MenuSound("codex/closeWindow")
		// (app_window.zs:136), which FProjectionScope now refuses too - it is a CHANF_UI sound and shows up
		// in the silenced-sound count beside the writes, so the sweep is silent however many windows it closes.
		for (int i = CallNumSubviews(desktop) - 1; i >= 0; i--)
		{
			DObject *v = CallViewAt(desktop, i);
			if (v == nullptr || v == target || !v->IsKindOf(AppWindowClass))
				continue;

			VMValue params[] = { v };
			VMCall(FuncAppClose, params, 1, nullptr, 0);
		}

		// The lever. With every other app window already gone this is moveToBack plus
		// currentAppWindow = target (pda_menu.zs:730-735), which is the field :469 sets from the restored
		// list and therefore the one thing that has to be overridden.
		{
			VMValue params[] = { menu, target };
			VMCall(FuncSwitchToApp, params, SwitchToAppRegs, nullptr, 0);
		}

		// Set the tab strip to match, explicitly for all six rather than leaning on the Event_Closed path
		// that PDAMenu3.handleControl uses to deselect a closed app's tab (pda_menu.zs:903-932). That path
		// does work, but it only fires for apps init() recorded in its own typed fields, and DATALOGS on
		// the panel under a highlighted INVASION TIERS is exactly the confusion this change removes.
		//
		// sound: false. The selected states were created without a sound (pda_menu.zs:218-220) so nothing
		// would play anyway, but a projection nobody asked for must not be able to make a noise.
		for (unsigned i = 0; i < countof(StandbyApps); i++)
		{
			DObject *tab = ReadObjectField(menu, AppTabFields[i]);
			if (tab == nullptr || !tab->IsKindOf(AppTabClass))
				continue;

			VMValue params[] = { tab, (int)(i == (unsigned)chosen), (int)0 };
			VMCall(FuncTabSetSelected, params, SetSelectedRegs, nullptr, 0);
		}
	}
	catch (const std::exception &e)
	{
		// Latch the SELECTION only, and not the desktop: the desktop is already built and drawing, and the
		// relayout in BuildStandbyView still runs. An abort part way through the sweep can leave fewer apps
		// open than init() restored, which is why this says "incomplete" rather than claiming the savegame's
		// set is intact.
		StandbyPendingApp = nullptr;
		Printf(TEXTCOLOR_YELLOW "AuxStandbyCodex: selecting the standby app aborted (%s), the standby codex's "
			"app set is incomplete for this build\n", e.what());
		AppSelectBroken = true;
		return false;
	}

	static const char *const SourceNames[] = { "live codex", "aux_standby_app", "aux_standby_app fallback" };
	const char *const from = StandbyAppShownIndex >= 0 && StandbyAppShownIndex != chosen
		? StandbyApps[StandbyAppShownIndex].EnumName : nullptr;

	// Advanced only on success, and it is the WANTED index rather than the chosen one - see the declaration.
	// Until this moves, the re-select trigger keeps asking, which is what makes a transient failure retry.
	StandbyAppWantedIndex = index;
	StandbyAppShownIndex = chosen;

	if (from != nullptr)
	{
		Printf("AuxStandbyCodex: standby codex switching from %s to %s (%s, from the %s, %s)\n",
			from, StandbyApps[chosen].EnumName, StandbyApps[chosen].ClassName,
			SourceNames[source], constructed ? "constructed" : "already open");
	}
	else
	{
		Printf("AuxStandbyCodex: standby codex showing %s (%s, from the %s, %s)\n",
			StandbyApps[chosen].EnumName, StandbyApps[chosen].ClassName,
			SourceNames[source], constructed ? "constructed" : "already open");
	}
	return true;
}

// ---------------------------------------------------------------------------------------------
// MODE 3: HOW BIG THE DASHBOARD IS ON THE PANEL - live, without a restart.
// ---------------------------------------------------------------------------------------------
//
// THE PROBLEM IS PHYSICAL, NOT A LAYOUT BUG. The relayout below puts the whole 1920-wide design across the
// 1240x1080 panel at scale 0.6458, which is correct and clips nothing - and at that scale, on a panel this
// size, it is too small to read at arm's length. So the zoom deliberately trades layout for legibility: it
// is EXPECTED to clip the right and the bottom, which is the price of text large enough to read.
//
// THE ZOOM IS ONE DIVISION. calcScale is newScale = uscale * CLAMP(canvasHeight / baseline.y, 0.599, 2)
// and then mainView.frame.size = canvasSize / newScale (pda_menu.zs:785-808), so dividing the baseline
// height by the zoom multiplies newScale by it and divides the logical box by it. A smaller logical box
// against a design whose offsets are fixed pixel figures is bigger content and a cropped right and bottom.
//
//     baseline.y  = DesktopBaselineHeight / zoom
//     newScale    = zoom * AuxCanvasWidth / DesktopDesignWidth      = zoom * 0.645833
//     logical box = (DesktopDesignWidth, DesktopBaselineHeight) / zoom
//
// ZOOM 1.0 IS BIT-IDENTICAL TO WHAT SHIPPED, which is the one property here that must not move: IEEE-754
// division by 1.0 is exact, so calcScale is handed the same double, settles on the same 0.645833 and
// produces the same 1920 x 1672.3 box with the same absence of clipping.
//
// WHERE THE RANGE ACTUALLY ENDS, WHICH IS NOT AT calcScale'S CEILING. The input is clamped to 0.5 - 2.0,
// and inside that window the binding limits are calcScale's FLOOR and its SNAP; its ceiling is out of
// reach entirely:
//
//     zoom <= 0.927    the 0.599 CLAMP floor holds newScale at 0.599, so every value below this draws
//                      identically - zooming OUT is effectively not available
//     1.425 - 1.672    inside |newScale - 1| < 0.08, so calcScale snaps the scale to exactly 1.0 and this
//                      whole quarter of the range is one plateau at a 1240x1080 box
//     zoom >= 3.097    the 2.0 CLAMP ceiling, which the 2.0 input clamp puts beyond reach
//
// So the useful travel is about 0.93 - 1.42 and 1.68 - 2.0, with a step across the plateau, and 2.0 gives
// scale 1.2917 - twice the apparent size of zoom 1.0. The 1.75 default sits in the upper band, above the
// snap plateau, at scale 1.1302 and a 1097 x 956 logical box. The log line reports the scale and box that
// came BACK from calcScale and names whichever limit fired, because "I changed the number and nothing
// happened" is otherwise indistinguishable from the cvar being broken.
//
// CVAR_ARCHIVE, UNLIKE THE OTHER SECOND-SCREEN CVARS, because this one is no longer a debug knob. It was
// unarchived while it was only reachable from the console and only a developer would move it; it is now a
// slider on the Handhelds options page, and a size the player picks in a menu has to still be there after
// a restart or the setting reads as broken.
//
// THIS DEFAULT IS COUPLED TO TabTextPaddingX AND CANNOT BE RAISED WITHOUT IT. At Selaco's own 42px tab
// padding, 1.75 puts the tab strip wider than the 1097 box and cuts DATALOGS and MANUAL off the ends -
// measured on the panel, not reasoned about. The strip is centre-pinned and sized to its contents, so it
// overflows symmetrically and the FIRST tab is lost as readily as the last. Tightening the padding to 20px
// recovers 264 design pixels and brings the whole strip, trigger icons included, inside that box. 1.5 was
// the default while the padding was untouched, and it is the value to fall back to if the tightening is
// ever removed.
//
// AND IT IS LOCALE-SENSITIVE FOR THE SAME REASON, which is worth knowing before raising it further. Each
// tab is its label's width plus the padding, so the strip's natural width depends on the language. 1.75 was
// confirmed on the panel in English; a locale with longer tab labels has less slack in the same box and
// could clip where English does not. That is an argument for measuring rather than assuming after any
// string change, not for keeping the panel small.
//
// ARCHIVING MAKES 1.75 A DEFAULT ONLY FOR CONFIGS THAT HAVE NEVER SET IT. A config carrying an earlier
// value keeps loading that value, because that is what CVAR_ARCHIVE means - the saved value wins over the
// declaration. That is expected rather than a bug: an existing profile keeps the size it was last seen at,
// and a fresh one starts at the size the panel was tuned for.
CVAR(Float, aux_codex_size, 1.75, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

// 0.5 - 2.0. The lower half is kept even though calcScale's floor swallows most of it, because a clamp
// that silently rewrites a value the player typed is worse than a range with a documented dead zone.
static const double CodexZoomMin = 0.5;
static const double CodexZoomMax = 2.0;

// calcScale's own limits expressed as zoom values - the numbers quoted in the table above. Derived from
// the canvas constants rather than written out so they follow a canvas resize; the literals are
// calcScale's own, the 0.599/2.0 CLAMP and the 0.08 snap window around 1.0 (pda_menu.zs:791-797).
static const double CodexZoomAtFloor = 0.599 * DesktopBaselineHeight / AuxCanvasHeight;
static const double CodexZoomAtSnapLow = 0.92 * DesktopBaselineHeight / AuxCanvasHeight;
static const double CodexZoomAtSnapHigh = 1.08 * DesktopBaselineHeight / AuxCanvasHeight;
static const double CodexZoomAtCeiling = 2.0 * DesktopBaselineHeight / AuxCanvasHeight;

// The zoom the relayout will actually use. The in-range test comes first so that a NaN - which fails every
// comparison, including both of the ones below it - lands on the no-zoom 1.0 rather than on a limit.
//
// NEVER NaN is the property mode 4 depends on, not merely a tidy fallback: i_auxlivecodex.cpp edge-triggers
// its relayout on this value changing, and NaN != NaN would fire that every frame - a savePos netevent into
// the player's savegame per frame. See the declaration in i_auxvmreflect.h.
//
// Not static: mode 4 divides its own baseline by the same clamped value, and the declaration in
// i_auxvmreflect.h says why that clamp is shared rather than written twice.
double CodexZoom()
{
	const double zoom = aux_codex_size;
	if (zoom >= CodexZoomMin && zoom <= CodexZoomMax)
		return zoom;
	if (zoom > CodexZoomMax)
		return CodexZoomMax;
	if (zoom < CodexZoomMin)
		return CodexZoomMin;
	return 1.0;
}

// The wide-box relayout's symbols, cached so the live retune can re-run it without resolving anything
// again. All null unless the OPTIONAL group in BuildStandbyView resolved; FuncStandbyCalcScale doubles as
// that group's flag, and with it null the zoom is inert and the layoutChange fallback is what runs.
static VMFunction *FuncStandbyCalcScale = nullptr;
static VMFunction *FuncStandbyMenuLayout = nullptr;
static VMFunction *FuncStandbyViewLayout = nullptr;
static PField *FldStandbyUIScaling = nullptr;
static PField *FldStandbyLastUIScale = nullptr;
static int StandbyCalcScaleRegs = 0;
static int StandbyViewLayoutRegs = 0;

// The two classes the relayout hands TightenTabStrip, cached for the same reason as everything above: the
// live retune re-runs the relayout with nothing but the two globals in scope, so it cannot look them up.
static PClass *StandbyPdaClass = nullptr;
static PClass *StandbyViewClass = nullptr;

// The zoom the live desktop is laid out for. The seed is inert in practice - BuildStandbyView writes it
// before anything the retune needs is non-null - and is the no-zoom value so a read before that says so.
static double StandbyLayoutZoom = 1.0;

// The relayout, factored out so the live retune re-runs EXACTLY what the build ran. Five steps: narrow the
// tab strip, null the menu's ui_scaling handle, calcScale with the zoomed baseline, then the remaining two
// lines of layoutChange's body (pda_menu.zs:777-781).
//
// THE CALLER OWNS THE PROJECTION SCOPE AND THE TRY/CATCH. Every call here can reach script - mainView
// .layout() reaches PDAAppWindow.layout -> savePos, which is a SendNetworkEvent into the player's savegame -
// so there is no safe way to call this outside one, and both call sites are already inside theirs.
static void StandbyRelayout(DObject *menu, DObject *mainView, double zoom)
{
	// The tab strip goes first because it only writes fields and pins: it needs the layout below to take
	// effect, and folding it in here is what keeps it from costing a second pass. Fails soft on its own and
	// latches nothing, so its return value is not worth testing - a clipped strip beats no desktop.
	TightenTabStrip(menu, StandbyPdaClass, StandbyViewClass);

	// Force uscale to 1.0 by nulling the menu's own cvar handle. calcScale's read of it is
	// `ui_scaling ? ui_scaling.getFloat() : 1.0` (pda_menu.zs:787), so null IS the 1.0 path. Left null
	// rather than restored: the only other readers are UIMenu.calcScale and UIMenu.ticker (menu.zs:125,
	// :206), both null-guarded, and this menu is never ticked. Never the CVAR - `ui_scaling` is
	// CVAR_USERINFO (d_main.cpp:1757) and writing it would push a DEM_UINFCHANGED into the demo/net
	// stream, the very class of player-state write FProjectionScope exists to stop.
	*(void **)((uint8_t *)menu + FldStandbyUIScaling->Offset) = nullptr;

	// calcScale(int screenWidth, int screenHeight, Vector2 baselineResolution) - the Vector2 is two
	// consecutive registers, hence five VMValues for three declared arguments. Dividing the baseline height
	// is the whole of the zoom, and at 1.0 that division is exact, so the argument is the same double the
	// un-zoomed build passed.
	VMValue params[] = { menu, (int)AuxCanvasWidth, (int)AuxCanvasHeight,
		DesktopDesignWidth, DesktopBaselineHeight / zoom };
	VMCall(FuncStandbyCalcScale, params, StandbyCalcScaleRegs, nullptr, 0);

	// hasLayedOutOnce is deliberately NOT set: its only reader is UIMenu.drawer (menu.zs:261), which we
	// never call, and setting it would arm a relayout at the default (1920, 1080) baseline if anything ever
	// did - undoing both the wide box and the zoom.
	VMValue selfOnly[] = { menu };
	VMCall(FuncStandbyMenuLayout, selfOnly, 1, nullptr, 0);

	// mainView.layout() with the DEFAULTS layoutChange passes: parentScale (0,0) is the sentinel that makes
	// UIView.layout derive cScale from the view's own scale chain (view.zs:763) instead of taking ours, and
	// parentAlpha -1 does the same for alpha (:764). Passing (1,1)/1.0 instead would overwrite the scale
	// calcScale just installed. layoutSubviews recurses unconditionally (view.zs:787-794) rather than
	// honouring requiresLayout, which is what makes this sufficient on a retune: nothing in the tree is
	// left holding the old scale.
	VMValue viewParams[] = { mainView, 0.0, 0.0, -1.0, (int)0 };
	VMCall(FuncStandbyViewLayout, viewParams, StandbyViewLayoutRegs, nullptr, 0);
}

// The scale calcScale actually installed, which the logical box follows from.
//
// READ BACK RATHER THAN RECOMPUTED, because the CLAMP and the snap are the whole point of the line: the
// scale asked for is not always the scale installed, and which limit fired is exactly what someone tuning
// the zoom by eye needs to see. PDAMenu3.calcScale leaves the FINAL value - after both the CLAMP and the
// snap - in lastUIScale (pda_menu.zs:801, field declared at menu.zs:40). UIMenu.calcScale would leave the
// raw uscale there instead, but it is PDAMenu3's override that runs.
//
// lastUIScale is OPTIONAL: its absence costs the accuracy of a log line and must not cost the wide box, so
// the arithmetic is used instead and the line says "predicted" rather than "measured". A number that is
// silently a guess is worse than one labelled as one.
static double StandbyInstalledScale(DObject *menu, double zoom, bool *outMeasured)
{
	if (FldStandbyLastUIScale != nullptr && menu != nullptr)
	{
		if (outMeasured != nullptr)
			*outMeasured = true;
		return *(const double *)((const uint8_t *)menu + FldStandbyLastUIScale->Offset);
	}

	if (outMeasured != nullptr)
		*outMeasured = false;

	// calcScale's body (pda_menu.zs:791-797), replicated for the log line only and never for the layout.
	double scale = AuxCanvasHeight / (DesktopBaselineHeight / zoom);
	if (scale < 0.599)
		scale = 0.599;
	else if (scale > 2.0)
		scale = 2.0;
	if (fabs(scale - 1.0) < 0.08)
		scale = 1.0;
	else if (fabs(scale - 2.0) < 0.08)
		scale = 2.0;
	return scale;
}

// Which of calcScale's own limits swallowed this zoom, if any. Empty for the normal case, where the zoom
// asked for is the zoom that came back. Not static, for the same reason CodexZoom is not: mode 4 hits
// the same three bands and needs the same phrase to say so.
const char *CodexZoomLimitNote(double zoom)
{
	if (zoom <= CodexZoomAtFloor)
		return " - held at calcScale's 0.599 floor, no smaller zoom changes anything";
	if (zoom >= CodexZoomAtSnapLow && zoom <= CodexZoomAtSnapHigh)
		return " - inside calcScale's 1.0 snap window, this whole band draws identically";
	if (zoom >= CodexZoomAtCeiling)
		return " - held at calcScale's 2.0 ceiling, no larger zoom changes anything";
	return "";
}

// LIVE RETUNE: apply a changed aux_codex_size to the desktop that is ALREADY BUILT.
//
// A RELAYOUT, NOT A REBUILD, and the difference is a dropped frame per value tried. PDAMenu3.init allocates
// 100+ DObjects, re-runs the app selection and fires the whole suppression machinery; the four calls in
// StandbyRelayout are the game's OWN answer to "the screen you are laid out for is not the screen you are
// on" and are what layoutChange would do to a live menu on a resolution change. Since layoutSubviews
// recurses unconditionally, that reaches every view in the tree - so there is nothing a rebuild would fix.
//
// A REDRAW IS ALSO REQUESTED, which is the part that makes the cvar look like it works at all. The
// panel holds the last pixels Java was pushed and the readback is edge-triggered (i_auxpanel.cpp), so
// without *outNeedsRedraw the new layout would sit in the canvas unread and the player would keep seeing
// the pre-zoom image - the cvar would read as broken while working perfectly.
static void StandbyRetune(bool *outNeedsRedraw)
{
	if (StandbyMenu == nullptr || StandbyRootView == nullptr)
		return;

	// Gated on the OPTIONAL wide-box group, so a build that fell back to layoutChange keeps exactly the
	// behaviour it had and the zoom is simply inert. Failing soft here is one unchanged panel, not a
	// broken one.
	const double zoom = CodexZoom();
	if (FuncStandbyCalcScale != nullptr && zoom != StandbyLayoutZoom)
	{
		try
		{
			// Load-bearing rather than precautionary: mainView.layout() reaches PDAAppWindow.layout ->
			// savePos -> SendNetworkEvent("pdaAppPos:..."), which would write the geometry of the PLAYER's
			// real PDA into their savegame once per value they try.
			FProjectionScope projection;
			FMenuActiveKeeper keepMenuState;

			StandbyRelayout(StandbyMenu, StandbyRootView, zoom);
		}
		catch (const std::exception &e)
		{
			// Nothing is latched beyond refusing to retry this value: the desktop is still built and still
			// drawable, laid out for whatever scale the abort left it at. A VM abort here must not take the
			// frame - and therefore the main screen - down.
			Printf(TEXTCOLOR_YELLOW "AuxStandbyCodex: relaying the standby codex out at zoom %g aborted (%s)\n",
				zoom, e.what());
		}

		// Advanced whether or not it threw, so the line above is said once per value rather than per frame.
		StandbyLayoutZoom = zoom;

		bool measured = false;
		const double scale = StandbyInstalledScale(StandbyMenu, zoom, &measured);

		// The parenthesised token is a REVISION SENTINEL, deliberately unique to this change so it can be
		// grepped out of the built .so to prove which revision is actually packaged - a size match has
		// twice passed against a stale APK on this project.
		Printf("AuxStandbyCodex: standby codex zoom %g%s, logical box %gx%g at %s scale %g%s "
			"(aux-codex-size r2)\n",
			zoom, zoom != (double)aux_codex_size ? " (clamped)" : "",
			AuxCanvasWidth / scale, AuxCanvasHeight / scale, measured ? "measured" : "predicted", scale,
			CodexZoomLimitNote(zoom));

		if (outNeedsRedraw != nullptr)
			*outNeedsRedraw = true;
	}
}

// Drop the built desktop and unroot it. Defined further down, next to the reasoning about PDAMenu3's
// onDestroy; forward-declared here because BuildStandbyView's own failure paths run AFTER the menu has
// been created and rooted, and every one of them has to hand it back.
static void StandbyViewDiscard();

// Build the desktop. Runs once; every failure latches.
//
// The transient gate comes FIRST, ahead of every symbol lookup: it is the cheapest test and the one
// most often false, and a mode-3 frame at the title screen must not pay for a dozen FindSymbols to be
// told to come back later. The cost is that the "not the full Selaco" line below only appears once a
// level is loaded, which is a delayed diagnostic rather than a missing one.
//
// EVERY FAILURE AFTER cls->CreateNew() DISCARDS THE MENU, which is not tidying: StandbyMenu is marked by
// the marker function registered below, so a bare `return false` would leave a half-built PDAMenu3 and the
// 100+ DObjects init() allocated rooted for the life of the process - and its onDestroy, which writes the
// music volume, would then never run at a controlled moment either.
static bool BuildStandbyView()
{
	// PDAMenu3.init dereferences players[consoleplayer].mo unguarded (pda_menu.zs:320,368,372,385,390)
	// and reads Level.LevelName/maptime/MusicVolume, so this is all or nothing - there is no half-built
	// state to defer. gamestate is also the spoiler gate: TITLEMAP is GS_TITLELEVEL and has its own pawn
	// with its own empty progress, so a desktop built there would confidently show the wrong unlock
	// state.
	//
	// THERE IS DELIBERATELY NO DEV OVERRIDE for this. Mode 3 constructs the whole desktop against the
	// live player, and building that before a level exists is the one thing this mode is required never
	// to do; a switch to do it anyway would only ever be a way to break that rule by accident. The gate
	// is unconditional and the build simply waits.
	if (gamestate != GS_LEVEL)
		return false;

	if (consoleplayer < 0 || consoleplayer >= MAXPLAYERS || !playeringame[consoleplayer]
		|| players[consoleplayer].mo == nullptr)
	{
		return false;
	}

	PClass *cls = PClass::FindClass(StandbyClassName);
	if (cls == nullptr)
	{
		// The Doom and demo path, and the only outcome here that is not a diagnostic.
		Printf("AuxStandbyCodex: no %s class - not the full Selaco, desktop view unavailable\n", StandbyClassName);
		StandbyAbsent = true;
		return false;
	}

	// Two ancestry checks, because two different things are assumed. DMenu is what makes init's Menu
	// parameter and the menuactive write make sense; UIMenu is what guarantees mainView exists.
	if (!cls->IsDescendantOf(RUNTIME_CLASS(DMenu)) || !cls->IsDescendantOf(FName(MenuClassName, true)))
	{
		Printf(TEXTCOLOR_YELLOW "AuxStandbyCodex: %s is not a %s, second-screen view disabled\n",
			StandbyClassName, MenuClassName);
		StandbyBroken = true;
		return false;
	}

	if (cls->bAbstract || cls->ConstructNative == nullptr)
	{
		// CreateNew calls I_Error on either of these rather than returning null (dobjtype.cpp:433-437).
		Printf(TEXTCOLOR_YELLOW "AuxStandbyCodex: %s cannot be instantiated, second-screen view disabled\n", StandbyClassName);
		StandbyBroken = true;
		return false;
	}

	PClass *viewCls = PClass::FindClass(ViewClassName);
	if (viewCls == nullptr)
	{
		Printf(TEXTCOLOR_YELLOW "AuxStandbyCodex: no %s class, second-screen view disabled\n", ViewClassName);
		StandbyBroken = true;
		return false;
	}

	FCanvas *canvas = GetTextureCanvas(AuxCanvasName);
	if (canvas == nullptr || canvas->Tex == nullptr)
	{
		Printf(TEXTCOLOR_YELLOW "AuxStandbyCodex: %s is not a canvas texture, second-screen view disabled\n", AuxCanvasName);
		StandbyBroken = true;
		return false;
	}

	// Resolve everything BEFORE constructing anything, so a mismatch leaves no half-built menu rooted
	// for the life of the process. The view-side methods are resolved against UIView because mainView
	// is literally new("UIView") (menu.zs:163); that it really is one is re-checked after init.
	static const EArgKind InitArgs[] = { Arg_Menu };
	static const EArgKind CanvasArgs[] = { Arg_Canvas };
	static const EArgKind LayoutChangeArgs[] = { Arg_Int, Arg_Int };

	int initRegs = 0, canvasRegs = 0, layoutChangeRegs = 0, drawRegs = 0, subviewRegs = 0;
	VMFunction *funcInit = ResolveMethod(cls, "init", "AuxStandbyCodex", InitArgs, 1, &initRegs);
	// layoutChange(int, int) is the game's OWN entry point for "the screen you are laid out for is not
	// the screen you are on" (menu.zs:113-121, overridden at pda_menu.zs:775): it calls calcScale,
	// refreshes background.freeze, then relayouts. Using it is why nothing here writes mainView.frame.
	VMFunction *funcLayoutChange = funcInit != nullptr
		? ResolveMethod(cls, "layoutChange", "AuxStandbyCodex", LayoutChangeArgs, 2, &layoutChangeRegs) : nullptr;
	VMFunction *funcSetCanvas = funcLayoutChange != nullptr
		? ResolveMethod(viewCls, "setCanvas", "AuxStandbyCodex", CanvasArgs, 1, &canvasRegs) : nullptr;
	VMFunction *funcDraw = funcSetCanvas != nullptr ? ResolveMethod(viewCls, "draw", "AuxStandbyCodex", nullptr, 0, &drawRegs) : nullptr;
	VMFunction *funcDrawSubviews = funcDraw != nullptr
		? ResolveMethod(viewCls, "drawSubviews", "AuxStandbyCodex", nullptr, 0, &subviewRegs) : nullptr;
	if (funcDrawSubviews == nullptr)
	{
		// ResolveMethod already logged which check failed; it latches nothing, so mode 3 records its own
		// required-symbol failures here. Same for the two groups below.
		StandbyBroken = true;
		return false;
	}

	// The fields. mainView is the draw root; the rest are the three suppressions from the header.
	PField *fldMainView = ResolveField(cls, "mainView", Field_ViewPtr, viewCls);
	PField *fldBackground = fldMainView != nullptr ? ResolveField(cls, "background", Field_ViewPtr, viewCls) : nullptr;
	PField *fldInnerView = fldBackground != nullptr ? ResolveField(cls, "innerView", Field_ViewPtr, viewCls) : nullptr;
	PField *fldHidden = fldInnerView != nullptr ? ResolveField(viewCls, "hidden", Field_Bool, viewCls) : nullptr;
	PField *fldAlpha = fldHidden != nullptr ? ResolveField(viewCls, "alpha", Field_Float, viewCls) : nullptr;
	if (fldAlpha == nullptr)
	{
		StandbyBroken = true;
		return false;
	}

	// The six tab buttons (declared together at pda_menu.zs:38, all PDATab : UIButton : UIControl :
	// UIView). All six are constructed unconditionally, so a null one would mean init took a branch this
	// code has not read - reported, not worked around.
	static const char *const TabFieldNames[] =
	{
		"readerButt", "mapButt", "objectivesButt", "statsButt", "tiersButt", "manualButt"
	};
	PField *fldTabs[countof(TabFieldNames)] = {};
	for (unsigned i = 0; i < countof(TabFieldNames); i++)
	{
		fldTabs[i] = ResolveField(cls, TabFieldNames[i], Field_ViewPtr, viewCls);
		if (fldTabs[i] == nullptr)
		{
			StandbyBroken = true;
			return false;
		}
	}

	// ------------------------------------------------------------------------------------------
	// THE WIDE-BOX PATH, and the one group of symbols here that is OPTIONAL.
	//
	// layoutChange(1240, 1080) calls calcScale with its DEFAULT baseline of (1920, 1080)
	// (menu.zs:123), and calcScale only ever looks at the height: newScale = uscale *
	// CLAMP(1080/1080) = uscale, so mainView ends up 1240/uscale logical pixels wide against a design
	// that assumes 1920 and the tab bar is cut off at both ends. Passing calcScale a baseline height
	// of DesktopBaselineHeight instead makes the logical box exactly DesktopDesignWidth wide.
	//
	// calcScale is called rather than reimplemented because it also sets UIDrawer's screenSize and
	// virtualScreenSize (pda_menu.zs:806-807) and mainView.scale, all of which have to move together;
	// the two calls after it are the rest of layoutChange's body (pda_menu.zs:775-782).
	//
	// If any of this does not resolve, the code falls back to layoutChange - which is what shipped and
	// is correct in every respect except the clipping. Neither resolver latches anything, so nothing here
	// touches StandbyBroken: a clipped tab bar beats no desktop.
	// ------------------------------------------------------------------------------------------
	static const EArgKind CalcScaleArgs[] = { Arg_Int, Arg_Int, Arg_Vector2 };
	static const EArgKind ViewLayoutArgs[] = { Arg_Vector2, Arg_Float, Arg_Bool };

	int calcScaleRegs = 0, menuLayoutRegs = 0, viewLayoutRegs = 0;
	// calcScale(int, int, Vector2) is FIVE registers, not four - a Vector2 is one declared argument and
	// two registers (types.cpp:365). ResolveMethod proves that against the callee's own NumArgs.
	VMFunction *funcCalcScale = ResolveMethod(cls, "calcScale", "AuxStandbyCodex", CalcScaleArgs, 3, &calcScaleRegs);
	// PDAMenu3.layout() - pda_menu.zs:771, non-virtual, self only. All it does is refresh
	// background.freeze, which is moot while background.hidden is true, but it is what layoutChange
	// calls and there may be more in it later.
	VMFunction *funcMenuLayout = funcCalcScale != nullptr
		? ResolveMethod(cls, "layout", "AuxStandbyCodex", nullptr, 0, &menuLayoutRegs) : nullptr;
	// mainView.layout(), with the DEFAULTS layoutChange passes: parentScale (0,0) is the sentinel that
	// makes UIView.layout derive cScale from the view's own scale chain (view.zs:763) instead of taking
	// ours, and parentAlpha -1 does the same for alpha (:764). Passing (1,1)/1.0 instead would overwrite
	// the 0.6458 scale calcScale just installed.
	VMFunction *funcViewLayout = funcMenuLayout != nullptr
		? ResolveMethod(viewCls, "layout", "AuxStandbyCodex", ViewLayoutArgs, 3, &viewLayoutRegs) : nullptr;
	// PDAMenu3.calcScale reads ui_scaling unconditionally (pda_menu.zs:787) - unlike UIMenu.calcScale
	// it does not honour ignoreUIScaling (menu.zs:125) - and Selaco's handheld profile sets that cvar
	// to 1.2 (SetSteamdeckPresets, forced on for Android at d_main.cpp:3513). A baseline cannot absorb
	// it: compensating would need CLAMP(1080/baseline) = 0.538, below calcScale's own 0.599 floor. So
	// the MENU'S FIELD is nulled instead of the cvar being touched, because `ui_scaling` is CVAR_USERINFO
	// (d_main.cpp:1757) and writing it would push a DEM_UINFCHANGED into the demo/net stream - the very
	// class of player-state write FProjectionScope exists to stop.
	PField *fldUIScaling = funcViewLayout != nullptr
		? ResolveField(cls, "ui_scaling", Field_CVarPtr, viewCls) : nullptr;
	if (fldUIScaling == nullptr)
	{
		funcCalcScale = nullptr;
		Printf(TEXTCOLOR_YELLOW "AuxStandbyCodex: no wide-box relayout, falling back to layoutChange - "
			"the tab bar will clip horizontally and aux_codex_size will do nothing\n");
	}

	// Cached for the live retune, which re-runs exactly this relayout when aux_codex_size moves.
	// Assigned on the failure path too, because FuncStandbyCalcScale is what StandbyRetune tests to decide
	// whether the zoom can do anything at all.
	FuncStandbyCalcScale = funcCalcScale;
	FuncStandbyMenuLayout = funcMenuLayout;
	FuncStandbyViewLayout = funcViewLayout;
	FldStandbyUIScaling = fldUIScaling;
	StandbyCalcScaleRegs = calcScaleRegs;
	StandbyViewLayoutRegs = viewLayoutRegs;
	StandbyPdaClass = cls;
	StandbyViewClass = viewCls;

	// lastUIScale is where PDAMenu3.calcScale leaves the scale it settled on, and the only use made of it
	// here is the log line - so it is resolved apart from the group above and its absence costs the accuracy
	// of that line and nothing else. See StandbyInstalledScale.
	FldStandbyLastUIScale = funcCalcScale != nullptr
		? ResolveField(cls, "lastUIScale", Field_Float, viewCls) : nullptr;

	// Root before anything can allocate: init() creates well over a hundred DObjects and any of those
	// allocations can run a GC step, so the menu has to be reachable from a root by the first one.
	//
	// REGISTERED ONCE, not once per build. The marker closes over the two globals rather than over a
	// particular menu, so one registration covers every rebuild - and since the stats trigger below can
	// rebuild many times in a playthrough, re-registering here would append a marker function per
	// rebuild forever, each doing the same work on every collection. There is no RemoveMarkerFunc.
	static bool markerRegistered = false;
	if (!markerRegistered)
	{
		GC::AddMarkerFunc([]() { GC::Mark(StandbyMenu); GC::Mark(StandbyRootView); GC::Mark(StandbyPendingApp); });
		markerRegistered = true;
	}

	DObject *menu = cls->CreateNew();
	StandbyMenu = menu;
	GC::WriteBarrier(menu);   // the pointer is not inside an object, same case as menu.cpp:372

	try
	{
		// Everything from here to the end of layoutChange is inside a projection scope, because all
		// three of the desktop's savegame writes fire in it: "pdaUnreadClear" from PDAReaderWindow.init
		// (reader.zs:538), "pdaTimeoutClear"/"seenTiers" from init's own tail (pda_menu.zs:490,496),
		// and "pdaAppPos:..." from PDAAppWindow.layout -> savePos (app_window.zs:170), which
		// layoutChange reaches through mainView.layout().
		FProjectionScope projection;
		const int suppressedBefore = FProjectionScope::Suppressed;
		const int silencedBefore = FProjectionScope::SuppressedSounds;

		{
			// init(Menu parent) - pda_menu.zs:75. Parent null: it is only used for `parent is "UIMenu"`
			// (menu.zs:150) and mParentMenu, and a parentless menu is what keeps this out of the menu
			// stack entirely. Returns void, so there is no self to check.
			FMenuActiveKeeper keepMenuState;
			VMValue params[] = { menu, (DObject *)nullptr };
			VMCall(funcInit, params, initRegs, nullptr, 0);
		}

		DObject *mainView = ReadObjectField(menu, fldMainView);
		if (mainView == nullptr || mainView->GetClass() != viewCls)
		{
			// Exactly UIView, not merely a subclass: draw/drawSubviews/setCanvas were resolved against
			// UIView's vtable, and a subclass could override any of them with something this code has
			// not read. Re-resolving against the real class would be the fix if Selaco ever changes it.
			Printf(TEXTCOLOR_YELLOW "AuxStandbyCodex: %s.mainView is not a plain %s, second-screen view disabled\n",
				StandbyClassName, ViewClassName);
			StandbyBroken = true;
			StandbyViewDiscard();
			return false;
		}

		// SUPPRESSION 1. UIScrollBG.draw() calls Screen.drawShape unconditionally (scroll_bg.zs:76) and
		// our hook runs before BeginFrame, so it would throw rather than misdraw. Its own first line is
		// `if(hidden) return;` (scroll_bg.zs:40), which is the whole fix.
		DObject *background = ReadObjectField(menu, fldBackground);
		if (background == nullptr || !background->IsKindOf(viewCls))
		{
			Printf(TEXTCOLOR_YELLOW "AuxStandbyCodex: %s.background is not a %s, second-screen view disabled\n",
				StandbyClassName, ViewClassName);
			StandbyBroken = true;
			StandbyViewDiscard();
			return false;
		}
		*(bool *)((uint8_t *)background + fldHidden->Offset) = true;

		// SUPPRESSION 2. innerView and all six tabs are created at alpha 0 and animated up only in
		// ticker(). Without this the desktop is laid out and drawn perfectly at alpha 0 - i.e. blank.
		if (!SetViewAlpha(ReadObjectField(menu, fldInnerView), viewCls, fldAlpha, 1.0))
		{
			Printf(TEXTCOLOR_YELLOW "AuxStandbyCodex: %s.innerView is not a %s, second-screen view disabled\n",
				StandbyClassName, ViewClassName);
			StandbyBroken = true;
			StandbyViewDiscard();
			return false;
		}

		for (unsigned i = 0; i < countof(TabFieldNames); i++)
		{
			if (!SetViewAlpha(ReadObjectField(menu, fldTabs[i]), viewCls, fldAlpha, 1.0))
			{
				Printf(TEXTCOLOR_YELLOW "AuxStandbyCodex: %s.%s is not a %s, second-screen view disabled\n",
					StandbyClassName, TabFieldNames[i], ViewClassName);
				StandbyBroken = true;
				StandbyViewDiscard();
				return false;
			}
		}

		// setCanvas before layoutChange, because the retarget has to be in place before anything the
		// relayout triggers can draw or measure against a screen. It recurses the whole subtree
		// (view.zs:263-268), and add() propagates it to anything added later (:941).
		{
			VMValue params[] = { mainView, canvas };
			VMCall(funcSetCanvas, params, canvasRegs, nullptr, 0);
		}

		// WHICH APP THE PANEL SHOWS - the live codex's last tab if there is one, otherwise
		// aux_standby_app, and never a locked app. See the block above SelectStandbyApp. Placed after
		// setCanvas so a window added now inherits the canvas (view.zs:941), and before the relayout below so
		// that whatever changed gets laid out by it. A false return is not a failure: the desktop then keeps
		// the app set init() restored, which is what shipped, so it is deliberately not checked.
		SelectStandbyApp(menu, cls, viewCls);

		// SUPPRESSION 3 - and the reason mainView's frame is never written by hand. UIMenu.init sized
		// mainView from Screen.GetWidth()/GetHeight() (menu.zs:157-163) and scaled it with calcScale
		// (:174), so the desktop is laid out for the MAIN screen and would crop onto our 1240x1080
		// canvas. Relaying it out is the game's own answer to that, so the scale, the frame, UIDrawer's
		// screenSize/virtualScreenSize and background.freeze all move together and stay consistent with
		// whatever PDAMenu3.calcScale decides (pda_menu.zs:786-808) rather than with our arithmetic.
		//
		// The difference from layoutChange is the BASELINE, and only the baseline: layoutChange would
		// let calcScale default to (1920, 1080), which makes the logical box the canvas width and clips
		// a 1920-wide design at both edges. See DesktopBaselineHeight for the arithmetic, and
		// aux_codex_size for the divisor the player can move - at zoom 1.0 this is exactly what
		// shipped.
		if (funcCalcScale != nullptr)
		{
			StandbyLayoutZoom = CodexZoom();
			StandbyRelayout(menu, mainView, StandbyLayoutZoom);
		}
		else
		{
			VMValue params[] = { menu, (int)AuxCanvasWidth, (int)AuxCanvasHeight };
			VMCall(funcLayoutChange, params, layoutChangeRegs, nullptr, 0);
		}

		StandbyRootView = mainView;
		GC::WriteBarrier(mainView);

		if (FProjectionScope::Suppressed != suppressedBefore
			|| FProjectionScope::SuppressedSounds != silencedBefore)
		{
			// Not a warning. This is the evidence that the projection scope is load-bearing rather than
			// decorative, and the counts are what a future app's writes and chirps would show up in. The
			// sound count is the one to read after a rebuild: it should be 1 for PDAMenu3.init's own
			// MenuSound("codex/open") (pda_menu.zs:90) plus one per app window SelectStandbyApp closed
			// (app_window.zs:136), and a 0 there means the guard in s_doomsound.cpp is not in this binary.
			//
			// The parenthesised token is a REVISION SENTINEL, deliberately unique to this change so it can
			// be grepped out of the built .so to prove which revision is actually packaged - a size match
			// has twice passed against a stale APK on this project.
			Printf("AuxStandbyCodex: suppressed %d player-state write(s) and silenced %d UI sound(s) "
				"while building %s (projection-mute r1)\n",
				FProjectionScope::Suppressed - suppressedBefore,
				FProjectionScope::SuppressedSounds - silencedBefore, StandbyClassName);
		}
	}
	catch (const std::exception &e)
	{
		// A VM abort is a CVMAbortException, deriving from std::exception (vm.h:107,
		// engineerrors.h:46). Catching it is what keeps a bug in game code we are calling unusually
		// from taking the frame - and therefore the main screen - down with it.
		Printf(TEXTCOLOR_YELLOW "AuxStandbyCodex: building %s aborted (%s), second-screen view disabled\n",
			StandbyClassName, e.what());
		StandbyBroken = true;
		StandbyViewDiscard();
		return false;
	}

	StandbyCanvas = canvas;
	FuncStandbyDraw = funcDraw;
	FuncStandbyDrawSubviews = funcDrawSubviews;

	// The logical box is what to check a screenshot against: at 1240x1080 with the derived baseline and zoom
	// 1.0 it is 1920 x 1672.3 at scale 0.6458, so the whole 1920-wide design fits across the panel.
	// The scale is read back from the menu rather than computed, so this shows what calcScale's own CLAMP and
	// snap settled on rather than what was asked for - see StandbyInstalledScale.
	if (funcCalcScale != nullptr)
	{
		bool measured = false;
		const double scale = StandbyInstalledScale(menu, StandbyLayoutZoom, &measured);
		Printf("AuxStandbyCodex: %s built at %gx%g on %s, logical box %gx%g at %s scale %g, zoom %g%s\n",
			StandbyClassName, AuxCanvasWidth, AuxCanvasHeight, AuxCanvasName,
			AuxCanvasWidth / scale, AuxCanvasHeight / scale, measured ? "measured" : "predicted", scale,
			StandbyLayoutZoom, CodexZoomLimitNote(StandbyLayoutZoom));
	}
	else
	{
		Printf("AuxStandbyCodex: %s built at %gx%g on %s, logical box %gx%g at scale 1 "
			"(layoutChange fallback, aux_codex_size inert)\n",
			StandbyClassName, AuxCanvasWidth, AuxCanvasHeight, AuxCanvasName,
			AuxCanvasWidth, AuxCanvasHeight);
	}
	return true;
}

// ---------------------------------------------------------------------------------------------
// MODE 3's REBUILD TRIGGER: the player's live stat totals.
// ---------------------------------------------------------------------------------------------
//
// WHY A REBUILD AND NOT AN UPDATE, which is the same root cause as the alpha-0 writes and is worth
// stating once: everything Selaco decides in init() is frozen for us, because the real PDA is opened,
// read and thrown away in seconds and never needed a refresh path. PDAStatWindowNormal has no tick()
// override and no refresh()/update()/rebuild() of any kind - it accumulates the counters and formats
// them into label text entirely inside init() (stat_wind_norm.zs:18-102). So redrawing renders whatever
// text the labels already hold, and ticking would change nothing. Re-running init() is the only way to
// get new numbers, and re-running init() means rebuilding the desktop.
//
// WHAT IS POLLED, and why it is NOT the engine's own counters. FLevelLocals carries found_secrets,
// found_items and killed_monsters (g_levellocals.h), and none of them is what this window reads. It
// reads EIGHT of Selaco's own StatTrackers through Stats.FindTracker - secrets, datapads, trading cards,
// upgrades, clearance cards, cabinet cards, storage cabinets and kills - and sums their per-level arrays
// across the current level group (stat_wind_norm.zs:33-60). Six of those eight have no FLevelLocals
// counterpart at all, so an engine-counter trigger would refresh on a secret and stay silently stale for
// a datapad, which is the more common pickup. That is a refresh that looks like it works.
//
// WHY A WHOLE-ARRAY HASH RATHER THAN NAMED INDICES. Stats holds `StatTracker trackers[STAT_COUNT]`
// (stats.zs:279) and the file's own comment at :297 warns that CHANGING OR DELETING INDICES BREAKS OLD
// SAVES - so the indices are stable, but hardcoding any of them makes this a hostage to a future patch
// that inserts one. Hashing every entry needs no index knowledge and is a COMPLETE detector: it cannot
// silently miss a category the way a hand-written list of counters can.
//
// AND WHY THE SCALARS ARE SUFFICIENT, which had to be checked rather than assumed. The window displays
// levelValues[]/levelPossibleValues[], not value/possibleValue - so hashing the scalars would be wrong
// if anything moved the arrays without moving the totals. StatTracker.add (stats.zs:219-240) is the
// single funnel for every increment (Stats.AddStat/AddStatF at :659/:664 both route to it) and it writes
// value/possibleValue AND the level arrays in the same call; reset (:242-256) always zeroes value first.
// So every write that can change what the window shows also moves a scalar. The one exception is
// StatTrackerSkill assigning levelValues directly (:715), which is not one of the eight displayed
// trackers.
static const char *const StatsClassName = "Stats";
static const char *const StatTrackerClassName = "StatTracker";

// Latched on the first failure and never retried, and the degradation is deliberate: mode 3 then keeps
// the static-snapshot behaviour it had before this, which is exactly right. If the stat totals cannot be
// read at all, we cannot know when they change, so rebuilding on a guess would be worse than not
// rebuilding.
static bool StatsProbeBroken = false;
static bool StatsProbeResolved = false;

static PClassActor *StatsActorClass = nullptr;
static PClass *StatTrackerClass = nullptr;
static PField *FldStatsTrackers = nullptr;
static PField *FldTrackerValue = nullptr;
static PField *FldTrackerPossible = nullptr;
static unsigned StatsTrackerCount = 0;

// The hash the current desktop was built against, and whether one has ever been taken. Separate from
// the value so that a legitimate hash of 0 - every tracker at zero, i.e. a brand-new game - is not
// confused with "not sampled yet".
static uint64_t StatsHashValue = 0;
static bool StatsHashValid = false;

// How long after a rebuild the next one is deferred, in milliseconds.
//
// THREE SECONDS, chosen against the two cases rather than as a round number. The case that must feel
// instant is finding a secret or a datapad out of combat: that is a single isolated change after a long
// quiet spell, so the cooldown has always expired and the rebuild is immediate - the cooldown costs it
// nothing at all. The case that must be bounded is sustained combat, where STAT_KILLS moves on every
// kill; at 3 s that is one rebuild per three seconds instead of one per kill.
//
// Wall time rather than gametics on purpose. The world is PAUSED while the PDA is open (mode 4 writes
// menuactive = MENU_On), and in mode 5 the two alternate, so a gametic count would stop advancing
// exactly when the player is looking at the panel and make the next rebuild after a long read appear to
// be inside the cooldown. I_msTimeF and never I_msTimeFS: the latter is measured from
// FirstFrameStartTime, which I_FreezeTime and I_ResetFrameTime both ADVANCE, so a level transition would
// make the delta go sharply negative - the same trap d_main.cpp's BenchTickMs comment records.
static const double StandbyRebuildCooldownMs = 3000.0;

// Wall time of the last rebuild, and how many distinct changes have been detected since. Zero-initialised
// rather than seeded, so the very first change is always a leading edge and rebuilds at once.
static double StandbyLastRebuildMs = 0.0;
static int StandbyPendingChanges = 0;

static bool StatsProbeResolve()
{
	FName statsName(StatsClassName, true);   // noCreate: never add a name to look one up
	PClassActor *statsCls = statsName != NAME_None ? PClass::FindActor(statsName) : nullptr;
	if (statsCls == nullptr)
	{
		Printf("AuxStandbyCodex: no %s inventory class - stat totals unreadable, "
			"the desktop will not refresh\n", StatsClassName);
		StatsProbeBroken = true;
		return false;
	}

	PClass *trackerCls = PClass::FindClass(StatTrackerClassName);
	if (trackerCls == nullptr)
	{
		Printf(TEXTCOLOR_YELLOW "AuxStandbyCodex: no %s class, the desktop will not refresh\n",
			StatTrackerClassName);
		StatsProbeBroken = true;
		return false;
	}

	// A failure to read the stats must NOT disable the desktop - a static desktop is the whole
	// pre-existing behaviour and is far better than none - so nothing here touches StandbyBroken and the
	// failure is recorded against StatsProbeBroken instead. ResolveField latches nothing of its own.
	PField *fldTrackers = ResolveField(statsCls, "trackers", Field_ObjArray, trackerCls);
	PField *fldValue = fldTrackers != nullptr ? ResolveField(trackerCls, "value", Field_Float, trackerCls) : nullptr;
	PField *fldPossible = fldValue != nullptr
		? ResolveField(trackerCls, "possibleValue", Field_Float, trackerCls) : nullptr;

	if (fldPossible == nullptr)
	{
		Printf(TEXTCOLOR_YELLOW "AuxStandbyCodex: %s does not look the way this code was written against, "
			"the desktop will not refresh\n", StatsClassName);
		StatsProbeBroken = true;
		return false;
	}

	StatsActorClass = statsCls;
	StatTrackerClass = trackerCls;
	FldStatsTrackers = fldTrackers;
	FldTrackerValue = fldValue;
	FldTrackerPossible = fldPossible;
	StatsTrackerCount = static_cast<PArray *>(fldTrackers->Type)->ElementCount;
	StatsProbeResolved = true;

	Printf("AuxStandbyCodex: %s.trackers resolved, %u trackers, desktop refreshes on stat changes\n",
		StatsClassName, StatsTrackerCount);
	return true;
}

// Hash every tracker's running totals. Returns false when there is nothing to read yet - no pawn, no
// Stats item - which is a normal transient state at the title screen, not a fault.
static bool StatsProbeHash(uint64_t &out)
{
	if (StatsProbeBroken)
		return false;

	if (consoleplayer < 0 || consoleplayer >= MAXPLAYERS || !playeringame[consoleplayer]
		|| players[consoleplayer].mo == nullptr)
	{
		return false;
	}

	if (!StatsProbeResolved && !StatsProbeResolve())
		return false;

	// subclass=true: Stats is a concrete Inventory the player carries, and a subclass of it would still
	// be the player's stats. Matches how i_auxcodex.cpp reaches ManualItem.
	AActor *stats = players[consoleplayer].mo->FindInventory(StatsActorClass, true);
	if (stats == nullptr)
		return false;

	// Straight-line from here: no allocation and nothing that can collect, so the array cannot be
	// rebuilt underneath the walk.
	const uint8_t *base = reinterpret_cast<const uint8_t *>(stats) + FldStatsTrackers->Offset;

	// FNV-1a over the raw bits of both doubles of every non-null tracker. The bit pattern rather than
	// the value because this is a change detector, not arithmetic - it never has to be interpreted.
	uint64_t hash = 0xcbf29ce484222325ULL;
	for (unsigned i = 0; i < StatsTrackerCount; i++)
	{
		DObject *tracker = *reinterpret_cast<DObject *const *>(base + (size_t)i * sizeof(DObject *));

		// The array's DECLARED element type was proved to be a StatTracker subclass when it was resolved;
		// this proves the object actually in the slot is one, which is what makes the StatTracker-relative
		// offsets below valid for this instance. A null slot is normal - Stats.init only creates the
		// trackers it knows about (stats.zs:299 onwards) - and is folded in as a distinguishable constant
		// so that a tracker appearing later still changes the hash.
		uint64_t bits = 0x9e3779b97f4a7c15ULL;
		if (tracker != nullptr && tracker->IsKindOf(StatTrackerClass))
		{
			double v, pv;
			memcpy(&v, reinterpret_cast<const uint8_t *>(tracker) + FldTrackerValue->Offset, sizeof(v));
			memcpy(&pv, reinterpret_cast<const uint8_t *>(tracker) + FldTrackerPossible->Offset, sizeof(pv));

			uint64_t vb, pb;
			memcpy(&vb, &v, sizeof(vb));
			memcpy(&pb, &pv, sizeof(pb));
			bits = vb ^ (pb * 0x100000001b3ULL);
		}

		hash = (hash ^ bits) * 0x100000001b3ULL;
	}

	out = hash;
	return true;
}

// Drop the desktop so the next update rebuilds it from scratch.
//
// DROPPING THE GC ROOT, not leaking. Nulling the globals the marker function reads is what makes the menu
// unreachable. Keeping the old menu rooted on every rebuild instead would accumulate 100+ DObjects per
// pickup across a playthrough, which is not acceptable even for a prototype.
//
// THE VIEW TREE IS SAFE TO COLLECT. There is no `override void OnDestroy()` anywhere in it: UIView's own is
// empty (view.zs:1111-1113), and all six real ones in the game are on classes that cannot be in the tree -
// three actors, a static handler, BBItem (an Inventory, and the one that would actually have mattered
// because its OnDestroy calls saveGlobalGame) and SaveSelacoMenu (a different menu). So nothing in the tree
// can reach SendNetworkEvent or a save.
//
// THE MENU ITSELF IS NOT, WHICH AN EARLIER VERSION OF THIS COMMENT GOT WRONG. It claimed no OnDestroy
// anywhere; that is true of the VIEWS but not of PDAMenu3, which overrides it at pda_menu.zs:973-976:
//
//     override void onDestroy() { SetMusicVolume(Level.MusicVolume); Super.onDestroy(); }
//
// ZScript identifiers are case-insensitive, so that IS DObject::OnDestroy, and SetMusicVolume is a static
// native straight onto I_SetMusicVolume (vmthunks_actors.cpp:105-111), which sets the global relative_volume
// (i_music.cpp:255-271). Nothing between PDAMenu3 and DObject declares onDestroy - checked in UIMenu,
// SelacoGamepadMenu and wadsrc's own Menu/GenericMenu - so this one line is the whole of it.
//
// NOTE WHAT THE BUG ACTUALLY IS, because it is not the pair of writes it looks like. init() does NOT duck
// the music: it only sets curMusicVolume/targetMusicVolume/musicVolumeSteps (pda_menu.zs:83-85), and the
// SetMusicVolume that acts on them is in ticker() at :821-824, which the standby codex never calls. So the
// standby codex ducks nothing and only ever performs the RESTORE - and it performs it at an arbitrary moment,
// whenever the GC happens to sweep, with no relationship to what the audio state is by then. The case that
// actually hurts: the player opens their real PDA, which legitimately ducks the music to 25% because that
// menu IS ticked, and a standby codex collected during that window snaps it back to full while they are reading.
//
// SO THE DESTROY IS MADE EXPLICIT AND NEUTRALISED, rather than left to the sweep. Destroying it here means
// the one script line runs at a known instant, inside a projection scope and a try/catch like every other
// script call in this file; snapshotting relative_volume across it makes the net effect exactly zero. The
// alternative - suppressing init()'s duck - would be fixing a write that does not happen.
//
// THE SOUND SUPPRESSION IN projectionscope.h DOES NOT SUBSUME THIS, and the reason is that onDestroy plays
// no sound at all. Its whole body is the two lines quoted above. PDAMenu3's MenuSound("codex/close") lives
// in animateClose() (pda_menu.zs:990-994), which is driven by the player dismissing their own PDA and is
// never called from here, so the discard path has nothing for the CHANF_UI guard to refuse. Music volume
// and UI sound are separate subsystems reached by separate natives - SetMusicVolume lands on
// I_SetMusicVolume, not on S_StartSound - so this snapshot stays exactly as load-bearing as it was.
static void StandbyViewDiscard()
{
	DObject *menu = StandbyMenu;

	StandbyMenu = nullptr;
	StandbyRootView = nullptr;

	// Cleared so nothing can draw through a stale root between the discard and the rebuild. Everything
	// else BuildStandbyView caches is reassigned by it, and StandbyAbsent/StandbyBroken are deliberately
	// untouched: a latched failure must stay latched.
	FuncStandbyDraw = nullptr;
	FuncStandbyDrawSubviews = nullptr;
	StandbyCanvas = nullptr;

	// Reset so the rebuilt desktop always re-selects, and so a selection that failed on the old menu is not
	// mistaken for one that succeeded on the new one.
	StandbyAppWantedIndex = -1;
	StandbyAppShownIndex = -1;

	if (menu == nullptr || (menu->ObjectFlags & OF_EuthanizeMe))
		return;

	// Read before, written back after: PDAMenu3.onDestroy is about to overwrite it with Level.MusicVolume.
	// relative_volume is the exact global I_SetMusicVolume writes (i_music.cpp:257), declared extern in
	// s_music.h, so this restores the audio state rather than a guess at what it should have been - and it is
	// written through the same path the game uses so the ZMusic setting and the cvar callback follow it.
	const float savedRelativeVolume = relative_volume;

	try
	{
		FProjectionScope projection;
		menu->Destroy();
	}
	catch (const std::exception &e)
	{
		// A VM abort in onDestroy would otherwise take the frame - and therefore the main screen - down.
		// Nothing to latch: the menu is already unrooted, so the worst case is the sweep finishing the job.
		Printf(TEXTCOLOR_YELLOW "AuxStandbyCodex: destroying %s aborted (%s)\n", StandbyClassName, e.what());
	}

	if (relative_volume != savedRelativeVolume)
		I_SetMusicVolume(savedRelativeVolume);
}

static bool StandbyViewUpdate(bool *outNeedsRedraw)
{
	if (StandbyAbsent || StandbyBroken)
		return false;

	if (StandbyMenu != nullptr && (StandbyMenu->ObjectFlags & OF_EuthanizeMe))
	{
		// Nothing should be able to destroy it - it is not in the menu stack and is referenced only
		// from here - so this is "the world is not what this code assumes", not a state to recover.
		Printf(TEXTCOLOR_YELLOW "AuxStandbyCodex: the desktop was destroyed, second-screen view disabled\n");
		StandbyMenu = nullptr;
		StandbyRootView = nullptr;
		StandbyBroken = true;
		return false;
	}

	// The two rebuild triggers, sampled every frame and BOTH cheap on the unchanged path: the codex
	// generation is one integer compare, and the stats hash is a walk of STAT_COUNT pointers plus two
	// doubles each with no allocation. They are independent on purpose - the generation moves on unlock
	// and publish-gate edges (i_auxcodex.cpp), the hash on anything the player picks up - and either one
	// alone leaves the desktop showing stale content.
	//
	// GATED ON THE REBUILD'S OWN PRECONDITIONS, which is not belt-and-braces. A discard is only safe if
	// the rebuild that follows it can actually succeed: BuildStandbyView refuses outside GS_LEVEL and
	// without a pawn, so discarding during a map change would throw away a WORKING desktop and leave
	// i_auxpanel.cpp falling back to the C++ test pattern until the next level finished loading. Waiting
	// costs nothing - the content is stale either way - and the triggers are still true when we get here.
	if (StandbyMenu != nullptr && gamestate == GS_LEVEL
		&& consoleplayer >= 0 && consoleplayer < MAXPLAYERS && playeringame[consoleplayer]
		&& players[consoleplayer].mo != nullptr)
	{
		bool changed = false;

		extern unsigned I_AuxCodexGeneration();
		const unsigned generation = I_AuxCodexGeneration();
		if (generation != StandbyGeneration)
		{
			StandbyGeneration = generation;
			changed = true;
		}

		uint64_t hash = 0;
		if (StatsProbeHash(hash))
		{
			if (!StatsHashValid || hash != StatsHashValue)
			{
				// The first successful sample is not a change: it is the baseline for the desktop that was
				// just built against it. Without this the desktop would be rebuilt once for nothing on the
				// first frame a pawn exists.
				if (StatsHashValid)
					changed = true;
				StatsHashValue = hash;
				StatsHashValid = true;
			}
		}

		// A THIRD trigger is deliberately ABSENT here. Changing which app is shown - whether from the player
		// closing their real PDA or from aux_standby_app moving - does NOT rebuild: it is handled by the
		// re-select block below, on the desktop that is already built. See there for why.

		// The baseline is advanced ABOVE, as each change is detected, rather than at the rebuild. That is
		// what makes coalescing lossless: every distinct change is counted exactly once and the eventual
		// rebuild reads whatever the totals are by then, so nothing is missed by not rebuilding for it.
		if (changed)
			StandbyPendingChanges++;

		// LEADING-EDGE DEBOUNCE WITH COOLDOWN, and the leading edge is the point. A hash change after a
		// quiet spell rebuilds on the SAME frame - finding a secret out of combat still feels instant,
		// which is the case the feature exists for. A change inside the cooldown only sets the counter, so
		// a burst collapses to one rebuild when the window expires.
		//
		// This is the alternative to hashing a subset of the trackers. STAT_KILLS is one of the eight the
		// window displays, so without a debounce sustained combat would rebuild - fresh init, ~30 ms
		// readback - once per kill. Hashing a subset would fix that by naming StatType indices, and
		// stats.zs:297 warns those are save-compatibility-critical; that trades a bounded cost now for an
		// unbounded correctness bug after a game patch. Debouncing keeps the whole-array hash and its
		// completeness property intact.
		const double nowMs = I_msTimeF();
		if (StandbyPendingChanges > 0 && nowMs - StandbyLastRebuildMs >= StandbyRebuildCooldownMs)
		{
			// Rebuilt rather than updated - see the header above this section. The remaining cost is accepted
			// and deliberately not made configurable: the whole init plus a ~30 ms readback drops a frame.
			// The codex no longer CHIRPS, which used to be the cost the player actually noticed here -
			// MenuSound("codex/open") still fires from PDAMenu3.init (pda_menu.zs:90), but it is a CHANF_UI
			// sound inside a projection scope and BuildStandbyView refuses it (projectionscope.h).
			// The app on show is NOT one of the costs any more: SelectStandbyApp runs at the end of the
			// rebuild with the same precedence as everywhere else, so the desktop comes back on the live
			// codex's last tab rather than on the default.
			//
			// The count is logged because a debounce and a MISSED DETECTION look identical on a device -
			// both are "the panel did not update when I expected". A number greater than 1 is positive proof
			// that the coalescing is what deferred the rebuild rather than the hash failing to notice.
			Printf("AuxStandbyCodex: stat or unlock change, rebuilding the desktop (%d change%s coalesced)\n",
				StandbyPendingChanges, StandbyPendingChanges == 1 ? "" : "s");
			StandbyViewDiscard();
		}
	}

	if (StandbyMenu == nullptr)
	{
		if (!BuildStandbyView())
			return false;

		// Re-baseline both triggers against the desktop that was just built, so the very next frame does
		// not immediately consider it stale again.
		extern unsigned I_AuxCodexGeneration();
		StandbyGeneration = I_AuxCodexGeneration();
		if (StatsProbeHash(StatsHashValue))
			StatsHashValid = true;

		// Start the cooldown from the rebuild that actually happened, not from the change that asked for
		// it, so the window is "time since the last rebuild" rather than "time since the last pickup".
		StandbyPendingChanges = 0;
		StandbyLastRebuildMs = I_msTimeF();

		if (outNeedsRedraw != nullptr)
			*outNeedsRedraw = true;
	}
	else if (AppSelectResolved && !AppSelectBroken && StandbyWantedIndex(nullptr) != StandbyAppWantedIndex)
	{
		// THE FALLING EDGE OF THE PLAYER CLOSING THEIR PDA, AND WHY IT IS NOT A REBUILD.
		//
		// The standby codex's PDAMenu3 is a DIFFERENT INSTANCE from the engine's CurrentMenu - mode 3 constructs
		// its own with PClass::CreateNew and never puts it in the menu stack - so when the player closes
		// theirs, ours is untouched, still rooted, still laid out, still drawable. Everything that has to
		// change is one switchToAppWindow, the close sweep and six setSelected calls. Rebuilding instead
		// would re-run PDAMenu3.init: 100+ DObject allocations and a dropped frame, all to arrive at the same
		// tree with a different app on top. So this is a re-select plus one redraw request and nothing else.
		//
		// It is not on an edge detector either, but on "what we want differs from what we selected", which
		// covers three cases with one test and no state machine: the player closed their PDA on a different
		// app, aux_standby_app moved, or the first sample from the live codex arrived. Cheap enough to sit
		// on the per-frame path - StandbyWantedIndex is at most twelve pointer and int compares with no
		// lookups, no allocation and no VM call.
		//
		// IN MODE 5 THIS RUNS ON EXACTLY THE RIGHT FRAMES. i_auxpanel.cpp resolves the mode per frame and
		// only asks mode 3 for an update while the PDA is SHUT, so the first mode-3 frame after a close is
		// the first frame this test can see the new value - which is the frame the panel should change on.
		//
		// The new window is laid out by the NEXT DRAW rather than here: PDAFullscreenAppWindow.init sets
		// requiresLayout (app_window.zs:283), UIView.drawSubviews calls layoutIfNecessary on every subview
		// it walks (view.zs:464) and UIView.draw calls it on itself (:439), and layoutIfNecessary's
		// argument-less layout() takes the (0,0)/-1 sentinels that derive cScale and cAlpha from the view's
		// own chain (view.zs:762-764) - so it inherits the scale calcScale installed at build time instead
		// of overwriting it. That first layout reaches savePos(), which is why StandbyViewDraw's own
		// FProjectionScope is load-bearing here too.
		if (SelectStandbyApp(StandbyMenu, AppMenuClass, AppViewClass) && outNeedsRedraw != nullptr)
			*outNeedsRedraw = true;
	}

	// AND THE SIZE THE PANEL DRAWS AT, retuned live on the desktop that is already built. Placed
	// after the chain above rather than inside it because it is orthogonal to both: a rebuild lays out at the
	// current zoom itself and baselines it, so this is a no-op on that frame, and a re-select changes
	// which app is on top without changing the box it is drawn in. Cheap on the unchanged path - one clamped
	// cvar read and one double compare, no lookups, no allocation and no VM call.
	StandbyRetune(outNeedsRedraw);

	return true;
}

static void StandbyViewDraw()
{
	if (StandbyAbsent || StandbyBroken || StandbyRootView == nullptr)
		return;

	try
	{
		// The projection scope is needed on the DRAW path too, not only the build: UIView.draw() calls
		// layoutIfNecessary() (view.zs:439), which can reach PDAAppWindow.layout -> savePos ->
		// SendNetworkEvent("pdaAppPos:...") on any frame a widget invalidated its layout.
		FProjectionScope projection;

		// No draw-path write to menuactive is known, but the cost of covering it is one stack word and
		// the failure it prevents - a permanently wrong menuactive - lasts the rest of the session.
		FMenuActiveKeeper keepMenuState;

		// mainView.draw() then mainView.drawSubviews(), never the menu's own drawer() or drawSubviews():
		// UIMenu.drawSubviews wraps the tree in Screen.EnableStencil/ClearStencil, pushing stencil state
		// into the MAIN screen's twod, and calls animator.step()/testMouse() which mutate the tree
		// mid-draw (menu.zs:278-289); PDAMenu3.drawer() hardcodes Screen.GetWidth/setClipRect/DrawTexture
		// (pda_menu.zs:1168-1197).
		VMValue params[] = { StandbyRootView };
		VMCall(FuncStandbyDraw, params, 1, nullptr, 0);
		VMCall(FuncStandbyDrawSubviews, params, 1, nullptr, 0);
	}
	catch (const std::exception &e)
	{
		Printf(TEXTCOLOR_YELLOW "AuxStandbyCodex: drawing %s aborted (%s), second-screen view disabled\n",
			StandbyClassName, e.what());
		StandbyBroken = true;
		// Fall through to the clip reset: an abort part way through drawSubviews leaves whatever clip
		// rect the last setClip installed.
	}

	if (StandbyCanvas != nullptr)
		StandbyCanvas->Drawer.ClearClipRect();
}

// ---------------------------------------------------------------------------------------------
// The two entry points: decide whether there is anything new, then draw it.
// ---------------------------------------------------------------------------------------------
//
// These are separate because the readback is edge-triggered and expensive (~30 ms in-level; see
// i_auxpanel.cpp), so the caller has to be able to ask "is there new content?" without paying for a
// draw to find out. Update is cheap on the overwhelmingly common no-change path: two latched bools,
// a null test and one integer compare against the codex generation.
//
// The mode arrives as an argument rather than being read from aux_codex_mode here, because
// i_auxpanel.cpp's I_AuxPanelFrame is the single place canvas ownership is decided and a second
// reader of that cvar is a second place for the two to disagree. Mode 3 is the only value that means
// anything to this file; anything else is answered as "no content", which the caller turns into a
// dropped frame and Selaco's startup splash.

bool I_AuxStandbyCodexUpdate(int mode, bool *outNeedsRedraw)
{
	if (outNeedsRedraw != nullptr)
		*outNeedsRedraw = false;

	return mode == AuxMode_Standby && StandbyViewUpdate(outNeedsRedraw);
}

void I_AuxStandbyCodexDraw(int mode)
{
	if (mode == AuxMode_Standby)
		StandbyViewDraw();
}

// Forget every resolve this file latched, for the restart teardown. See I_AuxForgetScriptState in
// i_auxvmreflect.cpp for why it exists and when it runs.
//
// NOT StandbyViewDiscard, and that is the whole reason this function is separate from it: Discard calls
// menu->Destroy(), which dispatches PDAMenu3's scripted onDestroy (pda_menu.zs:973) and through it
// I_SetMusicVolume. Both are illegal at teardown - the sound system is already down and the classes are
// about to be deleted - so the menu is simply unrooted here and left to the collection inside
// PClass::StaticShutdown, which runs with bVMOperational already false and therefore calls no script at all.
//
// THE THREE MARKED POINTERS COME FIRST because they are the crash: the marker function registered in
// BuildStandbyView stays in GC's marker array for the life of the process (there is no RemoveMarkerFunc),
// so anything still here is walked by the first collection after the restart, by which time its PClass has
// been deleted. The rest is the remainder the audit found - every one-shot resolve, so that the next init
// re-resolves against the freshly parsed script instead of reusing a freed PField's offset.
void I_AuxStandbyCodexForgetScriptState()
{
	StandbyMenu = nullptr;
	StandbyRootView = nullptr;
	StandbyPendingApp = nullptr;

	// StandbyAbsent and StandbyBroken are cleared here although StandbyViewDiscard deliberately leaves them
	// latched, because the two cases are not the same one. Discard keeps a verdict about script that is still
	// loaded; a restart can load an entirely different wad set, so the old verdict is about a game that is no
	// longer running. The cost of being wrong is one re-attempt and one repeated yellow line.
	StandbyAbsent = false;
	StandbyBroken = false;

	// The canvas is a DObject owned by the AUXCANVAS FCanvasTexture, and D_Cleanup's TexMan.DeleteAll()
	// destroys the texture, which unlinks the FCanvas from AllCanvases - its only GC root - and nulls it.
	StandbyCanvas = nullptr;
	FuncStandbyDraw = nullptr;
	FuncStandbyDrawSubviews = nullptr;
	StandbyGeneration = 0;

	// The app-select group, which BuildStandbyView does NOT reassign: it resolves once for the process and
	// is gated by AppSelectResolved, so without this the first post-restart selection reads tab offsets out
	// of freed PFields and calls freed VMFunctions.
	AppSelectResolved = false;
	AppSelectBroken = false;
	AppWindowClass = nullptr;
	AppTabClass = nullptr;
	FldStandbyView = nullptr;
	FldViewParentMenu = nullptr;
	FldTabDisabled = nullptr;
	FuncNumSubviews = nullptr;
	FuncViewAt = nullptr;
	FuncViewAdd = nullptr;
	FuncAppClose = nullptr;
	FuncSwitchToApp = nullptr;
	FuncTabSetSelected = nullptr;
	ViewAtRegs = 0;
	ViewAddRegs = 0;
	SwitchToAppRegs = 0;
	SetSelectedRegs = 0;
	AppMenuClass = nullptr;
	AppViewClass = nullptr;
	for (unsigned i = 0; i < countof(StandbyApps); i++)
	{
		AppClasses[i] = nullptr;
		AppVInit[i] = nullptr;
		AppVInitRegs[i] = 0;
		AppTabFields[i] = nullptr;
	}
	StandbyAppWantedIndex = -1;
	StandbyAppShownIndex = -1;

	// The wide-box relayout group. FuncStandbyCalcScale doubles as the group's "resolved" flag, which is
	// what StandbyRetune tests before calling through the rest of them.
	FuncStandbyCalcScale = nullptr;
	FuncStandbyMenuLayout = nullptr;
	FuncStandbyViewLayout = nullptr;
	FldStandbyUIScaling = nullptr;
	FldStandbyLastUIScale = nullptr;
	StandbyCalcScaleRegs = 0;
	StandbyViewLayoutRegs = 0;
	StandbyPdaClass = nullptr;
	StandbyViewClass = nullptr;
	StandbyLayoutZoom = 1.0;

	// The stat probe, which walks Stats.trackers through raw field offsets - the one place here where a
	// stale PField is a read of arbitrary object bytes rather than a missed call. StatsHashValid is cleared
	// so the first post-restart sample is taken as a new baseline instead of being reported as a change,
	// which would otherwise route straight back into StandbyViewDiscard.
	StatsProbeBroken = false;
	StatsProbeResolved = false;
	StatsActorClass = nullptr;
	StatTrackerClass = nullptr;
	FldStatsTrackers = nullptr;
	FldTrackerValue = nullptr;
	FldTrackerPossible = nullptr;
	StatsTrackerCount = 0;
	StatsHashValue = 0;
	StatsHashValid = false;
	StandbyLastRebuildMs = 0.0;
	StandbyPendingChanges = 0;
}
