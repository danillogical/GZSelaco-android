/*
** i_auxdevicereset.cpp
** The Handhelds page's "Reset Device Choice" button: one menu item, one confirmation prompt borrowed
** from Selaco's own, and one irreversible action - forget the device, write the ini, quit.
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
** WHY THIS REPLACES SELACO'S "CHOOSE OPTIMAL SETTINGS" RATHER THAN SITTING NEXT TO IT.
**
** Their item runs UIHelper.SetSteamdeckPresets() and nothing else (som_option_view.zs:558-562): the UI
** scale, subtitle size, HUD scale and aim assist, and no graphics preset at all. That is most of what a
** device profile writes and none of the rest of it, so once a profile HAS been applied their button is a
** way to half-override your own choice - it moves nine cvars of the profile's set and leaves the other
** several dozen where the profile put them, with nothing on screen saying so. The profile applier already
** calls their SetSteamdeckPresets for every profile that asks for it (i_auxprofile.cpp), so nothing is
** lost by taking the button away, and what replaces it is the honest version of the same intent: go back
** and pick again.
**
** THE ASYMMETRY THAT GOVERNS EVERY FAILURE PATH HERE. This action quits the game and the player cannot
** undo it, so the two directions of failure are not equally bad:
**
**   - Cannot install the button -> Selaco's item is left exactly as it shipped, untouched. The player
**     loses a feature they did not have yesterday.
**   - Cannot build the confirmation -> NOTHING HAPPENS AT ALL. There is deliberately no fallback that
**     resets without asking, and the button is not even installed unless the prompt has already been
**     proved to be buildable, so a player can never reach a press that resets unconfirmed.
**
** HOW A CONFIRMATION IS ASKED FOR WITHOUT BEING ABLE TO NAME A SELACO CLASS.
**
** Selaco's PromptMenu is the styled prompt every destructive action in their game uses, and it reports
** the button that was pressed through two fields of its own (prompt.zs:102-103):
**
**     Function<ui void(Object, int)> onClosed;
**     Object receiver;
**
** called as onClosed.call(receiver, ctrl.controlID) (prompt.zs:300-301). A Function<> field holds a
** PFunction the compiler had to have built from a real declaration, so the callback cannot be synthesised
** from C++ - and this fork's own ZScript compiles BEFORE the player's ipk3, so it cannot name PromptMenu
** either. What makes the bridge possible is that `Object` and `int` are CORE types: our
** AuxDeviceResetPrompt.Answered (wadsrc/static/zscript/engine/aux_devicereset.zs) has a signature that
** matches their field while naming nothing of theirs, and this file writes it into the field by
** reflection. Their side and ours therefore agree on a type neither had to know the other was using.
**
** The one thing the callback cannot do is run a CCMD - that is menu-item-only by construction
** (optionmenuitems.zs:187) - so it writes aux_devicereset and the work happens here, in AuxDeviceResetFrame.
**
** WHY THE PROMPT IS OPENED A FRAME LATER AND NOT FROM THE CCMD, which is the one thing here that looks
** like indirection and is not. Selaco run a command item through SelacoOptionActivator.RunCCMDI
** (som_option_view.zs:973-990), which stands up a THROWAWAY menu, activates it, dispatches the item, and
** then closes it - and DMenu::Close sets CurrentMenu = this->mParentMenu unconditionally, with its
** `assert(CurrentMenu == this)` commented out (menu.cpp:364-368). So a menu activated from inside the CCMD
** is dropped the instant RunCCMDI cleans up, having never been seen. Arming a flag and opening the prompt
** from the next frame's poll lands after all of that, with CurrentMenu back to the Handhelds page, which
** is also the parent the prompt wants so that Cancel returns there.
**
** NO FProjectionScope, deliberately, and it is worth saying why given how much of this fork's aux code
** runs inside one. That scope exists for UI run as a PASSIVE PROJECTION with no player having asked for
** it (projectionscope.h). This prompt is the opposite: the player pressed a button, it is drawn on the
** real screen, and its sounds are meant to be heard. Suppressing them would be the bug.
*/

#include <string.h>

#include <exception>     // std::exception, the base a VM abort is caught through

#include "c_cvars.h"
#include "c_dispatch.h"
#include "cmdlib.h"      // countof
#include "dobject.h"     // must precede dobjtype.h, which refuses to be included on its own
#include "dobjtype.h"
#include "engineerrors.h" // CExitEvent, the exception a clean quit is
#include "m_misc.h"      // M_SaveDefaults, the ini write this button cannot do without
#include "menu.h"        // MenuDescriptors, DOptionMenuDescriptor, CurrentMenu, M_ActivateMenu
#include "menustate.h"   // menuactive, which closes the window an answer is accepted in
#include "printf.h"
#include "symbols.h"
#include "types.h"       // PPrototype, PFunctionPointer, TypeBool, TypeVoid
#include "vm.h"
#include "zstring.h"

#include "i_auxdevicepicker.h"
#include "i_auxdevicereset.h"
#include "i_auxvmreflect.h"

// Same as the other aux files: the reflection helpers live in AuxView because i_auxcanvas.cpp would
// otherwise redefine three of its names at file scope.
using namespace AuxView;

// Unique to this revision and printed by the console command, so a change can be proved to be IN the
// packaged library - `strings libSelaco.so | grep AUXDEVICERESET_BUILD` - before anything is read into
// device behaviour. Every exit code in the Android packaging chain reports success either way.
static const char *const AuxDeviceResetBuild = "AUXDEVICERESET_BUILD_20260919_M7B_R1";

static const char *const AuxSubsystem = "AuxDeviceReset";

// Ends every resolver line, so a failure says what the PLAYER loses rather than what a symbol was.
static const char *const AuxDisabledNote = "the reset button is not added";

