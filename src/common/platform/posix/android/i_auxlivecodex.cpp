/*
** i_auxlivecodex.cpp
** The live codex: the player's OWN PDA, redirected onto the AYN Thor's second screen.
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
** Why the handful of script symbols here are reached by VM reflection rather than by a compile-time
** type: see WHY THIS IS C++ AND NOT ZSCRIPT at the top of i_auxstandbycodex.cpp, and the safety argument
** for the resolvers themselves in i_auxvmreflect.h.
*/

#include <exception>

// dobjtype.h and dobjgc.h are not self-contained (they lean on FName, FString and the DObject
// declaration being in scope already), so dobject.h leads - it is the header that pulls that chain
// in, and every engine translation unit that touches PClass reaches it the same way.
#include "dobject.h"
#include "auxmenuredirect.h"  // FAuxMenuRedirect, mode 4's guard; implemented at the bottom of this file
#include "dobjtype.h"
#include "menu.h"        // DMenu, CurrentMenu
#include "menustate.h"   // menuactive, which the redirect forces to MENU_On
#include "printf.h"
#include "symbols.h"
#include "textures.h"    // FCanvas
#include "v_2ddrawer.h"
#include "v_draw.h"      // twod, the global mode 4 swaps
#include "vm.h"
#include "zstring.h"

#include "i_auxcodexviewstate.h"   // the view-state manager, which is all mode 4 hands its samples to
#include "i_auxvmreflect.h"

using namespace AuxView;

// ---------------------------------------------------------------------------------------------
// MODE 4: "WII U MODE" - the PLAYER'S OWN PDA, on the second screen.
// ---------------------------------------------------------------------------------------------
//
// Mode 3 is a PROJECTION: nobody asked for it, so it builds a private copy of Selaco's UI, never ticks
// it, never takes input, and is structurally forbidden from recording anything (FProjectionScope).
// Mode 4 is the opposite of all four of those things and the code is much smaller as a result.
//
// Here the player really did press the codex button. PDAMenu3 is CurrentMenu, the engine ticks it,
// M_Responder routes the gamepad to it and M_Drawer calls its drawer() every frame - all of that is
// stock engine behaviour that already works. The ONE thing changed is `twod`, swapped to the canvas
// drawer for the duration of M_Drawer (see FAuxMenuRedirect below and DrawOverlays in d_main.cpp), so
// the menu's 2D output lands on the second screen instead of over the game.
//
// EXPLICITLY NO FProjectionScope. openEntry's "mansetcurrent", savePos's "pdaAppPos:..." and
// PDAReaderWindow's "pdaUnreadClear" are the player's own actions here and MUST reach the savegame.
// Suppressing them - the correct thing in mode 3 - would silently stop the game recording which
// codex entry the player is reading.
//
// WHY EVERY Screen.* CALL FOLLOWS THE SWAP, which is what makes the whole idea cheap: Screen.GetWidth
// and GetHeight return twod->GetWidth()/GetHeight() (v_framebuffer.cpp:342-356), and every Screen draw
// native writes to `twod` (v_draw.cpp), including EnableStencil/SetStencil/ClearStencil (:1907-1959)
// which UIMenu.drawSubviews wraps the whole tree in. So PDAMenu3.drawer's hardcoded
// Screen.GetWidth/setClipRect/DrawTexture (pda_menu.zs:1168-1197) and every widget below it retarget
// for free. Stencil works on a canvas because RenderTextureView binds a real depth+stencil attachment
// (vk_renderdevice.cpp:1149, created at vk_hwtexture.cpp:113-141) and Draw2D clears stencil on entry
// (hw_draw2d.cpp:67-70).
//
// FOUR THINGS THE SWAP ALONE DOES NOT COVER. Each of these silently produces either nothing on the
// panel or damage to the main screen, and each is handled below rather than hoped about:
//
// 1. isIn2D IS PER DRAWER. HasBegun2D() reads twod->isIn2D (v_2ddrawer.h:248), Begin() is the only
//    thing that sets it, and Begin() is only ever called on the MAIN drawer (d_main.cpp:1118, :1165,
//    :3853). GetTextureCanvas gives the canvas drawer SetSize() and nothing else (v_2ddrawer.cpp:1270),
//    so a bare swap leaves isIn2D false and the FIRST Screen draw native throws "Attempt to draw to
//    screen outside a draw function" (v_draw.cpp:260). Begin() also sets Width/Height, which is exactly
//    what makes the menu system believe the screen is 1240x1080 - so one call fixes both.
//
// 2. NOTHING MARKS THE TEXTURE DIRTY. The FCanvas draw natives call self->Tex->NeedUpdate() themselves
//    (v_draw.cpp:282 and friends); the _Screen natives have no texture to mark and do not. The
//    AllCanvases loop gates on CheckNeedsUpdate() (hw_entrypoint.cpp:365), so without an explicit
//    NeedUpdate the canvas is skipped entirely and the panel shows nothing at all.
//
// 3. BlurScene IS NOT A 2D DRAW. menu.cpp:903 calls screen->BlurScene() unless CurrentMenu->DontBlur,
//    and PDAMenu3.init sets DontBlur = false (pda_menu.zs:80). It is a scene-level postprocess on the
//    MAIN framebuffer, so the swap does not redirect it - left alone the main screen blurs for a menu
//    that is not on the main screen. DontDim needs nothing: PDAMenu3 sets it true (pda_menu.zs:79), and
//    sysCallbacks.LiveDim has no assignment anywhere in this tree so it is a no-op regardless.
//
// 4. THE MENU RELAYOUTS ITSELF AGAINST WHATEVER Screen.GetWidth() SAYS, and mode 4 makes that answer
//    change depending on where in the frame you ask. See LiveTune.
//
// AND ONE THING THAT IS NOT ABOUT CORRECTNESS AT ALL: aux_codex_size, the same cvar and the same
// clamp mode 3 uses, because 1240x1080 at arm's length is too small to read whichever mode put it there.
// The arithmetic is free - it divides the baseline height LiveTune hands calcScale, nothing more - but it
// is the one player-facing value here that can move while the menu is LIVE, and in a mode with no
// FProjectionScope a relayout is a real write to the player's savegame. So WHEN it is applied is the
// delicate part rather than what it computes: see the compare in FAuxMenuRedirect's constructor.
//
// THE FRAME ORDERING, which is the single most likely thing to make this draw nothing. Within
// D_Display: the AllCanvases flush lives in RenderView (hw_entrypoint.cpp:362-375), called at
// d_main.cpp:1115; DrawOverlays -> M_Drawer is at :907, reached from :1254. So commands pushed by the redirected M_Drawer
// in frame N are rendered by frame N+1's RenderView, which then Clear()s them - they are NOT lost, and
// the panel is consistently ONE FRAME behind. Do not "fix" that by moving the redirect earlier: before
// twod->Begin at :1108 the main drawer is not open either, and the readback path in i_auxcanvas.cpp is
// built around exactly this ordering.

