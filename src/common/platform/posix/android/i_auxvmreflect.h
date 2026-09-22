#pragma once

/*
** i_auxvmreflect.h
** ZScript symbol resolution for the AYN Thor's second screen, shared by the mode-3 standby codex
** (i_auxstandbycodex.cpp) and the mode-4 live codex, Selaco's real PDA redirected (i_auxlivecodex.cpp).
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
** THE VALIDATION IS NOT DEFENSIVE PADDING - IT IS THE SAFETY ARGUMENT.
**
** VMCall is unchecked in a release build. VMFillParams (vmexec.cpp:204-234) walks the CALLEE's own
** NumArgs and RegTypes, not the length of the array we hand it, so a function whose signature is
** one register wider than we assume reads past the end of our params array and copies whatever was
** on the stack into the callee's registers - a pointer register filled with garbage is an immediate
** crash inside script code with no hint that C++ caused it. So every symbol is proved before it is
** called: the class chain, the self type, each declared argument type, no out parameters, script
** (not native, not abstract), and finally NumArgs against the register count we actually pass.
** Same discipline, and for the same reason, as the Map<Name,Int> type checks in i_auxcodex.cpp.
**
** THE RESOLVERS LATCH NOTHING, AND THAT IS THE WHOLE POINT OF THEM LIVING HERE. They print which
** check failed and return nullptr; what a failure MEANS belongs to the caller. The same two
** functions serve mode 3's required symbols, mode 4's required symbols, and two groups that are
** explicitly optional (mode 3's wide-box relayout and mode 4's currentAppWindow), so a latch written
** from inside them could only ever be right for one caller and wrong for the rest. Each call site
** records the failure against its own latch.
*/

class DObject;
class FCanvas;
class FString;
class PClass;
class PField;
class VMFunction;

// The second screen's canvas owner, named where every aux file that compares against it can see the
// name rather than the bare integer. THE INTEGERS ARE A USER-FACING CONTRACT and must never be
// renumbered: aux_codex_mode is a console cvar the player types a number into
// (`aux_codex_mode 5`), and both TECHNICAL.md and the commit history cite these values.
//
// 1 and 2 are RETIRED and deliberately not reused, so a config or a habit that still names one lands
// on the no-content branch (i_auxpanel.cpp) rather than silently on whatever took the number next.
enum EAuxCanvasMode
{
	AuxMode_TestPattern = 0,
	// 1 and 2 are retired - see the comment above.
	AuxMode_Standby     = 3,   // the standby codex: a read-only summary drawn by this fork's own C++ view
	AuxMode_Live        = 4,   // the live codex: Selaco's real PDA, redirected onto the panel
	AuxMode_Auto        = 5,   // resolves per frame to Standby or Live, whichever should hold the canvas
};

// GetTextureCanvas has no declaration in any engine header - vmnatives.cpp:622, its only other
// caller, declares it locally too. Declared here rather than in an upstream header so this fork's
// footprint in upstream code stays zero, and once rather than per file so the three aux translation
// units that call it cannot drift apart.
FCanvas *GetTextureCanvas(const FString &texturename);

// ---------------------------------------------------------------------------------------------
// THE RESTART TEARDOWN. One entry point, called from D_Cleanup; one per-file reset behind it.
//
// A `restart` deletes every PClass and VMFunction and releases every symbol table, then reinitialises the
// engine in process against freshly parsed script - so every resolve this port latches is dangling from
// that moment, and the two GC marker functions keep walking the DObjects they mark until something nulls
// them. Each file resets its own statics because they are file-local by design; the aggregator in
// i_auxvmreflect.cpp is what D_Cleanup calls, and it carries the full reasoning.
//
// Declared here, rather than as an ad-hoc local `extern` at each call site, so the nine definitions and
// the one caller cannot drift apart - they are only ever called from one place and a silent signature
// mismatch would be a linker error at best and a wrong call at worst.
//
// EVERY ONE OF THESE MUST REMAIN FREE OF VM WORK. They run after the sound system is down and before the
// classes are deleted, so a VMCall, a Destroy(), a sound or a texture lookup from any of them would be
// unsafe. Storing to file statics is the whole contract.
// ---------------------------------------------------------------------------------------------
void I_AuxForgetScriptState();