// ----------------------------------------------------------------------------------------------
// THE ANSWER CHANNEL.
//
// The prompt's callback can reach a cvar and nothing else, so this is how the player's answer crosses back
// from UI scope into C++. It holds the controlID of the button that was pressed - -1 meaning "no answer" -
// and AuxDeviceResetFrame consumes it on the next frame.
//
// DELIBERATELY NOT CVAR_ARCHIVE. An answer is valid for the few frames between the press and the poll;
// persisting one would mean a launch could start with a reset already pending.
//
// A VALUE HERE IS NOT SUFFICIENT AUTHORITY TO RESET. Anyone can type `aux_devicereset 1` at the console,
// and that is not a confirmation, so the poll only looks at this while it knows a prompt of ours is open
// (AuxAwaitingAnswer). That window is opened by opening the prompt and by nothing else.
// ----------------------------------------------------------------------------------------------
CVAR(Int, aux_devicereset, -1, 0)

// The two button indices, which are the whole contract between this file and the ZScript callback: the
// callback reports an index and says nothing about what it means, so the meaning lives here only.
// initNew takes the buttons in order, so index 0 is the first argument and index 1 the second.
static const int AuxCancelButton = 0;
static const int AuxConfirmButton = 1;

// ----------------------------------------------------------------------------------------------
// THE COPY. Literals rather than $KEYS for the title and the body because there is no LANGUAGE entry for
// them and this port does not add one to the player's ipk3; StringTable.Localize passes a string with no
// leading $ through unchanged, which is how the "Handhelds" rename in i_auxpanel.cpp already works.
//
// The two BUTTONS are $KEYS, because those two exist in Selaco's own LANGUAGE lump and are the words their
// other destructive prompt uses (som_option_view.zs:553), so taking them means the buttons are localised
// and read identically to the rest of the game.
// ----------------------------------------------------------------------------------------------
static const char *const AuxResetTitle = "Reset Device Choice";
static const char *const AuxResetBody =
	"This resets your settings and quits the game. The device picker will appear next time you launch. "
	"Your save files are kept.";
static const char *const AuxResetCancelLabel = "$CANCEL_BUTTON";
static const char *const AuxResetConfirmLabel = "$RESET_BUTTON";

// ----------------------------------------------------------------------------------------------
// THE MENU ITEM.
// ----------------------------------------------------------------------------------------------

// The handheld page, under the name Selaco gives it; i_auxpanel.cpp renames what the player SEES but not
// the descriptor key.
static const char *const AuxHandheldMenuName = "SteamDeckMenu";

// Selaco's item class and the field that identifies which of their handlers an item wants
// (options_items.zs:457-467). mFunc and NOT mAction is what the item is found by: the MENUDEF line gives
// its action as the literal "none" (MENUDEF.zsc:69), so matching on mAction would match every
// actionless item on the page.
static const char *const AuxTooltipCommandClassName = "OptionMenuItemTooltipCommand";
static const char *const AuxTooltipFuncFieldName = "mFunc";
static const char *const AuxSteamDeckFunc = "setSteamDeck";

// The STOCK command item, not Selaco's TooltipCommand, and that choice decides how the item is dispatched:
// their option view runs SelacoOptionActivator.RunCCMDI directly for a command item that is not a
// TooltipCommand (som_option_view.zs:578), which is the path the device picker's own entries already take.
// A TooltipCommand would instead be routed through their mFunc ladder, which has no branch for ours.
// The cost is that a stock item carries no tooltip, so this one entry has no description line under it.
static const char *const AuxCommandItemClassName = "OptionMenuItemCommand";

static const char *const AuxResetItemLabel = "Reset Device Choice";

// The command the item runs, which is also its mAction: OptionMenuItemCommand.Activate looks itself up by
// mAction in the current menu's descriptor before running anything (optionmenuitems.zs:194-198), and
// RunCCMDI builds a one-item descriptor for exactly that lookup to succeed.
static const char *const AuxResetCommand = "aux_resetdevice";

static DOptionMenuDescriptor *AuxHandheldDescriptor()
{
	DMenuDescriptor **descp = MenuDescriptors.CheckKey(AuxHandheldMenuName);
	if (descp == nullptr || *descp == nullptr || !(*descp)->IsKindOf(RUNTIME_CLASS(DOptionMenuDescriptor)))
		return nullptr;
	return static_cast<DOptionMenuDescriptor *>(*descp);
}

// ----------------------------------------------------------------------------------------------
// THE PROMPT BRIDGE: Selaco's PromptMenu, our callback, and the three fields that join them.
// ----------------------------------------------------------------------------------------------

static const char *const AuxPromptClassName = "PromptMenu";
static const char *const AuxPromptInitFuncName = "initNew";
static const char *const AuxOnClosedFieldName = "onClosed";
static const char *const AuxReceiverFieldName = "receiver";
static const char *const AuxAllowBackFieldName = "allowBack";

// Ours, from wadsrc/static/zscript/engine/aux_devicereset.zs.
static const char *const AuxCallbackClassName = "AuxDeviceResetPrompt";
static const char *const AuxCallbackFuncName = "Answered";

// initNew's full declared argument list (prompt.zs:107), all eleven of them:
//
//   Menu parent, string title, string txt, string btn1, string btn2, string btn3, string btn4,
//   int destructiveIndex, int defaultSelection, String image, Vector2 imgScale
//
// EVERY ONE IS PASSED EXPLICITLY, which is why this is a plain VMCall and not the VMCallWithDefaults the
// rest of this port uses for an Init. The two arguments that matter most - destructiveIndex and
// defaultSelection - sit BEHIND four button slots, so they cannot be reached without filling those in
// anyway; and passing the whole list makes ResolveMethod's NumArgs check the entire guarantee that the
// callee reads no further than this array, with no second assumption about DefaultArgs's length.
static const EArgKind AuxInitNewArgs[] =
{
	Arg_Menu,
	Arg_String, Arg_String, Arg_String, Arg_String, Arg_String, Arg_String,
	Arg_Int, Arg_Int,
	Arg_String,
	Arg_Vector2,
};