// How often a redirected frame is actually published to the panel, in frames.
//
// TWO is the pipeline minimum and there is no point going below it. A publish takes three distinct
// points in time: frame N's DrawOverlays queues the commands and marks the texture, frame N+1's
// RenderView is the earliest they can reach the image, and frame N+2's I_AuxPanelFrame - which runs
// before BeginFrame, at the TOP of the frame - is the earliest I_AuxCanvasRenderPending can report the
// render as done. Because the readback sits at the top of a frame and the publish at the bottom of the
// same one, those cycles overlap and the period is 2, not 3. A smaller value would only stall the
// phase-2 gate, not update the panel faster.
//
// 2 is chosen over something calmer even though each readback costs ~30 ms in-level (see i_auxpanel.cpp)
// because mode 4's content is LIVE and INTERACTIVE - the player is moving a selection with the gamepad
// and latency is what decides whether the panel is usable - and because mode 4 pauses the world, so the
// frame time it gives up buys nothing back. Performance is explicitly not a constraint for this mode.
static const int AuxLivePublishInterval = 2;

// The menu class we host, and the ancestors every write below assumes. PDAMenu3 is a UIMenu (hence
// mainView, drawCanvas, ignoreUIScaling and ui_scaling) and a DMenu (hence being CurrentMenu at all).
static const char *const LivePdaClassName = "PDAMenu3";

// Two latches with the same split of meaning as the other modes'. Absent is the expected outcome on
// Doom and on the demo and says one line then nothing; broken is a real diagnostic. On either, mode 4
// reports the PDA as not open, so the redirect never engages and M_Drawer draws the menu to the MAIN
// screen exactly as it would without this file. Failing towards the main screen is deliberate: a PDA
// the player cannot see anywhere is far worse than a PDA that is merely not on the panel.
static bool LiveAbsent = false;
static bool LiveBroken = false;

static PClass *LivePdaClass = nullptr;
static PClass *LiveCodexClass = nullptr;
static FCanvas *LiveCanvas = nullptr;

static VMFunction *FuncLiveCalcScale = nullptr;
static VMFunction *FuncLiveLayout = nullptr;
static VMFunction *FuncLiveCodexLayout = nullptr;
static int LiveCalcScaleRegs = 0;
static int LiveCodexLayoutRegs = 0;

static PField *FldLiveMainView = nullptr;
static PField *FldLiveUIScaling = nullptr;
static PField *FldLiveIgnoreUIScaling = nullptr;
static PField *FldLiveDrawCanvas = nullptr;

// The coupling to mode 3, and the ONE optional group mode 4 resolves. Null here means the dashboard does
// not follow the live codex and falls back to aux_standby_app - a lost feature, not a broken mode, which
// is why a failure to resolve these must not set LiveBroken.
static PClass *LiveAppWindowClass = nullptr;
static PField *FldLiveCurrentAppWindow = nullptr;

static bool LiveResolved = false;

// The instance LiveTune has already been applied to, compared by POINTER and never dereferenced while
// stale. Cleared on the falling edge of "the PDA is open" so that a new menu landing on the recycled
// address of the old one is still tuned - a pointer compare alone would silently skip it.
static DObject *LiveTuned = nullptr;

// And the zoom it was tuned with, which is the SECOND half of that guard. Kept here rather than read from
// the cvar at the point of use because what has to be compared is the zoom already baked into the live
// layout, not the zoom the player currently wants. Not cleared alongside LiveTuned: a null LiveTuned
// already forces a tune, so the stale value can never be acted on.
static double LiveTunedZoom = 1.0;

// Whether the panel hook ran this frame with mode 4 selected. The guard also re-tests that the PDA is
// open, so all this really carries is "there is a live panel and mode 4 is on" - a slowly changing
// condition where one frame of staleness costs at most one redirected-and-discarded M_Drawer.
static bool LiveArmed = false;

// Whether the guard should mark the texture dirty and arm the readback when it finishes drawing. The
// decision is made at the top of the frame, in I_AuxLiveCodexFrame, because that is where the readback
// state it has to agree with lives.
static bool LiveWantPublish = false;

static bool LiveWasOpen = false;
static int LiveFramesSincePublish = 0;

