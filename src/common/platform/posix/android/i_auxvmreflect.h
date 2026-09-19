#pragma once

/*
** i_auxvmreflect.h
** ZScript symbol resolution for the AYN Thor's second screen, shared by the mode-3 dashboard
** (i_auxcodexview.cpp) and the mode-4 PDA redirect (i_auxmenuview.cpp).
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

// GetTextureCanvas has no declaration in any engine header - vmnatives.cpp:622, its only other
// caller, declares it locally too. Declared here rather than in an upstream header so this fork's
// footprint in upstream code stays zero, and once rather than per file so the three aux translation
// units that call it cannot drift apart.
FCanvas *GetTextureCanvas(const FString &texturename);

// ---------------------------------------------------------------------------------------------
// THE SECOND-SCREEN ZOOM, which BOTH modes lay their menu out against: one cvar, one clamp, two modes.
//
// aux_dashboard_zoom and the arithmetic table saying where its useful travel actually ends live in
// i_auxcodexview.cpp, next to the mode-3 relayout they were written for. Mode 4 divides the same
// baseline height by the same clamped value, so these two are the whole of what it needs from there.
//
// NOT a second cross-mode channel like MenuLastAppClass below: there is no state and no direction here,
// only a read of one cvar through one clamp. Shared rather than re-derived because the clamp is
// load-bearing for mode 4 in a way it is not for mode 3 - the value comes back inside 0.5 - 2.0 and
// NEVER NaN, and a NaN would compare unequal to itself, which in mode 4 is a relayout every frame and a
// network event with it.
//
// At global scope rather than inside AuxView for the same reason GetTextureCanvas is: they are defined
// in a translation unit that does not open the namespace. Hence the Dashboard prefix on both names,
// which is the cvar's own name rather than a claim that mode 3 owns them.
double DashboardZoom();

// Which of calcScale's own limits swallowed a zoom, as a phrase to append to a log line, or "" when the
// zoom asked for is the zoom that comes back. calcScale is the same PDAMenu3 method in both modes, so
// its floor and its snap band are dead zones in both.
const char *DashboardZoomLimitNote(double zoom);

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
// the concrete app classes against it (see SelectDashboardApp) and mode 4 checks it before reading
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
VMFunction *ResolveMethod(PClass *cls, const char *funcname,
	const EArgKind *argkinds, unsigned nargs, int *outRegs, PClass *argClass = nullptr);

// A field we are about to write, proved to be the field we mean. A wrong offset here is memory
// corruption rather than a misdraw, so the type is checked structurally every time and nothing is
// inferred from the name.
enum EFieldKind
{
	Field_Bool,
	Field_Float,
	Field_String,
	Field_ViewPtr,
	Field_CVarPtr,
	Field_CanvasPtr,
	Field_ObjArray,
	Field_MenuPtr,
};

// Returns nullptr on any mismatch, having already printed which check failed. Latches nothing, for
// the same reason ResolveMethod does not.
PField *ResolveField(PClass *cls, const char *fieldname, EFieldKind kind, PClass *viewCls);

DObject *ReadObjectField(DObject *obj, const PField *field);

// A ZScript String field, addressed for reading or writing. DObject::StringVar does the same thing in
// one call but I_Errors when the field is missing or the wrong type (dobject.cpp:624), which is exactly
// the startup this fork's fail-soft rule refuses to cost the player - so the field is proved by
// ResolveField first and only the arithmetic happens here.
FString *StringFieldAddr(DObject *obj, const PField *field);

// ---------------------------------------------------------------------------------------------
// THE ONE DELIBERATE CHANNEL BETWEEN MODE 4 AND MODE 3, and the whole point of the feature: the class
// of the app the player last switched to in their OWN PDA. Written by MenuSampleCurrentApp (mode 4,
// i_auxmenuview.cpp) on every frame the real PDA is open, read by DashboardWantedIndex (mode 3,
// i_auxcodexview.cpp).
//
// Every other piece of state in the two mode files is deliberately private to one mode so that a
// failure in one cannot disturb another. This is the exception, so it is built to be harmless in both
// directions: mode 4 only ever stores a PClass* it has already proved is a PDAAppWindow, PClass objects
// are never freed, and a mode-4 failure simply leaves it null - which reads as "the player has not
// opened their PDA yet" and falls through to the cvar. Never cleared, because "the last app this
// session" is exactly what it means.
//
// An accessor pair rather than an extern so that the storage stays in one place and neither mode owns
// the other's state: this is the only thing that crosses the file boundary between them.
// ---------------------------------------------------------------------------------------------
PClass *MenuLastAppClass();
void SetMenuLastAppClass(PClass *cls);

}   // namespace AuxView