// Self, plus one register each for the pointer, the six strings and the two ints, plus TWO for the
// Vector2 (types.cpp:365, the reason a register count cannot be inferred from an argument count).
static const int AuxInitNewRegs = 13;

// The kinds of field this file writes. NOT i_auxvmreflect.h's ResolveField: two of the three are kinds it
// does not know - a function pointer and a bare Object - and teaching it two new kinds means editing the
// resolver the second screen depends on for the sake of one button. It also ends every failure line with
// "second-screen view disabled", which is not what is lost here.
enum EAuxPromptFieldKind
{
	AuxPromptField_Bool,
	AuxPromptField_ObjectPtr,
	AuxPromptField_FuncPtr,
};

// A plain instance field on cls, proved to exist and to be the kind we are about to write. A wrong offset
// here is memory corruption rather than a misdraw, so nothing is inferred from the name.
//
// expectCls is read by AuxPromptField_ObjectPtr and ignored otherwise: it is the class of the instance we
// intend to store, which has to derive from whatever the field is declared as for the store to be
// type-safe for script.
static PField *AuxPromptField(PClass *cls, const char *fieldname, EAuxPromptFieldKind kind,
	PClass *expectCls = nullptr)
{
	// noCreate: never add a name to the table merely to look one up.
	const FName name(fieldname, true);
	PField *field = name != NAME_None ? dyn_cast<PField>(cls->FindSymbol(name, true)) : nullptr;
	if (field == nullptr)
	{
		Printf(TEXTCOLOR_YELLOW "%s: %s.%s is not a field, %s\n", AuxSubsystem,
			cls->TypeName.GetChars(), fieldname, AuxDisabledNote);
		return nullptr;
	}

	// A static or meta field's Offset is not an offset into the instance at all, which would make the
	// store write somewhere other than where it appears to. BitValue is deliberately not tested, for the
	// reason spelled out in i_auxvmreflect.cpp's ResolveField: it holds uninitialised garbage for every
	// field that is not a bitfield, and no script-declared field is ever one.
	if (field->Flags & (VARF_Native | VARF_Static | VARF_Meta))
	{
		Printf(TEXTCOLOR_YELLOW "%s: %s.%s is not a plain instance field, %s\n", AuxSubsystem,
			cls->TypeName.GetChars(), fieldname, AuxDisabledNote);
		return nullptr;
	}

	bool ok = false;
	switch (kind)
	{
	case AuxPromptField_Bool:
		ok = field->Type == TypeBool;
		break;

	case AuxPromptField_ObjectPtr:
		// PromptMenu.receiver, declared as the core `Object`. Descendant so a field declared as a base of
		// our class still matches, which is what makes the store assignable in script's own terms.
		ok = field->Type != nullptr && field->Type->isObjectPointer()
			&& static_cast<PObjectPointer *>(field->Type)->PointedClass() != nullptr
			&& expectCls != nullptr
			&& expectCls->IsDescendantOf(static_cast<PObjectPointer *>(field->Type)->PointedClass());
		break;

	case AuxPromptField_FuncPtr:
		// A ZScript Function<...> field. Only the SHAPE is checked here - that it is a function pointer at
		// all, and that its prototype is not the untyped Function<void> whose call convention nothing can
		// know. Whether OUR function may actually be stored in it is a separate and much sharper question,
		// answered by the engine's own NativeFunctionPointerCast in the resolve below rather than by a
		// hand-rolled comparison here.
		//
		// TYPE_FunctionPointer is set only by PFunctionPointer's constructor and TYPE_ObjectPointer only by
		// PObjectPointer's (types.cpp:1592, :3097), so this kind and the one above cannot both match.
		{
			PFunctionPointer *fp = PType::toFunctionPointer(field->Type);
			ok = fp != nullptr && fp->PointedType != nullptr && fp->PointedType != TypeVoid;
		}
		break;
	}

	if (!ok)
	{
		Printf(TEXTCOLOR_YELLOW "%s: %s.%s has an unexpected type, %s\n", AuxSubsystem,
			cls->TypeName.GetChars(), fieldname, AuxDisabledNote);
		return nullptr;
	}

	return field;
}

// The engine's OWN test for "may this PFunction be stored in a field of this Function<> type", and the
// reason none of that comparison is written by hand here: it is the function ZScript itself calls to
// validate a function-pointer cast at runtime (codegen.cpp:12662, exported as BuiltinFunctionPtrCast and
// used again by PFunctionPointer::ReadValue when a savegame is loaded). It checks the scope sides, the
// argument and return types and the per-argument flags in one place that cannot drift from what the
// compiler accepts. Returns `from` when the store is legal and null when it is not.
//
// Declared here rather than in a shared header because that is what the engine does with it - both
// codegen.cpp and types.cpp declare it locally, it appears in no header at all, and this is the only
// translation unit in the fork that needs it.
PFunction *NativeFunctionPointerCast(PFunction *from, const PFunctionPointer *to);

// Resolved together and cached, because the button is not installed at all unless ALL of it resolved - see
// the asymmetry at the top of this file.
//
// KEYED ON THE PromptMenu CLASS rather than on a bare "already tried" flag, the same way TightenTabStrip
// is keyed on its menu class: a script recompile builds new PClass, PField and PFunction objects and frees
// the old ones, so a stale PField here would be a write through a freed offset rather than a missed
// button. One attempt per class either way - the class is recorded before the resolve, so a failure is
// said once and not once per press. The separate "attempted" flag is what makes the FIRST attempt run even
// when the class is absent, so a game with no PromptMenu still gets its one line.
static bool AuxBridgeAttempted = false;
static PClass *AuxBridgeClass = nullptr;
static PClass *AuxPromptClass = nullptr;
static PClass *AuxCallbackClass = nullptr;
static PFunction *AuxCallbackFunc = nullptr;
static VMFunction *AuxFuncInitNew = nullptr;
static PField *AuxFldOnClosed = nullptr;
static PField *AuxFldReceiver = nullptr;
static PField *AuxFldAllowBack = nullptr;   // optional; see the resolve