// Resolve everything mode 4 calls or writes. Runs once, on the first frame the PDA is actually open -
// never at startup, because PClass::FindClass cannot answer before the scripts are compiled and a
// premature lookup would latch LiveAbsent forever on a game that does have the class.
static bool LiveResolve()
{
	PClass *cls = PClass::FindClass(LivePdaClassName);
	if (cls == nullptr)
	{
		// The Doom and demo path, and the only outcome here that is not a diagnostic.
		Printf("AuxLiveCodex: no %s class - not the full Selaco, second-screen PDA unavailable\n", LivePdaClassName);
		LiveAbsent = true;
		return false;
	}

	// Two ancestry checks because two different things are assumed: DMenu is what lets it be
	// CurrentMenu and carry DontBlur, UIMenu is what guarantees mainView, drawCanvas, ui_scaling and
	// ignoreUIScaling exist.
	if (!cls->IsDescendantOf(RUNTIME_CLASS(DMenu)) || !cls->IsDescendantOf(FName(MenuClassName, true)))
	{
		Printf(TEXTCOLOR_YELLOW "AuxLiveCodex: %s is not a %s, second-screen PDA disabled\n",
			LivePdaClassName, MenuClassName);
		LiveBroken = true;
		return false;
	}

	PClass *viewCls = PClass::FindClass(ViewClassName);
	if (viewCls == nullptr)
	{
		Printf(TEXTCOLOR_YELLOW "AuxLiveCodex: no %s class, second-screen PDA disabled\n", ViewClassName);
		LiveBroken = true;
		return false;
	}

	FCanvas *canvas = GetTextureCanvas(AuxCanvasName);
	if (canvas == nullptr || canvas->Tex == nullptr)
	{
		Printf(TEXTCOLOR_YELLOW "AuxLiveCodex: %s is not a canvas texture, second-screen PDA disabled\n", AuxCanvasName);
		LiveBroken = true;
		return false;
	}

	static const EArgKind CalcScaleArgs[] = { Arg_Int, Arg_Int, Arg_Vector2 };
	static const EArgKind ViewLayoutArgs[] = { Arg_Vector2, Arg_Float, Arg_Bool };

	// Mode 4 is not the standby codex and must not report as one. The note is the consequence this group
	// actually has - every symbol below is required, so a miss sets LiveBroken and the live codex stops
	// existing - and it is the same wording the two Printfs in this function already use.
	static const char *const Subsystem = "AuxLiveCodex";
	static const char *const Note = "second-screen PDA disabled";

	int calcScaleRegs = 0, liveLayoutRegs = 0, viewLayoutRegs = 0;
	// calcScale(int, int, Vector2) is FIVE registers for three declared arguments - a Vector2 is two
	// (types.cpp:365). ResolveMethod proves that against the callee's own NumArgs.
	VMFunction *funcCalcScale = ResolveMethod(cls, "calcScale", Subsystem, CalcScaleArgs, 3, &calcScaleRegs,
		nullptr, Note);
	VMFunction *funcLiveLayout = funcCalcScale != nullptr
		? ResolveMethod(cls, "layout", Subsystem, nullptr, 0, &liveLayoutRegs, nullptr, Note) : nullptr;
	VMFunction *funcViewLayout = funcLiveLayout != nullptr
		? ResolveMethod(viewCls, "layout", Subsystem, ViewLayoutArgs, 3, &viewLayoutRegs, nullptr, Note) : nullptr;

	PField *fldMainView = funcViewLayout != nullptr
		? ResolveField(cls, "mainView", Field_ViewPtr, viewCls, Subsystem, Note) : nullptr;
	PField *fldUIScaling = fldMainView != nullptr
		? ResolveField(cls, "ui_scaling", Field_CVarPtr, viewCls, Subsystem, Note) : nullptr;
	PField *fldIgnore = fldUIScaling != nullptr
		? ResolveField(cls, "ignoreUIScaling", Field_Bool, viewCls, Subsystem, Note) : nullptr;
	PField *fldDrawCanvas = fldIgnore != nullptr
		? ResolveField(cls, "drawCanvas", Field_CanvasPtr, viewCls, Subsystem, Note) : nullptr;

	// THE COUPLING, and the one OPTIONAL group here. currentAppWindow (pda_menu.zs:49) is the only handle mode
	// 4 has on what the player is actually looking at, so it is what everything the view-state manager samples
	// is read off - the app, the open datalog entry and the scroll position alike. Without it the panel keeps
	// using aux_standby_app and opens documents at the top, which is the behaviour that shipped before any of
	// this. Resolved after the required group so a miss cannot be mistaken for one, and excluded from `ok`
	// below so it cannot disable the second-screen PDA - losing the panel entirely to save a nicety is the
	// wrong trade.
	PClass *appWindowCls = PClass::FindClass(AppWindowClassName);
	PField *fldCurrentApp = appWindowCls != nullptr && appWindowCls->IsDescendantOf(viewCls)
		? ResolveField(cls, "currentAppWindow", Field_ViewPtr, appWindowCls, Subsystem,
			"the panel falls back to aux_standby_app and opens documents at the top") : nullptr;

	const bool ok = fldDrawCanvas != nullptr;

	if (!ok)
	{
		// ResolveMethod/ResolveField already printed which symbol failed; all that is left is to record
		// it against mode 4. Neither resolver latches anything of its own - see i_auxvmreflect.h.
		Printf(TEXTCOLOR_YELLOW "AuxLiveCodex: %s does not look the way this code was written against, "
			"second-screen PDA disabled\n", LivePdaClassName);
		LiveBroken = true;
		return false;
	}

	LivePdaClass = cls;
	LiveCodexClass = viewCls;
	LiveCanvas = canvas;
	FuncLiveCalcScale = funcCalcScale;
	FuncLiveLayout = funcLiveLayout;
	FuncLiveCodexLayout = funcViewLayout;
	LiveCalcScaleRegs = calcScaleRegs;
	LiveCodexLayoutRegs = viewLayoutRegs;
	FldLiveMainView = fldMainView;
	FldLiveUIScaling = fldUIScaling;
	FldLiveIgnoreUIScaling = fldIgnore;
	FldLiveDrawCanvas = fldDrawCanvas;
	LiveAppWindowClass = fldCurrentApp != nullptr ? appWindowCls : nullptr;
	FldLiveCurrentAppWindow = fldCurrentApp;
	LiveResolved = true;

	if (fldCurrentApp == nullptr)
	{
		Printf(TEXTCOLOR_YELLOW "AuxLiveCodex: cannot read %s.currentAppWindow - the standby codex will not "
			"follow the live codex and will stay on aux_standby_app at the top of every document\n",
			LivePdaClassName);
	}

	Printf("AuxLiveCodex: %s resolved, second-screen PDA ready on %s at %gx%g\n",
		LivePdaClassName, AuxCanvasName, AuxCanvasWidth, AuxCanvasHeight);
	return true;
}