void I_AuxCanvasForgetScriptState();
void I_AuxCodexForgetScriptState();
void I_AuxCodexViewStateForgetScriptState();
void I_AuxStandbyCodexForgetScriptState();
void I_AuxLiveCodexForgetScriptState();
void I_AuxPanelForgetScriptState();
void I_AuxDevicePickerForgetScriptState();
void I_AuxDeviceResetForgetScriptState();

// ---------------------------------------------------------------------------------------------
// THE SECOND-SCREEN ZOOM, which BOTH modes lay their menu out against: one cvar, one clamp, two modes.
//
// aux_codex_size and the arithmetic table saying where its useful travel actually ends live in
// i_auxstandbycodex.cpp, next to the mode-3 relayout they were written for. Mode 4 divides the same
// baseline height by the same clamped value, so these two are the whole of what it needs from there.
//
// NOT a cross-mode channel like the view-state manager (i_auxcodexviewstate.h): there is no state and no
// direction here, only a read of one cvar through one clamp. Shared rather than re-derived because the clamp
// is load-bearing for mode 4 in a way it is not for mode 3 - the value comes back inside 0.5 - 2.0 and
// NEVER NaN, and a NaN would compare unequal to itself, which in mode 4 is a relayout every frame and a
// network event with it.
//
// At global scope rather than inside AuxView for the same reason GetTextureCanvas is: they are defined
// in a translation unit that does not open the namespace. Hence the Codex prefix on both names, which
// is the cvar's own name rather than a claim that either mode owns them.
double CodexZoom();

// Which of calcScale's own limits swallowed a zoom, as a phrase to append to a log line, or "" when the
// zoom asked for is the zoom that comes back. calcScale is the same PDAMenu3 method in both modes, so
// its floor and its snap band are dead zones in both.
const char *CodexZoomLimitNote(double zoom);