// Held across a VMCall that allocates, so both need a GC root of their own - a raw C++ local is not one.
// The receiver is created once and kept for the process because it carries no state; the prompt is rooted
// only from its creation until CurrentMenu takes over.
static DObject *AuxPendingPrompt = nullptr;
static DObject *AuxReceiver = nullptr;

static bool AuxResolveBridge()
{
	// noCreate is implicit in FindClass, and a missing PromptMenu is a genuine "not this game" rather than
	// an error: a non-Selaco iwad has no styled confirmation to ask with, and therefore no button.
	PClass *promptCls = PClass::FindClass(AuxPromptClassName);
	if (AuxBridgeAttempted && promptCls == AuxBridgeClass)
		return AuxFuncInitNew != nullptr;

	// Recorded BEFORE the resolve, so a resolve that fails still counts as the one attempt for this class.
	AuxBridgeAttempted = true;
	AuxBridgeClass = promptCls;
	AuxPromptClass = nullptr;
	AuxCallbackClass = nullptr;
	AuxCallbackFunc = nullptr;
	AuxFuncInitNew = nullptr;
	AuxFldOnClosed = nullptr;
	AuxFldReceiver = nullptr;
	AuxFldAllowBack = nullptr;

	if (promptCls == nullptr)
	{
		Printf(TEXTCOLOR_YELLOW "%s: no %s class - not Selaco, or renamed, %s\n", AuxSubsystem,
			AuxPromptClassName, AuxDisabledNote);
		return false;
	}

	// The prompt is activated through M_ActivateMenu, which takes a DMenu. Proved rather than assumed
	// because the cast that follows is the one mistake here that corrupts memory.
	if (!promptCls->IsDescendantOf(RUNTIME_CLASS(DMenu)))
	{
		Printf(TEXTCOLOR_YELLOW "%s: %s is not a Menu, %s\n", AuxSubsystem, AuxPromptClassName,
			AuxDisabledNote);
		return false;
	}

	// CreateNew calls I_Error on either of these rather than returning null (dobjtype.cpp:433-437), so an
	// abstract or non-constructible prompt has to be caught HERE - the whole asymmetry at the top of this
	// file is that a button which cannot raise its confirmation is simply not installed, and an I_Error at
	// the press is the opposite of that.
	if (promptCls->bAbstract || promptCls->ConstructNative == nullptr)
	{
		Printf(TEXTCOLOR_YELLOW "%s: %s cannot be instantiated, %s\n", AuxSubsystem, AuxPromptClassName,
			AuxDisabledNote);
		return false;
	}

	// OUR OWN CALLBACK, proved the same way anything of Selaco's is. It lives in gzdoom.pk3 so it is not
	// meant to be able to go missing, but a ZScript lump this port ships is still a lump: if the class or
	// the function is not there, the button is not installed rather than installed and inert.
	PClass *callbackCls = PClass::FindClass(AuxCallbackClassName);
	const FName callbackName(AuxCallbackFuncName, true);
	PFunction *callbackFunc = callbackCls != nullptr && callbackName != NAME_None
		? dyn_cast<PFunction>(callbackCls->FindSymbol(callbackName, false)) : nullptr;
	if (callbackFunc == nullptr || callbackFunc->Variants.Size() != 1)
	{
		Printf(TEXTCOLOR_YELLOW "%s: no %s.%s to answer the prompt with, %s\n", AuxSubsystem,
			AuxCallbackClassName, AuxCallbackFuncName, AuxDisabledNote);
		return false;
	}

	// AuxResetReceiver CreateNew()s this, and CreateNew I_Errors rather than returning null on either of
	// these (dobjtype.cpp:433-437). Ours by construction, but proved for the same reason the prompt class is.
	if (callbackCls->bAbstract || callbackCls->ConstructNative == nullptr)
	{
		Printf(TEXTCOLOR_YELLOW "%s: %s cannot be instantiated, %s\n", AuxSubsystem,
			AuxCallbackClassName, AuxDisabledNote);
		return false;
	}

	// A ZScript static is identified by the ABSENCE of an implied self, not by a positive flag
	// (zcc_compile.cpp:2507 clears VARF_Method for one and never sets VARF_Static, which only ever applies
	// to fields). Checked explicitly even though NativeFunctionPointerCast below would also reject a
	// method on its argument count, because "it is not a static" is the useful thing to print.
	if (callbackFunc->Variants[0].Flags & (VARF_Method | VARF_Action))
	{
		Printf(TEXTCOLOR_YELLOW "%s: %s.%s is not a static, %s\n", AuxSubsystem,
			AuxCallbackClassName, AuxCallbackFuncName, AuxDisabledNote);
		return false;
	}

	int regs = 0;
	VMFunction *initNew = ResolveMethod(promptCls, AuxPromptInitFuncName, AuxSubsystem, AuxInitNewArgs,
		(unsigned)countof(AuxInitNewArgs), &regs, nullptr, AuxDisabledNote);
	if (initNew == nullptr)
		return false;

	// ResolveMethod proves the arguments and the register count but says nothing about the return, and
	// initNew returns `self`. Calling a function that DOES return while passing numret 0 has the callee's
	// RET write past the end of a zero-length array, so the return is proved and then received.
	PPrototype *proto = initNew->Proto;
	if (proto == nullptr || proto->ReturnTypes.Size() != 1 || proto->ReturnTypes[0] == nullptr
		|| proto->ReturnTypes[0]->GetRegType() != REGT_POINTER)
	{
		Printf(TEXTCOLOR_YELLOW "%s: %s.%s does not return a single pointer, %s\n", AuxSubsystem,
			AuxPromptClassName, AuxPromptInitFuncName, AuxDisabledNote);
		return false;
	}

	// The argument list this file hands over is written out by hand, so the count it implies is checked
	// against the count ResolveMethod derived from the same table. They can only disagree if AuxInitNewArgs
	// and AuxInitNewRegs were edited apart from each other.
	if (regs != AuxInitNewRegs)
	{
		Printf(TEXTCOLOR_YELLOW "%s: %s.%s wants %d registers, this passes %d, %s\n", AuxSubsystem,
			AuxPromptClassName, AuxPromptInitFuncName, regs, AuxInitNewRegs, AuxDisabledNote);
		return false;
	}

	PField *fldOnClosed = AuxPromptField(promptCls, AuxOnClosedFieldName, AuxPromptField_FuncPtr);
	if (fldOnClosed == nullptr)
		return false;

	// THE DECISIVE CHECK on the callback: not "does it look right" but "would the compiler let this be
	// assigned". Our function is plain-scope, which CheckSidesForFunctionPointer accepts into a pointer of
	// any scope (scopebarrier.cpp:211-217), so this passes whether they declare the field ui, play or
	// clearscope - and fails loudly if they ever change its signature.
	PFunctionPointer *onClosedType = PType::toFunctionPointer(fldOnClosed->Type);
	if (NativeFunctionPointerCast(callbackFunc, onClosedType) != callbackFunc)
	{
		Printf(TEXTCOLOR_YELLOW "%s: %s.%s cannot hold %s.%s (%s), %s\n", AuxSubsystem,
			AuxPromptClassName, AuxOnClosedFieldName, AuxCallbackClassName, AuxCallbackFuncName,
			onClosedType->DescriptiveName(), AuxDisabledNote);
		return false;
	}

	PField *fldReceiver = AuxPromptField(promptCls, AuxReceiverFieldName, AuxPromptField_ObjectPtr,
		callbackCls);
	if (fldReceiver == nullptr)
		return false;

	// THE ONLY OPTIONAL PIECE. allowBack is what lets a controller B press or Escape dismiss the prompt
	// (prompt.zs:378-384); without it the prompt is still perfectly answerable, it just has to be answered
	// with its own Cancel button. So this one failure is a line and not a refusal - and note which way it
	// fails, because that matters here: a prompt that cannot be backed out of is inconvenient, whereas a
	// prompt that could be dismissed INTO a reset would be the bug this whole file is arranged against.
	// Backing out never calls onClosed at all, so it can only ever mean "no".
	PField *fldAllowBack = AuxPromptField(promptCls, AuxAllowBackFieldName, AuxPromptField_Bool);
	if (fldAllowBack == nullptr)
	{
		Printf(TEXTCOLOR_YELLOW "%s: the prompt cannot be dismissed with Back, only with its Cancel "
			"button\n", AuxSubsystem);
	}

	// Registered before anything can allocate, and ONCE for the process: the marker closes over the two
	// globals rather than over a particular object, and there is no RemoveMarkerFunc, so re-registering on
	// a later resolve would append a second marker doing the same work on every collection forever.
	static bool markerRegistered = false;
	if (!markerRegistered)
	{
		GC::AddMarkerFunc([]() { GC::Mark(AuxPendingPrompt); GC::Mark(AuxReceiver); });
		markerRegistered = true;
	}

	AuxPromptClass = promptCls;
	AuxCallbackClass = callbackCls;
	AuxCallbackFunc = callbackFunc;
	AuxFuncInitNew = initNew;
	AuxFldOnClosed = fldOnClosed;
	AuxFldReceiver = fldReceiver;
	AuxFldAllowBack = fldAllowBack;
	return true;
}