// Record what the player is looking at in their OWN PDA, so mode 3 can put the panel back on it when they
// close it. Called on every frame the real PDA is open.
//
// WHAT IS SAMPLED, AND WHERE IT GOES. The app window is handed to the view-state manager
// (i_auxcodexviewstate.h), which takes three things off it: the app's class, and - when it is the reader -
// which datalog entry is open and how far down it the player has scrolled. All three live there rather than
// here so that neither mode owns the other's state; the full argument is in that header.
//
// READ-ONLY, WHICH IS NOT NEGOTIABLE. This is the PLAYER'S menu. The manager writes nothing to it and calls
// no script on this path at all, so mode 4 remains exactly the pass-through it was: the engine ticks the
// menu, M_Responder drives it and M_Drawer draws it, and the only thing this file changes is where the
// pixels land.
//
// SAMPLED EVERY FRAME RATHER THAN READ ON THE FALLING EDGE, and that is not caution. M_ClearMenus destroys
// the menu when it closes (menu.cpp:946), so by the time any "is the PDA open" predicate goes false the
// instance may already be euthanized - and reading a field off it then is reading freed memory to answer a
// question we could have answered a frame earlier for free. A per-frame handful of object-field loads costs
// nothing, and it means a kill mid-session loses nothing and there is no close edge to miss.
//
// currentAppWindow AND NOT currentApp, which is the part that had to be checked rather than assumed.
// PDAMenu3 does declare `int currentApp` at pda_menu.zs:52 - it looks exactly like the right field - but a
// grep of the entire extracted ipk3 (zscript/, zscripts/, MENUDEF.zsc, ACS) finds that ONE line and nothing
// else: it is never written and never read anywhere in Selaco. It would therefore sample 0 forever, and 0 is
// not even a valid PDA_APP_ID (PDA_APP_READER is 1), so the panel would silently never follow anything.
//
// currentAppWindow is unambiguous instead: it is set by switchToAppWindow (pda_menu.zs:735), which is the
// single funnel every app switch in the PDA goes through - the six openX() helpers at :515-609, the tab
// handler at :880-900, and the click-to-raise in mouseDownEvent at :961 - and it is repaired to the topmost
// remaining window when one is closed (:935-945). It is also the only handle that reaches the READER
// INSTANCE, which is what the entry and the scroll have to be read off; there is no parallel path to it and
// none is added.
static void LiveSampleCurrentApp()
{
	if (FldLiveCurrentAppWindow == nullptr || LiveAppWindowClass == nullptr || CurrentMenu == nullptr)
		return;

	DObject *app = ReadObjectField(CurrentMenu, FldLiveCurrentAppWindow);

	// The field's DECLARED type was proved to be a PDAAppWindow when it was resolved; this proves the object
	// in it is one. Null is normal - handleControl clears it when the last window closes (pda_menu.zs:936) -
	// and is deliberately NOT propagated: "the player closed every window" is not a new view state, so the
	// last real one stands.
	if (app != nullptr && !(app->ObjectFlags & OF_EuthanizeMe) && app->IsKindOf(LiveAppWindowClass))
		AuxCodexView::SampleAppWindow(app);
}

// Is the player's own PDA the current menu? Null-safe on every game: PClass::FindClass returns nullptr
// on Doom and on the demo, LiveAbsent latches, and this answers false forever after.
static bool LiveIsPdaOpen()
{
	bool open = false;

	// Cheapest tests first, and they are also what makes the lazy resolve safe: a menu cannot exist
	// before the scripts that declare it have been compiled.
	//
	// A descendant counts: PDAMenu3 is what Selaco opens today, and a subclass would still be the
	// player's PDA. A modal child menu pushed ON TOP of it is NOT a descendant, so mode 4 stands down
	// for it and that child draws on the main screen - which is where a confirmation prompt belongs.
	if (!LiveAbsent && !LiveBroken && CurrentMenu != nullptr && menuactive != MENU_Off
		&& (LiveResolved || LiveResolve()))
	{
		open = CurrentMenu->IsKindOf(LivePdaClass);
	}

	// SAMPLED HERE, which is the only place that knows CurrentMenu really is a PDAMenu3 and is called on
	// every frame the PDA is open in both mode 4 and mode 5 - in mode 5 because the dispatcher resolves to 4
	// exactly when this returns true (i_auxpanel.cpp:417-421). Idempotent, so the second call the redirect
	// makes later in the same frame is harmless; that later one is in fact the more current of the two,
	// because it runs after the tick that could have switched apps.
	//
	// PURE MODE 3 DOES NOT SAMPLE, and that is a deliberate boundary rather than an oversight: nothing calls
	// this on a mode-3 frame, and making mode 3 call it would drag mode 4's lazy LiveResolve - and therefore
	// mode 4's latches - onto mode 3's per-frame path, which is exactly the cross-contamination the split
	// between these two files is arranged to avoid. In pure mode 3 the dashboard stays on aux_standby_app.
	// Mode 5 is the shipping combination and the one the coupling is for.
	if (open)
		LiveSampleCurrentApp();

	// Forget the tuned instance the moment the PDA stops being the current menu, and this is NOT tidying -
	// it is what makes the pointer comparison in the guard safe. LiveTuned is compared by ADDRESS, and a
	// new PDAMenu3 is very likely to land on the freed one's address: same class, same size, same
	// allocator. A stale LiveTuned would therefore make the SECOND and every subsequent open skip
	// LiveTune entirely and draw a desktop that was never relaid out - the clipped tab strip, back again,
	// but only after the first open, which is exactly the kind of bug that gets called intermittent.
	//
	// Cleared HERE rather than on an edge inside I_AuxLiveCodexFrame on purpose: in mode 5 that function
	// is not called at all on frames the PDA is shut, so an edge there would never fire and the bug would
	// be reachable only in the combined mode.
	if (!open)
		LiveTuned = nullptr;

	return open;
}

// Exported so the combined mode (5) can ask who should own the canvas this frame without any of mode 4's
// state having to be unpicked. Mode 3 (the standby codex) and mode 4 (the live codex) share only the
// canvas and the readback flag, so the dispatcher in i_auxpanel.cpp just resolves to one of them per
// frame; both edges then fall out of the existing mode-change trigger.
//
// Note this has a deliberate side effect - it forgets a tuned menu instance when the PDA is shut - so it
// must be called every frame in any mode that uses it, which the dispatcher does.
bool I_AuxLiveCodexIsPdaOpen()
{
	return LiveIsPdaOpen();
}