// Everything below is in a namespace for one concrete reason: i_auxcanvas.cpp includes this header
// for GetTextureCanvas and already has its own AuxCanvasName, AuxCanvasWidth and AuxCanvasHeight
// with different types and meanings, which at file scope would be a redefinition. Both mode files
// say `using namespace AuxView;`, so nothing that moved in here had to be renamed.
namespace AuxView
{

// ---------------------------------------------------------------------------------------------
// The panel's geometry, which both modes lay their menu out against.
// ---------------------------------------------------------------------------------------------

// Must match the canvastexture line in wadsrc/static/animdefs.txt, which is what actually creates the
// texture and is therefore authoritative. These cannot be derived from it - the lump is parsed at
// runtime and the layout math below needs the numbers at compile time - so AuxCanvasResolve in
// i_auxcanvas.cpp compares the two on startup and prints a named warning if they have drifted apart.
static const char *const AuxCanvasName = "AUXCANVAS";
static const double AuxCanvasWidth = 1240.0;
static const double AuxCanvasHeight = 1080.0;

// The width the PDA desktop is DESIGNED against. innerView caps itself at 1920 (pda_menu.zs:103) and
// every offset in PDAMenu3.init is a pixel figure measured against that width, so a logical box
// narrower than this clips the tab bar - which is exactly what the panel showed before, "ALOGS" at
// the left edge and "MAI" at the right.
static const double DesktopDesignWidth = 1920.0;

// The baseline resolution to hand PDAMenu3.calcScale (pda_menu.zs:785) so the design width lands
// exactly on the canvas width. calcScale is newScale = uscale * CLAMP(height / baseline.y, .599, 2)
// and then mainView.frame.size = screen / newScale, so:
//
//   want mainView.frame.size.x == DesktopDesignWidth
//     => newScale            == AuxCanvasWidth / DesktopDesignWidth   = 1240/1920 = 0.645833
//     => baseline.y          == AuxCanvasHeight / newScale
//                            == AuxCanvasHeight * DesktopDesignWidth / AuxCanvasWidth = 1672.258
//
// Derived from the canvas constants rather than written out, so it follows a canvas resize. 0.645833
// clears calcScale's 0.599 CLAMP floor and is far outside both of its snap windows
// (|s-1| < 0.08 and |s-2| < 0.08), so the value asked for is the value that comes back.
//
// baseline.x is never read by calcScale; DesktopDesignWidth is passed because that is what it means.
static const double DesktopBaselineHeight = AuxCanvasHeight * DesktopDesignWidth / AuxCanvasWidth;

// The class every view call in both modes assumes.
static const char *const ViewClassName = "UIView";

static const char *const MenuClassName = "UIMenu";

// The base of every app that lives on the desktop (app_window.zs:1). Mode 3's app selection resolves
// the concrete app classes against it (see SelectStandbyApp) and mode 4 checks it before reading
// currentAppWindow.
static const char *const AppWindowClassName = "PDAAppWindow";

// ---------------------------------------------------------------------------------------------
// Symbol resolution, with every check the VM will not do for us.
// ---------------------------------------------------------------------------------------------

// The declared argument kinds this file knows how to pass. Kinds rather than PType pointers on
// purpose: building the expected Canvas pointer type with NewPointer would ADD a type to the global
// type table (types.cpp:1662-1667) from the render path, which is exactly what i_auxcodex.cpp
// refuses to do with NewMap. Comparing structurally allocates nothing.
enum EArgKind
{
	Arg_Vector2,
	Arg_Float,
	Arg_Bool,
	Arg_Int,
	Arg_String,
	Arg_Canvas,
	Arg_Menu,
	Arg_ObjectOf,
};

// Resolve a method on cls and prove it is exactly the function we are about to call. Returns nullptr
// on any mismatch, having already printed which check failed; *outRegs receives the number of params
// to pass. Nothing is latched here - see THE RESOLVERS LATCH NOTHING above.
//
// argClass is forwarded to ArgMatches for Arg_ObjectOf and ignored otherwise. One class covers every
// call site because none of them passes more than one script object.
//
// subsystem prefixes every failure line and disabledNote ends it, so a caller that is not the standby
// codex neither announces itself as one nor tells the player the second-screen view is off.
//
// EVERY CALLER PASSES BOTH UNLESS IT REALLY IS THE STANDBY CODEX, and the standby codex is the only
// reason the default exists. Taking the default elsewhere does not fail, it MISREPORTS: the live codex,
// the tab strip, the panel and the reset button each used to print "AuxStandbyCodex: ... second-screen
// view disabled" for a symbol the standby codex never asks for and a consequence that was not the one
// they suffer - which sends anyone reading a device log to the wrong file. Match the name and the
// consequence the caller's own Printfs already use rather than inventing a third wording.
VMFunction *ResolveMethod(PClass *cls, const char *funcname, const char *subsystem,
	const EArgKind *argkinds, unsigned nargs, int *outRegs, PClass *argClass = nullptr,
	const char *disabledNote = "second-screen view disabled");

// A field we are about to write, proved to be the field we mean. A wrong offset here is memory
// corruption rather than a misdraw, so the type is checked structurally every time and nothing is
// inferred from the name.
enum EFieldKind
{
	Field_Bool,
	Field_Int,
	Field_Float,
	Field_String,
	Field_ViewPtr,
	Field_CVarPtr,
	Field_CanvasPtr,
	Field_ObjArray,
	Field_MenuPtr,
};

// A ZERO-ARGUMENT STATIC class method, proved the same way ResolveMethod proves an instance one.
//
// SEPARATE FROM ResolveMethod RATHER THAN A FLAG ON IT, deliberately. ResolveMethod hardcodes
// selfArgs = 1 and rejects the absence of a self outright (see the VARF_Method comment in its body),
// because every mode-3 and mode-4 call site passes an instance. Threading a "no self" mode through it
// would edit the one function the whole second screen resolves its symbols with, and a mistake there
// costs the panel - which is the one thing this fork's aux code is not allowed to break. A static with
// no arguments needs none of ResolveMethod's argument machinery anyway: there is no self to type-check
// and no parameter list to walk, so the honest version is shorter, not a special case of a longer one.
//
// Restricted to zero arguments because that is the only static this fork calls
// (UIHelper.SetSteamdeckPresets, helper.zs:616). A static WITH arguments would need the ArgMatches
// loop, and should extend this rather than grow a second copy of it.
//
// subsystem prefixes every failure line, so a caller that is not the standby codex does not print
// "second-screen view disabled" at the player. Returns nullptr on any mismatch, having already printed
// which check failed, and latches nothing - for the same reason ResolveMethod does not.
VMFunction *ResolveStaticMethod(PClass *cls, const char *funcname, const char *subsystem, int *outRegs);

// Returns nullptr on any mismatch, having already printed which check failed. Latches nothing, for
// the same reason ResolveMethod does not.
//
// subsystem prefixes every failure line and disabledNote ends it, for the same reason and with the same
// defaults as ResolveMethod's: a caller that is not the standby codex must neither announce itself as one nor
// tell the player the second-screen view is off when only its own feature has been lost.
PField *ResolveField(PClass *cls, const char *fieldname, EFieldKind kind, PClass *viewCls,
	const char *subsystem = "AuxStandbyCodex", const char *disabledNote = "second-screen view disabled");

DObject *ReadObjectField(DObject *obj, const PField *field);

// A ZScript String field, addressed for reading or writing. DObject::StringVar does the same thing in
// one call but I_Errors when the field is missing or the wrong type (dobject.cpp:624), which is exactly
// the startup this fork's fail-soft rule refuses to cost the player - so the field is proved by
// ResolveField first and only the arithmetic happens here.
FString *StringFieldAddr(DObject *obj, const PField *field);

// ---------------------------------------------------------------------------------------------
// THE TAB STRIP, narrowed to fit a logical box the desktop was never designed for.
//
// PDAMenu3's tab strip is a UIHorizontalLayout pinned Pin_HCenter and sized to its contents
// (pda_menu.zs:199-205), so when it is wider than the box it overflows SYMMETRICALLY: it loses the first
// tab as readily as the last, and at the wider zooms it loses the LT/RT trigger icons before either.
// Selaco gives each of the six tabs 42 pixels of padding on each side (pda_menu.zs:238, and once more per
// tab after it), which is 84 per tab and just over 500 across the strip - by far the biggest thing that
// can be handed back without touching anything the design depends on.
//
// SHARED BETWEEN BOTH MODES because both host the same PDAMenu3 against the same narrow canvas, and it
// lives here rather than in either mode file for the reason the view-state manager lives in its own file:
// neither mode owns it.
//
// EACH CALLER FOLDS IT INTO THE RELAYOUT IT ALREADY RUNS, and that placement is a requirement rather
// than a tidiness: this only writes fields and pins, so it needs a layout pass after it to take effect,
// and in mode 4 a layout pass reaches PDAAppWindow.layout -> savePos -> SendNetworkEvent (app_window.zs
// :170) with no FProjectionScope to catch it. An extra relayout there would be an extra event in the
// player's savegame, so there must not be one.
//
// FAILS SOFT, AND LATCHES NOTHING OF THE CALLER'S. One yellow line and a return leaves every tab at
// Selaco's own 42 and the strip exactly as wide as it shipped. The try/catch inside is therefore not
// optional: both callers sit in a try whose catch DOES latch their mode off, so an abort from here has
// to be swallowed before it can reach one. Tighter tabs are a nicety; a drawing panel is not.
//
// menuCls is what the tab fields are resolved against and viewCls the UIView they are proved to point
// at; both callers have already proved menu is an instance of menuCls.
// ---------------------------------------------------------------------------------------------
void TightenTabStrip(DObject *menu, PClass *menuCls, PClass *viewCls);

}   // namespace AuxView