// The object that goes in PromptMenu.receiver. Created on the first press rather than at startup, so a
// launch that never touches the button allocates nothing, and then kept: it carries no state, so one
// instance serves every press.
//
// IT IS NOT READ BY THE CALLBACK. `receiver` is the other half of Selaco's calling convention rather than
// something this needs - onClosed.call passes it straight through - and it is filled in so the prompt is
// left in exactly the shape their own callers leave it.
static DObject *AuxResetReceiver()
{
	if (AuxReceiver == nullptr && AuxCallbackClass != nullptr)
	{
		AuxReceiver = AuxCallbackClass->CreateNew();
		GC::WriteBarrier(AuxReceiver);   // the pointer is not inside an object, same case as menu.cpp:372
	}
	return AuxReceiver;
}

// ----------------------------------------------------------------------------------------------
// OPENING IT.
// ----------------------------------------------------------------------------------------------

// Set by the CCMD the menu item runs, cleared by the poll that opens the prompt.
static bool AuxPromptWanted = false;

// True from the moment a prompt of ours is on screen until an answer is consumed or every menu closes.
// This is what stops a hand-written `aux_devicereset 1` from counting as a confirmation.
static bool AuxAwaitingAnswer = false;

static bool AuxOpenResetPrompt()
{
	if (!AuxResolveBridge())
		return false;

	// The prompt is opened OVER whatever menu the button was pressed from, which is what makes its Cancel
	// return to the Handhelds page. No menu at all means that page went away between the press and this
	// frame; a confirmation nobody is looking at must not be openable, so nothing is.
	if (CurrentMenu == nullptr)
	{
		Printf(TEXTCOLOR_YELLOW "%s: the menu closed before the confirmation could be shown, nothing has "
			"been reset\n", AuxSubsystem);
		return false;
	}

	DObject *receiver = AuxResetReceiver();
	if (receiver == nullptr)
	{
		Printf(TEXTCOLOR_YELLOW "%s: %s could not be created, nothing has been reset\n", AuxSubsystem,
			AuxCallbackClassName);
		return false;
	}

	// ROOTED BEFORE THE CALL THAT ALLOCATES. initNew builds the whole prompt - a background, a title, a
	// body label, a layout and two buttons - and any allocation can run a GC step, so the menu has to be
	// reachable from a root by the first one.
	DObject *prompt = AuxPromptClass->CreateNew();
	AuxPendingPrompt = prompt;
	GC::WriteBarrier(prompt);

	try
	{
		// Locals, because a String argument is passed as the ADDRESS of an FString the callee copies out
		// of; they have to outlive the call and nothing more.
		FString title = AuxResetTitle;
		FString body = AuxResetBody;
		FString cancel = AuxResetCancelLabel;
		FString confirm = AuxResetConfirmLabel;
		FString unusedButton;   // btn3 and btn4, which initNew skips when they are empty
		FString noImage;

		// CANCEL FIRST, CONFIRM SECOND, and that ordering is the whole reason the indices above read the
		// way they do: btn1 becomes controlID 0 and btn2 controlID 1 (prompt.zs:231-256).
		// destructiveIndex 1 gives the confirm button Selaco's red styling; defaultSelection 0 puts the
		// cursor on Cancel, so a player who presses A twice by reflex cancels.
		//
		// The last three arguments reproduce initNew's own declared defaults (prompt.zs:107) rather than
		// relying on them: no image, and the scale an absent image is given anyway.
		VMValue params[] =
		{
			prompt, CurrentMenu, &title, &body, &cancel, &confirm, &unusedButton, &unusedButton,
			AuxConfirmButton, AuxCancelButton, &noImage, 1.0, 1.0
		};
		static_assert(countof(params) == AuxInitNewRegs, "initNew's register count and this list disagree");

		void *returned = nullptr;
		VMReturn ret;
		ret.PointerAt(&returned);
		VMCall(AuxFuncInitNew, params, AuxInitNewRegs, &ret, 1);

		if (returned != prompt)
		{
			// initNew returns self on every path in the game's source; anything else means it took a branch
			// this code has not read. Nothing has been activated, so dropping the root is the whole
			// cleanup: the half-built prompt is collected and no menu of theirs has been touched.
			AuxPendingPrompt = nullptr;
			Printf(TEXTCOLOR_YELLOW "%s: %s.%s did not return self, nothing has been reset\n", AuxSubsystem,
				AuxPromptClassName, AuxPromptInitFuncName);
			return false;
		}

		// onClosed holds a PFunction, not an object reference, and NO WRITE BARRIER BELONGS HERE.
		// PFunctionPointer derives from PPointer rather than PObjectPointer, and only PObjectPointer
		// registers a field for tracing (types.cpp:1603), so the GC never walks this field - and the
		// PFunction itself was removed from the GC entirely when its symbol table adopted it
		// (symbols.cpp:323, "no more GC, please!"). A barrier here would protect nothing and imply
		// otherwise.
		*(PFunction **)((uint8_t *)prompt + AuxFldOnClosed->Offset) = AuxCallbackFunc;

		// receiver IS a traced object pointer, so this one does need the barrier.
		*(DObject **)((uint8_t *)prompt + AuxFldReceiver->Offset) = receiver;
		GC::WriteBarrier(prompt, receiver);

		if (AuxFldAllowBack != nullptr)
			*(bool *)((uint8_t *)prompt + AuxFldAllowBack->Offset) = true;
	}
	catch (const std::exception &e)
	{
		// A script abort inside their prompt must not take the frame down with it, and it must not be read
		// as an answer either: the window below is never opened, so the player is simply left on the
		// Handhelds page with nothing changed.
		AuxPendingPrompt = nullptr;
		Printf(TEXTCOLOR_YELLOW "%s: building the confirmation aborted (%s), nothing has been reset\n",
			AuxSubsystem, e.what());
		return false;
	}

	// Proved to be a Menu at resolve time, which is what makes this cast rather than a dynamic check.
	M_ActivateMenu(static_cast<DMenu *>(prompt));

	// CurrentMenu is a GC root of its own (M_MarkMenus, menu.cpp:184), so the local root is handed back.
	AuxPendingPrompt = nullptr;

	// The window in which an answer counts, opened here and nowhere else.
	aux_devicereset = -1;
	AuxAwaitingAnswer = true;
	return true;
}