// Make the menu lay itself out for a 1240x1080 panel with a 1920-wide design, once per PDA instance.
//
// THE PROBLEM IS NOT THE SIZE, IT IS THAT THE ANSWER MOVES. Swapping twod already makes
// Screen.GetWidth()/GetHeight() report 1240x1080, and UIMenu.drawer() calls
// layoutChange(Screen.GetWidth(), Screen.GetHeight()) itself (menu.zs:260-263), so on the face of it
// the menu would retarget with no help at all. Two things break that:
//
//   THE HEIGHT-ONLY SCALE. PDAMenu3.calcScale is newScale = ui_scaling * CLAMP(height/baseline.y,
//   0.599, 2.0) (pda_menu.zs:785-808) and never looks at the width. At 1240x1080 against the default
//   (1920, 1080) baseline that is ui_scaling exactly - 1.0, or 1.2 with Selaco's handheld profile - so
//   mainView ends up 1240 or 1033 logical pixels wide against a design that caps innerView at 1920
//   (pda_menu.zs:103) and the tab strip loses characters off both ends. Mode 3 already solved this and
//   this copies it: pass calcScale a baseline height derived from the canvas constants so the logical
//   box comes out exactly DesktopDesignWidth wide, and force uscale to 1.0 by nulling the MENU'S OWN
//   ui_scaling handle rather than touching the cvar - `ui_scaling` is CVAR_USERINFO (d_main.cpp:1757)
//   and writing it would push a DEM_UINFCHANGED into the demo/net stream. A baseline cannot absorb 1.2
//   on its own: it would need CLAMP(1080/baseline) = 0.538, below calcScale's own 0.599 floor.
//
//   THE SCREEN SIZE NOW DEPENDS ON WHERE IN THE FRAME YOU ASK, and this one is unique to mode 4 -
//   mode 3 never calls the game's own drawer or ticker, so it cannot happen there. screenSizeChanged()
//   is `(Screen.GetWidth(), Screen.GetHeight()) != lastScreenSize`, and it CACHES what it saw
//   (drawer.zs:155-163). UIMenu.ticker() runs outside the redirect and sees 1920x1080; UIMenu.drawer()
//   runs inside it and sees 1240x1080. So it would report "changed" on every single call and both
//   menu.zs:208 and :261 would relayout every frame, alternating between the two widths - the layout
//   would never settle and our scale would be overwritten before it was ever drawn.
//
// The fix for that is the game's own: BOTH of those call sites are already gated on `!drawCanvas`
// (menu.zs:208, :261), a UIMenu field that exists for exactly this case. Setting it is honest rather
// than a trick - this menu genuinely is being drawn into a canvas. Note it is a DIFFERENT field from
// UIView.drawCanvas (view.zs:146): the menu's is read at those two sites and nowhere else in all of
// Selaco's scripts, and is never propagated to the view tree, so setting it does NOT make the widgets
// draw through Canvas.* natives. They keep using Screen.*, which is the whole point - the twod swap is
// what retargets them.
//
// ignoreUIScaling is the third write and closes the last relayout trigger. ticker()'s condition has a
// second clause, `!ignoreUIScaling && !(uiScale ~== lastUIScale)` (menu.zs:206-208), which is NOT
// covered by drawCanvas. With ui_scaling nulled uiScale reads 1.0 while calcScale has left lastUIScale
// at the final 0.6458, so without this the menu would relayout every tick and undo the scale.
//
// ONCE PER INSTANCE PER ZOOM, not per frame, and that is now safe precisely because all three relayout
// triggers above are closed: nothing else recomputes mainView.frame or mainView.scale. Per-frame would
// also be actively harmful here - mainView.layout() reaches PDAAppWindow.layout -> savePos ->
// SendNetworkEvent("pdaAppPos:...") (app_window.zs:170), and unlike mode 3 mode 4 does not
// suppress that, so relaying out every frame would put a network event into the stream every frame.
// Once per open is exactly what the game itself does.
//
// THE ZOOM IS THEREFORE AN EDGE AND NOT AN ARGUMENT READ FRESH EACH FRAME. It divides the baseline
// height exactly as mode 3's does, which is the whole of the feature; what mode 3 can afford and this
// cannot is calling the relayout speculatively. See the caller in FAuxMenuRedirect for the compare.
//
// `retune` says this instance is already tuned and only the zoom moved, which changes ONE thing: an abort
// must not latch LiveBroken. See the catch.
static bool LiveTune(DObject *menu, double zoom, bool retune)
{
	// Read the three handles before writing them so the whole tune can be undone if something aborts
	// part way through. All three, not just ui_scaling: restoring a GUESSED default would be a
	// different write rather than an undo. Leaving a half-tuned menu behind is the one failure here
	// that would follow the player onto the MAIN screen, because the writes that stop the game
	// relaying it out are the same writes that would stop it repairing itself.
	void *savedUIScaling = *(void **)((uint8_t *)menu + FldLiveUIScaling->Offset);
	DObject *savedDrawCanvas = *(DObject **)((uint8_t *)menu + FldLiveDrawCanvas->Offset);
	const bool savedIgnoreUIScaling = *(bool *)((uint8_t *)menu + FldLiveIgnoreUIScaling->Offset);

	try
	{
		DObject *mainView = ReadObjectField(menu, FldLiveMainView);
		if (mainView == nullptr || mainView->GetClass() != LiveCodexClass)
		{
			// Exactly UIView, not merely a subclass: layout() was resolved against UIView's vtable and a
			// subclass could override it with something this code has not read.
			Printf(TEXTCOLOR_YELLOW "AuxLiveCodex: %s.mainView is not a plain %s, second-screen PDA disabled\n",
				LivePdaClassName, ViewClassName);
			LiveBroken = true;
			return false;
		}

		// Stop the game relaying the menu out from under us, THEN relayout it ourselves. In this order,
		// so that nothing between the two can act on a half-updated state.
		*(DObject **)((uint8_t *)menu + FldLiveDrawCanvas->Offset) = LiveCanvas;
		GC::WriteBarrier(menu, LiveCanvas);
		*(bool *)((uint8_t *)menu + FldLiveIgnoreUIScaling->Offset) = true;

		// NARROW THE TAB STRIP BEFORE THE RELAYOUT BELOW, AND NOWHERE ELSE. It only writes fields and
		// pins, so it needs a layout pass to take effect - and the pass it needs is the one already three
		// lines further down. Doing it after, or in its own pass, would mean a SECOND mainView.layout(),
		// which in this mode reaches PDAAppWindow.layout -> savePos -> SendNetworkEvent("pdaAppPos:...")
		// with no FProjectionScope to stop it, i.e. an extra event in the player's savegame per open.
		//
		// AND THE FLAG IT SETS IS CONSUMED BY THAT SAME PASS, which is what stops it costing a layout of
		// its own later: setTextPadding sets requiresLayout on the tab and its label (button.zs:269-270),
		// UIView.tick would act on that on the next tick (view.zs:497-505), but UIView.layout clears it
		// unconditionally on the way out (view.zs:772) and mainView.layout() below reaches every tab -
		// UIHorizontalLayout lays each managed view out by hand in Content_SizeParent mode
		// (horizontal_layout.zs:97). So the flag is already false by the first tick.
		//
		// Even if it were not, it could not reach savePos: tick routes through parent.layoutChildChanged
		// only while the parent has layoutWithChildren, which only UIViewManager sets (view_manager.zs:19),
		// and the strip's parent innerView is a plain UIView (pda_menu.zs:99). The walk up stops at the
		// strip and relays out the strip, never mainView.
		//
		// It fails soft and latches nothing, so there is no return value worth testing, and it swallows
		// its own aborts - which matters here specifically, because the catch below this would otherwise
		// read one as a failed tune and latch mode 4 off over a cosmetic nicety.
		TightenTabStrip(menu, LivePdaClass, LiveCodexClass);

		// calcScale's read is `ui_scaling ? ui_scaling.getFloat() : 1.0` (pda_menu.zs:787), so null IS
		// the 1.0 path. Safe without a write barrier: a native struct pointer is not an object pointer
		// (zcc_compile.cpp:2208), so the GC does not trace this field and nulling it orphans nothing.
		*(void **)((uint8_t *)menu + FldLiveUIScaling->Offset) = nullptr;

		// calcScale(int screenWidth, int screenHeight, Vector2 baselineResolution), then the rest of
		// layoutChange's body in its own order (pda_menu.zs:777-781). calcScale is CALLED rather than
		// reimplemented because it also sets UIDrawer's screenSize and virtualScreenSize (:806-807) and
		// mainView.scale, all of which have to move together.
		//
		// Dividing the baseline height is the whole of the zoom, the same single division mode 3 does:
		// calcScale's newScale is uscale * CLAMP(canvasHeight / baseline.y, ...), so a smaller baseline
		// is a bigger scale and a smaller logical box, which is bigger content and a cropped right and
		// bottom edge. At zoom 1.0 the division is exact in IEEE-754, so calcScale is handed the same
		// double this line passed before the zoom existed and the layout is bit-identical.
		//
		// hasLayedOutOnce is deliberately not set: its only reader is menu.zs:261, which drawCanvas has
		// already short-circuited.
		{
			VMValue params[] = { menu, (int)AuxCanvasWidth, (int)AuxCanvasHeight,
				DesktopDesignWidth, DesktopBaselineHeight / zoom };
			VMCall(FuncLiveCalcScale, params, LiveCalcScaleRegs, nullptr, 0);
		}

		{
			VMValue selfOnly[] = { menu };
			VMCall(FuncLiveLayout, selfOnly, 1, nullptr, 0);
		}

		// mainView.layout() with the DEFAULTS layoutChange passes: parentScale (0,0) is the sentinel
		// that makes UIView.layout derive cScale from the view's own scale chain (view.zs:763) instead
		// of taking ours, and parentAlpha -1 does the same for alpha (:764). Passing (1,1)/1.0 instead
		// would overwrite the scale calcScale just installed.
		{
			VMValue viewParams[] = { mainView, 0.0, 0.0, -1.0, (int)0 };
			VMCall(FuncLiveCodexLayout, viewParams, LiveCodexLayoutRegs, nullptr, 0);
		}
	}
	catch (const std::exception &e)
	{
		// Put the three writes back, each to the value it actually had. Without this the menu keeps
		// drawCanvas and ignoreUIScaling set, which is precisely what stops UIMenu.ticker and
		// UIMenu.drawer from relaying it out - so a half-tuned menu would stay mis-laid-out for the
		// rest of its life, now on the main screen, where the player would see the damage rather than
		// just lose the feature.
		//
		// On a RETUNE these three reads happened while the menu was already tuned, so all three writes
		// put back the tuned values and the block is a no-op. That is correct - undoing a second tune
		// leaves the first one standing - but it is worth knowing it is not restoring anything pristine.
		*(DObject **)((uint8_t *)menu + FldLiveDrawCanvas->Offset) = savedDrawCanvas;
		if (savedDrawCanvas != nullptr)
			GC::WriteBarrier(menu, savedDrawCanvas);
		*(bool *)((uint8_t *)menu + FldLiveIgnoreUIScaling->Offset) = savedIgnoreUIScaling;
		*(void **)((uint8_t *)menu + FldLiveUIScaling->Offset) = savedUIScaling;

		if (retune)
		{
			// NOTHING IS LATCHED, because latching here would be the worse failure by some distance: the
			// menu is still drawCanvas'd and still drawable on the panel, so refusing to redirect it would
			// hand the player a PDA laid out for 1240x1080 drawn over the game on the MAIN screen, with no
			// way to undo it. Reported and left where it is instead, laid out for whatever the abort got to.
			// The caller still advances the tuned zoom, so this is said once per value rather than per frame.
			Printf(TEXTCOLOR_YELLOW "AuxLiveCodex: relaying out %s at zoom %g aborted (%s), the panel keeps "
				"the layout it had\n", LivePdaClassName, zoom, e.what());
			return true;
		}

		Printf(TEXTCOLOR_YELLOW "AuxLiveCodex: relaying out %s aborted (%s), second-screen PDA disabled\n",
			LivePdaClassName, e.what());
		LiveBroken = true;
		return false;
	}

	// THE FIGURES ARE THE ONES ASKED FOR, and the note is what corrects them. Mode 3 reads the installed
	// scale back out of lastUIScale; mode 4 does not resolve that field, so rather than print a number that
	// is silently a guess this prints the request and lets CodexZoomLimitNote name whichever of
	// calcScale's dead bands - its 0.599 floor, its 1.0 snap window, its 2.0 ceiling - swallowed it.
	//
	// The parenthesised token is a REVISION SENTINEL, deliberately unique to this change so it can be
	// grepped out of the built .so to prove which revision is actually packaged - a size match has twice
	// passed against a stale APK on this project.
	Printf("AuxLiveCodex: %s relaid out for %s at zoom %g, logical box %gx%g at scale %g%s "
		"(aux-menu-zoom r1)\n",
		LivePdaClassName, AuxCanvasName, zoom, DesktopDesignWidth / zoom, DesktopBaselineHeight / zoom,
		zoom * AuxCanvasWidth / DesktopDesignWidth, CodexZoomLimitNote(zoom));
	return true;
}