// ----------------------------------------------------------------------------------------------
// DOING IT: clear, persist, quit, in that order and no other.
// ----------------------------------------------------------------------------------------------

// Returns only when the clear failed; otherwise it quits. The quit is a CExitEvent, which is what
// CCMD(quit) throws (c_enginecmds.cpp:61-64) and what D_DoomMain catches to run its own shutdown
// (d_main.cpp:4160) - D_DoomLoop catches only CRecoverableError, FileSystemException and CVMAbortException,
// so nothing in between swallows it. Reached from D_Display's poll, which sits BEFORE
// screen->BeginFrame(), so the throw cannot unwind out of a half-open frame.
//
// Bare like CCMD(quit) rather than through M_Quit, which additionally stops the music and the screen jobs
// (doommenu.cpp:366-372). The shutdown D_DoomMain runs does all of that anyway; the only visible difference
// is that the menu music plays for the fraction of a second the shutdown takes.
static void AuxDoReset()
{
	// THE CLEAR AND THE RESTORE ARE ONE CALL because those two facts have to move together: a cleared
	// device with Selaco's first-run dialogs still suppressed is the state their three first-run bugs come
	// from, and the player would get neither the picker nor their dialogs. See i_auxdevicepicker.h.
	if (!AuxDeviceChoiceClear())
	{
		// Said its own line already. Nothing is written and nothing is quit: a launch that came back with
		// the same device is a button that visibly did nothing, which is far better than a launch with no
		// first-run anything.
		Printf(TEXTCOLOR_YELLOW "%s: the device choice was not cleared, the game stays up\n", AuxSubsystem);
		return;
	}

	// THE ONE STEP THIS BUTTON CANNOT DO WITHOUT. aux_device is CVAR_ARCHIVE|CVAR_GLOBALCONFIG, which
	// means it reaches the ini only when something archives it - and a process that is killed rather than
	// quit never gets there, which was measured on device to lose the recorded device every time. The
	// whole point of this button is that the picker returns on the next launch, so the cleared value goes
	// to disk HERE rather than on the trust that the quit will complete.
	//
	// M_SaveDefaults(nullptr) is the engine's own write and the same call CCMD(writeini) and
	// M_SaveDefaultsFinal make (m_misc.cpp:390-431): ArchiveGlobalData writes every
	// CVAR_ARCHIVE|CVAR_GLOBALCONFIG cvar into [GlobalSettings] (gameconfigfile.cpp:594) and then
	// WriteConfigFile puts the file on disk. It returns whether that succeeded.
	if (!M_SaveDefaults(nullptr))
	{
		// QUIT ANYWAY, and say so. The shutdown this throw runs into calls M_SaveDefaultsFinal, which
		// retries the same write in a loop against I_WriteIniFailed - so the second attempt is strictly
		// better placed than a third from here, and staying up would leave the player looking at a button
		// that did nothing while the choice is already cleared in memory.
		Printf(TEXTCOLOR_YELLOW "%s: writing the config failed, the shutdown will try again\n",
			AuxSubsystem);
	}
	else
	{
		Printf("%s: device choice cleared and written, Selaco's first-run dialogs restored - quitting\n",
			AuxSubsystem);
	}

	throw CExitEvent(0);
}

// ----------------------------------------------------------------------------------------------
// THE POLL. Once per frame, from I_AuxDevicePickerFrame.
// ----------------------------------------------------------------------------------------------

void AuxDeviceResetFrame()
{
	if (AuxPromptWanted)
	{
		// One attempt per press either way: a failure has already said what happened, and retrying it every
		// frame would only spam a player who is still looking at the page.
		AuxPromptWanted = false;
		AuxOpenResetPrompt();
		return;
	}

	if (!AuxAwaitingAnswer)
		return;

	const int answer = aux_devicereset;
	if (answer < 0)
	{
		// No answer yet. The prompt calls back on both of its buttons, but a Back press closes it silently
		// (prompt.zs:378-384) and never calls onClosed at all, so the window is also closed by every menu
		// going away - otherwise a dismissed prompt would leave it open for the rest of the session.
		if (menuactive == MENU_Off)
			AuxAwaitingAnswer = false;
		return;
	}

	// Consumed whichever button it was, so a stale value can never be read twice.
	aux_devicereset = -1;
	AuxAwaitingAnswer = false;

	if (answer != AuxConfirmButton)
		return;   // Cancel. Nothing to say and nothing to do.

	AuxDoReset();
}

// ----------------------------------------------------------------------------------------------
// INSTALLING THE BUTTON, once, after M_Init has parsed every MENUDEF.
// ----------------------------------------------------------------------------------------------

void AuxDeviceResetInitMenu()
{
	// EVERYTHING IS PROVED BEFORE ANYTHING OF SELACO'S IS TOUCHED, and here that is not tidiness but the
	// safety property at the top of this file: a button that cannot raise its confirmation must not exist,
	// because the alternative is a press that either does nothing or - much worse - is tempted into doing
	// the reset unconfirmed. Their item stays exactly as it shipped.
	if (!AuxResolveBridge())
	{
		Printf(TEXTCOLOR_YELLOW "%s: the confirmation cannot be built, so the Handhelds page is left "
			"exactly as Selaco ships it\n", AuxSubsystem);
		return;
	}

	DOptionMenuDescriptor *desc = AuxHandheldDescriptor();
	if (desc == nullptr)
	{
		Printf(TEXTCOLOR_YELLOW "%s: no %s option menu, %s\n", AuxSubsystem, AuxHandheldMenuName,
			AuxDisabledNote);
		return;
	}

	// NOT searching parents, and that is load-bearing rather than tidiness: OptionMenuItemCommand declares
	// its own four-argument Init, while the OptionMenuItemSubmenu.Init it would fall back to takes a
	// String, a Name, an int and a bool. Calling the wrong one with our arguments is the read-past-the-array
	// crash i_auxvmreflect.h opens with.
	PClass *cls = PClass::FindClass(AuxCommandItemClassName);
	PFunction *init = (cls != nullptr) ? dyn_cast<PFunction>(cls->FindSymbol("Init", false)) : nullptr;
	if (init == nullptr)
	{
		Printf(TEXTCOLOR_YELLOW "%s: no %s.Init, %s\n", AuxSubsystem, AuxCommandItemClassName,
			AuxDisabledNote);
		return;
	}

	// FOUND BY mFunc, NOT BY POSITION AND NOT BY mAction. Their MENUDEF line gives the item's action as the
	// literal "none" (MENUDEF.zsc:69), so mAction identifies nothing; mFunc is the field their option view
	// dispatches on (som_option_view.zs:541-575) and therefore the only thing that actually says which item
	// this is. Position is avoided for the reason i_auxpanel.cpp avoids it on the Options list: a MENUDEF
	// line can move.
	PClass *tooltipCls = PClass::FindClass(AuxTooltipCommandClassName);
	PField *fldFunc = tooltipCls != nullptr
		? ResolveField(tooltipCls, AuxTooltipFuncFieldName, Field_String, nullptr, AuxSubsystem, AuxDisabledNote)
		: nullptr;
	if (fldFunc == nullptr)
	{
		// Only the missing-class case is said here. ResolveField is given this file's own subsystem and note,
		// so when it is the one that fails it already prints "AuxDeviceReset: <class>.mFunc is not a field, the
		// reset button is not added" - it used to take the default and blame the standby codex, which is why a
		// second line correcting it stood here.
		if (tooltipCls == nullptr)
		{
			Printf(TEXTCOLOR_YELLOW "%s: no %s to identify their button by, %s\n", AuxSubsystem,
				AuxTooltipCommandClassName, AuxDisabledNote);
		}
		return;
	}

	unsigned index = desc->mItems.Size();
	for (unsigned i = 0; i < desc->mItems.Size(); i++)
	{
		DMenuItemBase *item = desc->mItems[i];
		if (item == nullptr || !item->IsKindOf(tooltipCls))
			continue;
		if (strcmp(StringFieldAddr(item, fldFunc)->GetChars(), AuxSteamDeckFunc) == 0)
		{
			index = i;
			break;
		}
	}

	if (index >= desc->mItems.Size())
	{
		Printf(TEXTCOLOR_YELLOW "%s: no \"%s\" item on %s, %s\n", AuxSubsystem, AuxSteamDeckFunc,
			AuxHandheldMenuName, AuxDisabledNote);
		return;
	}

	// IN PLACE, NOT APPENDED. Theirs is the first item on the page, above the page's own group headings,
	// and that is where a reset belongs - appending would put it below the aim-assist options, reading as an
	// afterthought rather than as the thing it replaced. The old item is simply dropped: it is a DObject
	// held by nothing else once the slot stops pointing at it.
	//
	// STORED INTO THE ARRAY IMMEDIATELY, before the Init below rather than after, for the reason the device
	// picker's own item loop gives: Init runs script code that can allocate, and until the item is in mItems
	// it is held only by a C++ local, which is not a GC root. Init writes fields on an object it is handed,
	// so it does not care that the array already refers to it.
	//
	// No write barrier, matching every mItems write in the engine's own MENUDEF parser (menudef.cpp:574,
	// :1183, :1467) and the rest of this port: the barrier menudef.cpp:740 takes is for inserting the
	// DESCRIPTOR into MenuDescriptors, not for the items inside one.
	DMenuItemBase *ours = (DMenuItemBase *)cls->CreateNew();
	desc->mItems[index] = ours;

	// VMCallWithDefaults, not VMCall, so a build whose Init grew an argument keeps whatever value the
	// declaration gives it - exactly how the MENUDEF parser builds this same item.
	//
	// The two bools are Init's `centered` and `closeonselect`. closeonselect is FALSE, and that is a
	// requirement rather than a preference: true makes Activate close whatever menu is current the moment
	// the command returns (optionmenuitems.zs:202-206), and the page must still be there for the prompt to
	// open over on the next frame.
	FString label = AuxResetItemLabel;
	TArray<VMValue> params;
	params.Push(ours);
	params.Push(&label);
	params.Push(FName(AuxResetCommand).GetIndex());
	params.Push(false);
	params.Push(false);
	VMCallWithDefaults(init->Variants[0].Implementation, params, nullptr, 0);

	Printf("%s: build=%s - \"%s\" replaces \"Choose Optimal Settings\" at %s item %u\n", AuxSubsystem,
		AuxDeviceResetBuild, AuxResetItemLabel, AuxHandheldMenuName, index);
}