// Called once per frame from I_AuxPanelFrame while aux_codex_mode is 4, BEFORE screen->BeginFrame.
//
// Mode 4 has no phase 1: its draw is issued from DrawOverlays later in the same frame, not from here.
// All this does is arm the guard, decide whether that draw publishes, and blank the panel when the PDA
// closes. Everything expensive is behind LiveIsPdaOpen, so a frame with no PDA open costs a latched
// bool, a null test, a menuactive compare and one IsKindOf.
void I_AuxLiveCodexFrame()
{
	const bool open = LiveIsPdaOpen();
	LiveArmed = open;

	if (open != LiveWasOpen)
	{
		LiveWasOpen = open;
		LiveFramesSincePublish = 0;

		if (!open)
		{
			// The PDA closed. Blank the panel rather than leave the last codex page on it: the canvas
			// texture is never cleared for us (RenderTextureView does not clear the attachment, and the
			// AllCanvases loop's Drawer.Clear() clears the COMMAND LIST, not the image), so without this
			// a stale page would sit on the lower screen for the rest of the session. One black quad,
			// one readback, then mode 4 is completely inert until the PDA opens again.
			extern void I_AuxCanvasClear();
			I_AuxCanvasClear();

			extern void I_AuxPanelRequestReadback();
			I_AuxPanelRequestReadback();

			// A new menu landing on the recycled address of this one must still be tuned.
			LiveTuned = nullptr;
			LiveWantPublish = false;
			return;
		}
	}

	// Pace the publishing, and never stack two: while a published frame is still working its way to the
	// panel, redirect and discard rather than mark the texture again. Marking it again would keep
	// CheckNeedsUpdate true at the top of every frame and permanently starve the phase-2 gate in
	// i_auxpanel.cpp, which is the readback's only proof that the render actually happened.
	//
	// The counter is advanced UNCONDITIONALLY, outside the pending test, and that is not cosmetic. With
	// `++counter` inside the && it is short-circuited away on every pending frame, so the interval starts
	// counting again from zero after each readback instead of overlapping with it - which turned an
	// intended 2-frame period into 4. Written this way the pending test and the interval are independent
	// limits and the slower of the two wins, which is what "interval" is supposed to mean.
	extern bool I_AuxPanelReadbackPending();
	++LiveFramesSincePublish;
	LiveWantPublish = open && !I_AuxPanelReadbackPending()
		&& LiveFramesSincePublish >= AuxLivePublishInterval;

	if (LiveWantPublish)
		LiveFramesSincePublish = 0;
}

FAuxMenuRedirect::FAuxMenuRedirect()
{
	const bool armed = LiveArmed;
	LiveArmed = false;

	// Re-test rather than trusting the arm. The arm only says the panel is live and mode 4 is selected;
	// whether the PDA is the current menu can have changed since the top of the frame, and getting that
	// wrong in the false direction sends the game's real menu to a screen nobody reads it back from.
	if (!armed || !LiveIsPdaOpen() || LiveCanvas == nullptr)
		return;

	// Before the swap and before M_Drawer, because M_Drawer is what reads it (menu.cpp:903). Written
	// every frame rather than once because PDAMenu3.init sets DontBlur = false explicitly
	// (pda_menu.zs:80) and any future re-init would put it back.
	Blurred = CurrentMenu;
	SavedDontBlur = Blurred->DontBlur;
	Blurred->DontBlur = true;

	// PAUSE THE WORLD. PDAMenu3 asks for MENU_OnNoPause (pda_menu.zs:82), which is right on the main
	// screen - the PDA is an overlay there and the player can still see the game. On the second screen
	// the player is looking AWAY from the game, so leaving it running means being shot at while reading.
	// P_CheckTickerPaused pauses on anything that is neither MENU_Off nor MENU_OnNoPause
	// (p_tick.cpp:57-62), so MENU_On is the whole change.
	//
	// Written every frame, and deliberately NEVER restored. M_ClearMenus already sets
	// menuactive = MENU_Off when the menu closes (menu.cpp:946), so putting a saved MENU_OnNoPause back
	// afterwards would leave the engine believing a menu is open for the rest of the session. The
	// engine owns this global; mode 4 only overrides its value while the PDA is up.
	//
	// One tic of latency each way, because D_Display runs after TryRunTics - so the world runs for one
	// tic after the PDA opens and one after it closes. Against MENU_OnNoPause, which runs the world for
	// the entire time the PDA is open, that is not worth a hook earlier in the loop.
	menuactive = MENU_On;

	Saved = twod;
	Canvas = LiveCanvas;
	twod = &Canvas->Drawer;

	// The two things Begin() does are both required, and neither is obvious. isIn2D is per drawer and
	// the canvas drawer never gets Begin() from anywhere else, so without this the first Screen draw
	// native throws (v_draw.cpp:260). Width/Height are what Screen.GetWidth()/GetHeight() return
	// (v_framebuffer.cpp:342-356), which is what makes the whole menu system lay itself out for the
	// panel. It does NOT clear the command list, so the previous frame's commands - which RenderView
	// has not consumed yet - are safe.
	twod->Begin((int)AuxCanvasWidth, (int)AuxCanvasHeight);
	Redirected = true;

	// STRICTLY EDGE-TRIGGERED ON THE ZOOM, and this compare is the single most load-bearing line in the
	// mode. Mode 4 deliberately has NO FProjectionScope, so LiveTune's mainView.layout() reaches
	// PDAAppWindow.layout -> savePos -> SendNetworkEvent("pdaAppPos:...") for real - into the player's
	// savegame and the demo stream, which is exactly what it should do for the player's own actions and
	// exactly what must not happen because we felt like re-reading a cvar. So the zoom the live layout was
	// built with is remembered and only a CHANGE to it re-tunes; an unchanged zoom costs one cvar read, one
	// clamp and two compares, calls no script and emits nothing.
	//
	// CodexZoom never returns NaN (i_auxvmreflect.h), which this relies on: NaN != NaN is true, so a
	// NaN would be a permanent edge and therefore a relayout and a network event on every single frame.
	const double zoom = CodexZoom();
	if (LiveTuned != CurrentMenu || LiveTunedZoom != zoom)
	{
		// A zoom change on the instance we already tuned, as opposed to a first tune, which is the only
		// thing LiveTune treats differently - it must not latch the mode off on an abort.
		const bool retune = LiveTuned == CurrentMenu;

		// Inside the redirect on purpose: anything the relayout reads or draws through Screen.* then
		// sees the panel rather than the main screen, and is legal because Begin() has run.
		if (LiveTune(CurrentMenu, zoom, retune))
		{
			LiveTuned = CurrentMenu;

			// Advanced whether the relayout succeeded or aborted, so a zoom that aborts is tried once
			// rather than on every frame the player leaves it set.
			LiveTunedZoom = zoom;
		}
		else
		{
			LiveWantPublish = false;   // LiveBroken is latched; discard this frame rather than show it
		}
	}
}