// ----------------------------------------------------------------------------------------------
// THE CONSOLE COMMAND, which is what the menu item runs.
//
// A PLAIN CCMD and not an UNSAFE_CCMD: their option view dispatches a command item with
// UnsafeExecutionScope set (vmnatives.cpp:1208), and an unsafe command refuses to run in one
// (c_dispatch.cpp:488-496). The device picker's own aux_setdevice is plain for the same reason.
//
// IT ONLY ASKS. Nothing is cleared, written or quit on this path - it arms the prompt and returns, and
// only the answer the player gives the prompt reaches AuxDoReset.
// ----------------------------------------------------------------------------------------------

CCMD(aux_resetdevice)
{
	Printf("%s: build=%s\n", AuxSubsystem, AuxDeviceResetBuild);
	Printf("%s: device=\"%s\"\n", AuxSubsystem, AuxDeviceChoice());

	if (!AuxResolveBridge())
	{
		// The resolvers have said which symbol failed. NO FALLBACK: without Selaco's prompt there is no way
		// to ask, and an action that quits the game is not one to take on an unasked question.
		Printf(TEXTCOLOR_YELLOW "%s: the confirmation cannot be shown, so nothing has been reset\n",
			AuxSubsystem);
		return;
	}

	// Armed rather than opened, for the reason given at the top of this file: a menu activated from inside
	// a CCMD is dropped again by RunCCMDI's own cleanup a moment later.
	AuxPromptWanted = true;
	Printf("%s: asking for confirmation on the next frame\n", AuxSubsystem);
}

// Forget every resolve this file latched, for the restart teardown. See I_AuxForgetScriptState in
// i_auxvmreflect.cpp for why it exists and when it runs.
//
// AuxReceiver IS THE SECOND HALF OF THE CRASH. It is created once and deliberately kept for the process,
// and the marker function above keeps marking it - so it survives every collection in D_Cleanup with its
// PClass deleted underneath it, and the first collection after the restart walks it. Nulling it here is
// what lets the collection inside PClass::StaticShutdown sweep it instead, while bVMOperational is already
// false and no scripted OnDestroy can run. Nothing here does VM work; every line is a store.
void I_AuxDeviceResetForgetScriptState()
{
	AuxPendingPrompt = nullptr;
	AuxReceiver = nullptr;

	// Keying the bridge on the PromptMenu class is the right shape but it compares POINTERS, and the
	// restart frees every PClass and then re-parses the same script in the same order - so the new
	// PromptMenu can land on the freed one's address, the compare matches, and the resolve is skipped with
	// all eight pointers below still pointing into freed memory. Clearing the key is what closes that.
	AuxBridgeAttempted = false;
	AuxBridgeClass = nullptr;
	AuxPromptClass = nullptr;
	AuxCallbackClass = nullptr;
	AuxCallbackFunc = nullptr;
	AuxFuncInitNew = nullptr;
	AuxFldOnClosed = nullptr;
	AuxFldReceiver = nullptr;
	AuxFldAllowBack = nullptr;

	// An answer owed to a prompt that no longer exists must not be honoured: AuxAwaitingAnswer is the only
	// thing separating our own confirmation from a hand-typed `aux_devicereset 1`, and the action it
	// authorises clears the device choice and quits.
	AuxPromptWanted = false;
	AuxAwaitingAnswer = false;
}