FAuxMenuRedirect::~FAuxMenuRedirect()
{
	if (Redirected)
	{
		// The view tree sets a clip rect on the drawer per subview and does not always clear it
		// (view.zs:1175-1177). The canvas drawer outlives this frame, so a leaked clip would clip
		// whatever draws into the canvas next. DMenu::CallDrawer resets the main drawer for the same
		// reason (menu.cpp:426).
		Canvas->Drawer.ClearClipRect();
		twod->End();
		twod = Saved;

		if (LiveWantPublish)
		{
			// The _Screen natives do not mark the texture, so this is the only thing that gets the canvas
			// into the AllCanvases loop at all.
			Canvas->Tex->NeedUpdate();

			// Arm the readback. It cannot happen this frame - the AllCanvases flush is in RenderView at
			// d_main.cpp:1115, EARLIER than this destructor at :907 - so these commands are rendered by
			// the next frame's RenderView and I_AuxCanvasRenderPending holds the readback until then.
			extern void I_AuxPanelRequestReadback();
			I_AuxPanelRequestReadback();
		}
		else
		{
			// Not a publish frame: throw the commands away. Redirecting on EVERY frame is what keeps the
			// PDA off the main screen; only publishing is paced. Leaving the commands in the drawer
			// instead would do two bad things - the loop only Clear()s a drawer it actually renders
			// (hw_entrypoint.cpp:371), so a whole PDA's worth of geometry would accumulate per frame, and
			// the texture would stay dirty and starve the readback gate.
			Canvas->Drawer.Clear();
		}

		LiveWantPublish = false;
	}

	// Only if it is STILL the current menu. A menu drawer can close its own menu, and M_ClearMenus
	// Destroy()s it (menu.cpp:946) - after which any allocation inside the rest of M_Drawer can run a
	// GC step that frees it. Writing through the held pointer then is a use-after-free. The compare
	// itself never dereferences, so it is safe on a dead pointer, and a menu that is gone has nothing
	// left to restore.
	if (Blurred != nullptr && Blurred == CurrentMenu)
		Blurred->DontBlur = SavedDontBlur;
}

// Forget every resolve this file latched, for the restart teardown. See I_AuxForgetScriptState in
// i_auxvmreflect.cpp for why it exists and when it runs.
//
// LiveResolved is the one that matters most here: LiveIsPdaOpen short-circuits LiveResolve() on it, so
// left set it keeps all eleven pointers below in use against a script that has been recompiled - and the
// four PFields are used as raw byte offsets to WRITE through, which is the widest corruption primitive
// this file has. Nothing here does VM work; every line is a store.
void I_AuxLiveCodexForgetScriptState()
{
	// Cleared, unlike the in-session behaviour where a latched verdict stays latched, because a restart can
	// bring up a different wad set and the old verdict is about a game that is no longer running.
	LiveAbsent = false;
	LiveBroken = false;
	LiveResolved = false;

	LivePdaClass = nullptr;
	LiveCodexClass = nullptr;
	FuncLiveCalcScale = nullptr;
	FuncLiveLayout = nullptr;
	FuncLiveCodexLayout = nullptr;
	LiveCalcScaleRegs = 0;
	LiveCodexLayoutRegs = 0;
	FldLiveMainView = nullptr;
	FldLiveUIScaling = nullptr;
	FldLiveIgnoreUIScaling = nullptr;
	FldLiveDrawCanvas = nullptr;
	LiveAppWindowClass = nullptr;
	FldLiveCurrentAppWindow = nullptr;

	// The canvas is a DObject owned by the AUXCANVAS FCanvasTexture, which D_Cleanup's TexMan.DeleteAll()
	// destroys. Left set, FAuxMenuRedirect would point the global twod at a freed F2DDrawer for the whole
	// of M_Drawer, so every 2D command the engine's menu system issues would land in freed memory.
	LiveCanvas = nullptr;

	LiveTuned = nullptr;
	LiveTunedZoom = 1.0;
	LiveArmed = false;
	LiveWantPublish = false;

	// Cleared so the first post-restart frame does not see a close edge for a PDA that belonged to the
	// previous session and issue a canvas clear and a readback against the stale canvas.
	LiveWasOpen = false;
	LiveFramesSincePublish = 0;
}
