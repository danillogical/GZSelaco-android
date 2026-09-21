/*
** i_auxdevicepicker.cpp
** The first-launch device picker: one menu of handhelds, built from the profiles that were actually
** shipped, shown once, and standing in for Selaco's own two first-run dialogs.
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
** WHAT MAKES REPLACING SELACO'S FIRST-RUN DIALOGS SAFE AT ALL.
**
** CLAUDE.md's "do not bypass the first-run dialog" is a record of three real bugs - walking instead of
** running, a 20x view bob and a stale gamepad layout with no weapon-wheel binding - all caused by
** g_tos being set by hand so that UIHelper.SetSteamdeckPresets() and Selaco's preset block never ran.
** This file is allowed to suppress those dialogs for exactly one reason: the profile applier does that
** work instead. Every shipped profile sets `steamdeck`, which calls their own SetSteamdeckPresets by
** reflection, and names a real GFXPreset* block which is applied out of Selaco's own MENUDEF.
**
** So the ordering is the safety argument, and it is enforced in two ways rather than trusted:
**
**   1. AuxDeviceChoiceSet is never called before AuxProfileApply has returned, and the selection
**      command refuses to record a choice when the profile asked for SetSteamdeckPresets and did not
**      get it (AuxProfileResult::steamdeckWanted without steamdeckApplied).
**   2. The suppression itself is one function, AuxSuppressFirstRun, reachable from exactly two places:
**      arming the picker and recording a selection. Its inverse, AuxRestoreFirstRun, is reachable from
**      every path that gives up. See THE ONE STATE THAT CANNOT BE LOST below for why arming has to
**      suppress, and what stops that from being a hole.
**
** THE ONE STATE THAT CANNOT BE LOST: "a picker is owed".
**
** Selaco opens its dialogs from TOSEventHandler.WorldTick at ticks == 0 (tos/tos_menu.zs:29-33), which
** is the FIRST world tick of TITLEMAP - long before the player could have picked anything. So their
** dialogs have to be suppressed while arming, not while recording, and that means there is a window in
** which they are suppressed and no profile has been applied. If that window could survive a launch it
** would be exactly the three-bug state.
**
** It cannot, because arming WRITES the window down. `aux_device` takes a reserved value meaning "a
** picker is owed" in the same operation that suppresses, so every subsequent launch either shows the
** picker again or, if it cannot, hands Selaco's dialogs straight back (AuxRestoreFirstRun and a cleared
** choice). There is therefore no launch on which the player has neither dialog: the flag and the
** suppression are written together and checked together.
**
** WHY THE MENU IS SHOWN FROM A PER-FRAME POLL AND NOT ONCE FROM D_DoomMain.
**
** IntroHandler.UITick (intro_handler.zs:326) forces IntroMenu over any menu that is not IntroMenu or
** StartupMenu for the whole title-screen intro - 498 ticks, about fourteen seconds. A plain option menu
** opened from D_DoomMain is therefore replaced a moment later and never comes back. Selaco's own
** dialogs are immune because they derive from StartupMenu, which this fork cannot name at compile time
** and would not want to inherit anyway.
**
** So the picker waits for the state IntroHandler itself leaves behind: TITLEMAP up, no menu at all,
** console up - which is the "press any key" screen, after the intro blocker has closed itself. That is
** the one moment nothing else wants the menu slot, so the poll never fights the engine, and because it
** is a poll rather than a one-shot it also re-opens the picker if the player backs out of it. A player
** who has not chosen keeps being asked, which is what makes the suppression above honest.
**
** NOTHING SELACO-SPECIFIC IS NAMED AT COMPILE TIME. IntroHandler's three static helpers are resolved
** and fully type-checked through i_auxvmreflect.h, so a missing, renamed or differently-built ipk3
** degrades to "the picker does not arm and Selaco's dialogs run" instead of refusing to start. In
** particular NOTE THAT `g_tos 1` IS NOT ENOUGH ON THE SHIPPED GAME: needsTOS() compares against
** IntroHandler.TOS_ID, which is 2, and that constant is removed from the symbol table by
** RemoveUnusedSymbols (d_main.cpp:3750) before any console command could read it. Calling their own
** clearNeedsTOS() writes whatever their current ID is and keeps doing so after they bump it.
*/

#include <string.h>

#include <exception>     // std::exception, the base a VM abort is caught through

#include "c_console.h"   // ConsoleState, so the picker never yanks the console away mid-command
#include "c_cvars.h"
#include "c_dispatch.h"
#include "dobject.h"     // must precede dobjtype.h, which refuses to be included on its own
#include "dobjtype.h"
#include "doomstat.h"
#include "gamestate.h"   // GS_TITLELEVEL, the only gamestate the picker will open itself in
#include "m_misc.h"      // M_SaveDefaults, the ini write a recorded choice cannot do without
#include "menu.h"        // MenuDescriptors, DOptionMenuDescriptor, CurrentMenu, M_SetMenu
#include "menustate.h"   // menuactive
#include "printf.h"
#include "symbols.h"
#include "types.h"       // PPrototype::ReturnTypes, checked because ResolveStaticMethod does not
#include "vm.h"
#include "zstring.h"

#include "i_auxdevicepicker.h"
#include "i_auxdevicereset.h"
#include "i_auxprofile.h"
#include "i_auxvmreflect.h"

// Same as the other aux files: the reflection helpers live in AuxView because i_auxcanvas.cpp would
// otherwise redefine three of its names at file scope.
using namespace AuxView;

// Unique to this revision and printed by both console commands, so a change can be proved to be IN the
// packaged library - `strings libSelaco.so | grep AUXDEVICEPICKER_BUILD` - before anything is read into
// device behaviour. Every exit code in the Android packaging chain reports success either way.
static const char *const AuxDevicePickerBuild = "AUXDEVICEPICKER_BUILD_20260918_M7A_R1";

// ----------------------------------------------------------------------------------------------
// THE CHOICE.
//
// A STRING AND NOT A BOOL, deliberately: the value is the profile's file stem, which is what makes a
// log line say which device a session is running as, and what lets a later step show or reset the
// current one without a second cvar to keep in step.
//
// CVAR_ARCHIVE so it survives a launch and CVAR_GLOBALCONFIG so it is not per-game - a handheld is a
// property of the machine, not of the iwad that happens to be loaded. Deliberately NOT g_tos: that one
// is Selaco's, means something else, and is reset by their own dialogs.
//
// Three reserved values as well as a stem, all of them empty-or-punctuation so they cannot collide with
// a filename:
//
//   ""  - nobody has been asked. The picker decides from Selaco's own first-run state.
//   "?" - A PICKER IS OWED. Written at the moment Selaco's dialogs are suppressed, so that the
//         suppression can never outlive the obligation. See THE ONE STATE THAT CANNOT BE LOST above.
//   "-" - a recorded non-choice: this install had already been through Selaco's first run when the
//         picker first ran, so it is not a first launch, nothing was suppressed and nothing is owed.
//
// Read and written only through the four functions in i_auxdevicepicker.h.
// ----------------------------------------------------------------------------------------------
CVAR(String, aux_device, "", CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

static const char *const AuxDeviceOwed = "?";
static const char *const AuxDeviceDeclined = "-";

const char *AuxDeviceChoice()
{
	return *aux_device;
}

bool AuxDeviceChoiceIsSet()
{
	const char *value = *aux_device;
	if (value == nullptr || *value == '\0')
		return false;
	return strcmp(value, AuxDeviceOwed) != 0;
}

// True only for the reserved "a picker is owed" value, which is what a later launch keys off.
static bool AuxDeviceChoiceIsOwed()
{
	const char *value = *aux_device;
	return value != nullptr && strcmp(value, AuxDeviceOwed) == 0;
}

void AuxDeviceChoiceSet(const char *profileStem)
{
	aux_device = profileStem != nullptr ? profileStem : "";
}

// ----------------------------------------------------------------------------------------------
// THE BRIDGE TO SELACO'S FIRST-RUN STATE, which is three of their own static methods and nothing else.
//
// Their two cvars are never written by name from here. g_tos is compared against IntroHandler.TOS_ID
// rather than against 1 (intro_handler.zs:62-65), and that constant is gone from the symbol table by
// the time any of this could read it (RemoveUnusedSymbols, d_main.cpp:3750), so a hardcoded 1 would
// leave needsTOS() true and both dialogs would appear on top of the picker. Their clearNeeds* writes
// the right value by construction, now and after they bump the ID.
//
// The one exception is the restore path, which has no Selaco equivalent - see AuxRestoreFirstRun.
// ----------------------------------------------------------------------------------------------

static const char *const AuxIntroHandlerClassName = "IntroHandler";
static const char *const AuxSubsystem = "AuxDevicePicker";

// Resolved once per IntroHandler class and cached. KEYED ON THE CLASS rather than on a bare "already
// tried" flag, the same way i_auxdevicereset.cpp keys its own bridge: a `restart` tears the script state
// down (PClass::StaticShutdown, d_main.cpp:4260) and D_InitGame runs I_AuxDevicePickerInit again, so a
// bare latch would leave these three pointing at freed VMFunctions and AuxSuppressFirstRun would VMCall
// one. Caching also means the arm step can PROVE the suppression will work before it commits to
// replacing anything of Selaco's.
static VMFunction *AuxFuncStartupCheck = nullptr;
static VMFunction *AuxFuncClearTOS = nullptr;
static VMFunction *AuxFuncClearVisibility = nullptr;
static PClass *AuxBridgeClass = nullptr;
static bool AuxBridgeAttempted = false;
static bool AuxBridgeResolved = false;

// ResolveStaticMethod proves the arguments and the register count but says nothing about the return, so
// the return is proved here. It matters in both directions: reading a bool out of a function that
// returns nothing leaves a stale int on the stack being trusted, and calling a function that DOES
// return while passing numret 0 has the callee's RET write past the end of a zero-length array.
static bool AuxReturnMatches(VMFunction *func, const char *funcname, bool wantInt)
{
	PPrototype *proto = func->Proto;
	const unsigned rets = proto != nullptr ? proto->ReturnTypes.Size() : 0u;

	if (rets != (wantInt ? 1u : 0u))
	{
		Printf(TEXTCOLOR_YELLOW "%s: %s.%s returns %u values, not %d\n", AuxSubsystem,
			AuxIntroHandlerClassName, funcname, rets, wantInt ? 1 : 0);
		return false;
	}

	// A ZScript bool comes back in an integer register, so the register class is what is checked rather
	// than the exact type - the same structural comparison, and for the same reason, as EArgKind.
	if (wantInt && (proto->ReturnTypes[0] == nullptr || proto->ReturnTypes[0]->GetRegType() != REGT_INT))
	{
		Printf(TEXTCOLOR_YELLOW "%s: %s.%s does not return an int-class value\n", AuxSubsystem,
			AuxIntroHandlerClassName, funcname);
		return false;
	}

	return true;
}

// Idempotent, and latches its failure PER CLASS: a game with no IntroHandler is not going to grow one,
// and this is called from a per-frame path as well as from startup. The class is re-read on every call so
// that a script recompile - which builds a new IntroHandler and frees the old one - is noticed instead of
// being answered from three dangling pointers. See the declarations above.
static bool AuxResolveBridge()
{
	// noCreate is implicit in FindClass, and a non-Selaco iwad has no IntroHandler and therefore no
	// first-run dialogs for the picker to stand in for.
	FName clsName(AuxIntroHandlerClassName, true);
	PClass *cls = clsName != NAME_None ? PClass::FindClass(clsName) : nullptr;

	if (AuxBridgeAttempted && cls == AuxBridgeClass)
		return AuxBridgeResolved;

	// Recorded BEFORE the resolve, so a resolve that fails still counts as the one attempt for this class
	// and the yellow line is said once rather than once per frame.
	AuxBridgeAttempted = true;
	AuxBridgeResolved = false;
	AuxBridgeClass = cls;
	AuxFuncStartupCheck = nullptr;
	AuxFuncClearTOS = nullptr;
	AuxFuncClearVisibility = nullptr;

	if (cls == nullptr)
	{
		Printf(TEXTCOLOR_YELLOW "%s: no %s class - not Selaco, or renamed\n",
			AuxSubsystem, AuxIntroHandlerClassName);
		return false;
	}

	int regs = 0;
	VMFunction *startupCheck = ResolveStaticMethod(cls, "startupCheck", AuxSubsystem, &regs);
	VMFunction *clearTOS = ResolveStaticMethod(cls, "clearNeedsTOS", AuxSubsystem, &regs);
	VMFunction *clearVisibility = ResolveStaticMethod(cls, "clearNeedsVisibility", AuxSubsystem, &regs);

	// All three or none. The picker must not arm on a build where it can ask whether a first run is
	// pending but cannot then stop the dialogs, because that is both dialogs on top of the picker.
	if (startupCheck == nullptr || clearTOS == nullptr || clearVisibility == nullptr)
		return false;

	if (!AuxReturnMatches(startupCheck, "startupCheck", true)
		|| !AuxReturnMatches(clearTOS, "clearNeedsTOS", false)
		|| !AuxReturnMatches(clearVisibility, "clearNeedsVisibility", false))
	{
		return false;
	}

	AuxFuncStartupCheck = startupCheck;
	AuxFuncClearTOS = clearTOS;
	AuxFuncClearVisibility = clearVisibility;
	AuxBridgeResolved = true;
	return true;
}

// Is Selaco's own first run still pending? False in `out` means this install has already been through
// their dialogs, which is what tells an existing install apart from a fresh one. The return value is
// whether the question could be asked at all - a failure must never be read as "no".
static bool AuxSelacoFirstRunPending(bool &out)
{
	if (!AuxResolveBridge())
		return false;

	try
	{
		int result = 0;
		VMReturn ret(&result);
		// Proved to be a zero-argument static, so nothing is passed. The array exists only so that a
		// null params pointer never reaches VMCall.
		VMValue params[1] = {};
		VMCall(AuxFuncStartupCheck, params, 0, &ret, 1);
		out = result != 0;
	}
	catch (const std::exception &e)
	{
		// A script abort inside their function must not take the startup down with it.
		Printf(TEXTCOLOR_YELLOW "%s: %s.startupCheck aborted: %s\n", AuxSubsystem,
			AuxIntroHandlerClassName, e.what());
		return false;
	}

	return true;
}

// Suppress both of Selaco's first-run dialogs, by calling the two functions their own menus call when
// the player finishes with them.
//
// THE ONLY PLACE THIS HAPPENS. Two callers: arming the picker, which has already proved every symbol,
// every profile and the descriptor, and recording a selection, which has already applied the profile.
static bool AuxSuppressFirstRun()
{
	if (!AuxResolveBridge())
		return false;

	try
	{
		VMValue params[1] = {};
		VMCall(AuxFuncClearTOS, params, 0, nullptr, 0);
		VMCall(AuxFuncClearVisibility, params, 0, nullptr, 0);
	}
	catch (const std::exception &e)
	{
		Printf(TEXTCOLOR_YELLOW "%s: clearing Selaco's first-run flags aborted: %s\n",
			AuxSubsystem, e.what());
		return false;
	}

	return true;
}

// Hand the dialogs back, which is the one thing that has to be done by cvar name.
//
// Selaco has no inverse of clearNeedsTOS - their IDs only ever go up (intro_handler.zs:55-56, "increase
// this every time we want to force a TOS"), so there is nothing to call. Zero is used rather than a
// remembered previous value because zero is the only value that is below every ID they will ever pick,
// which is exactly what "pending again" has to mean.
//
// Reached from every path that gives up while a picker is owed, so that giving up restores the player
// to the dialogs they would have had. Never reached when a device is recorded or declined, because then
// the state is not ours to reset.
static bool AuxRestoreFirstRun()
{
	static const char *const names[] = { "g_tos", "g_promptSpectacle" };

	bool ok = true;
	for (const char *name : names)
	{
		FBaseCVar *cvar = FindCVar(name, nullptr);
		if (cvar == nullptr)
		{
			Printf(TEXTCOLOR_YELLOW "%s: no cvar '%s' - Selaco's first-run dialogs cannot be restored\n",
				AuxSubsystem, name);
			ok = false;
			continue;
		}

		UCVarValue val;
		val.Int = 0;
		cvar->SetGenericRep(val, CVAR_Int);
	}

	return ok;
}

bool AuxDeviceChoiceClear()
{
	// Order chosen so a failure leaves the choice alone rather than cleared with the dialogs still
	// suppressed, which is the one combination that must not be reachable.
	const bool ok = AuxRestoreFirstRun();
	if (ok)
		AuxDeviceChoiceSet("");
	return ok;
}

// ----------------------------------------------------------------------------------------------
// THE MENU, built here because the list of devices is runtime data.
//
// A MENUDEF lump of ours could not do this. Every MENUDEF parses in load order (menudef.cpp:1529) and
// our gzdoom.pk3 loads before the player's Selaco.ipk3, so a lump cannot know what is in the profiles
// directory and cannot be written against a list that a contributor is meant to extend by dropping in a
// file. The same reasoning, and the same fail-soft discipline, as the two Selaco menus i_auxpanel.cpp
// rewrites: every failure is one yellow line and a return.
// ----------------------------------------------------------------------------------------------

static const char *const AuxPickerMenuName = "AuxDevicePickerMenu";
static const char *const AuxPickerTitle = "SELECT YOUR DEVICE";

// The command each entry runs. Held as one string because it is also the item's mAction, which
// OptionMenuItemCommand.Activate looks itself up by (optionmenuitems.zs:198) - so it has to be unique
// per entry, and appending the profile stem makes it so for free.
static const char *const AuxPickerCommand = "aux_setdevice";

// The stock command item, not Selaco's TooltipCommand: their builder dispatches on the stock base
// (options_menu_base.zs), so a stock item is built as an ordinary entry and simply has no tooltip.
static const char *const AuxCommandItemClassName = "OptionMenuItemCommand";

// Where the picker's look is borrowed from. SteamDeckMenu is a Selaco option menu with Class
// "HoveringTooltipMenu" (MENUDEF.zsc:64), so taking its mClass makes the picker read as one of their
// pages rather than a stock GZDoom one - and it is the page this port already extends, so the two
// cannot end up looking like different features. Absent on a non-Selaco game, which is fine: the
// descriptor falls back to Reset()'s stock values and the picker still works.
static const char *const AuxPickerStyleDonor = "SteamDeckMenu";

static bool AuxPickerBuilt = false;
static int AuxPickerEntries = 0;

static DOptionMenuDescriptor *AuxOptionMenuDescriptorNamed(const char *name)
{
	DMenuDescriptor **descp = MenuDescriptors.CheckKey(name);
	if (descp == nullptr || *descp == nullptr || !(*descp)->IsKindOf(RUNTIME_CLASS(DOptionMenuDescriptor)))
		return nullptr;
	return static_cast<DOptionMenuDescriptor *>(*descp);
}

// Build and register the picker descriptor. Idempotent, so the startup arm and either console command
// can all call it without caring which got there first.
//
// AN EMPTY PROFILES DIRECTORY IS A FAILURE, not an empty menu: a picker the player cannot answer would
// leave them worse off than Selaco's own dialogs, so nothing is registered and the caller falls through
// to those. That check is why the item loop below has no post-condition of its own - one entry is built
// per profile and there is at least one profile.
static bool AuxBuildPicker()
{
	// The descriptor is re-checked rather than trusted to the bool, because a `restart` runs DeinitMenus,
	// which clears MenuDescriptors wholesale (menudef.cpp:158), and then D_InitGame builds the menus again.
	// A bare latch would report success while AuxDevicePickerMenu no longer exists, and M_SetMenu would
	// then open nothing - see the CurrentMenu check in AuxOpenPicker.
	if (AuxPickerBuilt && AuxOptionMenuDescriptorNamed(AuxPickerMenuName) != nullptr)
		return true;
	AuxPickerBuilt = false;

	TArray<AuxProfileEntry> profiles;
	if (!AuxProfileScan(profiles) || profiles.Size() == 0)
	{
		// AuxProfileScan has already said if the directory itself could not be read.
		Printf(TEXTCOLOR_YELLOW "%s: no device profiles found, there is nothing to pick from\n",
			AuxSubsystem);
		return false;
	}

	// NOT searching parents, and that is load-bearing rather than tidiness: OptionMenuItemCommand
	// declares its own four-argument Init, while the OptionMenuItemSubmenu.Init it would fall back to
	// takes a String, a Name, an int and a bool. Calling the wrong one with our arguments is the
	// read-past-the-array crash i_auxvmreflect.h opens with.
	PClass *cls = PClass::FindClass(AuxCommandItemClassName);
	PFunction *init = (cls != nullptr) ? dyn_cast<PFunction>(cls->FindSymbol("Init", false)) : nullptr;
	if (init == nullptr)
	{
		Printf(TEXTCOLOR_YELLOW "%s: no %s.Init, the device picker cannot be built\n",
			AuxSubsystem, AuxCommandItemClassName);
		return false;
	}

	// CreateNew calls I_Error on either of these rather than returning null (dobjtype.cpp:433-437), so an
	// item class that cannot be instantiated has to be caught here or it costs the player a startup rather
	// than a picker. Same check, and the same reason, as i_auxstandbycodex.cpp makes before its own CreateNew.
	if (cls->bAbstract || cls->ConstructNative == nullptr
		|| !cls->IsDescendantOf(RUNTIME_CLASS(DMenuItemBase)))
	{
		Printf(TEXTCOLOR_YELLOW "%s: %s is not an instantiable menu item, the device picker cannot be "
			"built\n", AuxSubsystem, AuxCommandItemClassName);
		return false;
	}

	DOptionMenuDescriptor *desc = Create<DOptionMenuDescriptor>();
	desc->Reset();                      // mPosition, mScrollTop, mIndent, mDontDim and the stock font
	desc->mMenuName = FName(AuxPickerMenuName);
	desc->mTitle = AuxPickerTitle;      // a literal, which StringTable.Localize passes through unchanged
	desc->mSelectedItem = -1;
	desc->mDrawTop = 0;
	desc->mScrollPos = 0;
	desc->mClass = nullptr;
	desc->mDontBlur = false;
	desc->mAnimated = false;
	desc->mAnimatedTransition = false;
	desc->mProtected = false;

	DOptionMenuDescriptor *donor = AuxOptionMenuDescriptorNamed(AuxPickerStyleDonor);
	if (donor != nullptr)
	{
		desc->mClass = donor->mClass;
		desc->mFont = donor->mFont;
		desc->mPosition = donor->mPosition;
		desc->mScrollTop = donor->mScrollTop;
	}

	// REGISTERED BEFORE ITS ITEMS EXIST, so the descriptor is GC-reachable (M_MarkMenus walks
	// MenuDescriptors, menu.cpp:184) before the first CreateNew below can run a collection. The
	// WriteBarrier is the same one menudef.cpp:740 uses for exactly this insert.
	const FName menuName = desc->mMenuName;
	MenuDescriptors[menuName] = desc;
	GC::WriteBarrier(desc);

	// Order is AuxProfileScan's, which is the one place it is decided - see the comment at the end of
	// that function for why, and where to change it.
	for (const AuxProfileEntry &profile : profiles)
	{
		DMenuItemBase *item = (DMenuItemBase *)cls->CreateNew();

		// Pushed BEFORE Init rather than after, unlike i_auxpanel.cpp's items: Init runs script code
		// that can allocate, and until the item is in mItems it is held only by a local. Init writes
		// fields on an object it is handed, so it does not care that the array already refers to it.
		desc->mItems.Push(item);

		FString label = profile.display;
		FString command;
		command.Format("%s %s", AuxPickerCommand, profile.stem.GetChars());

		// VMCallWithDefaults, not VMCall, so a build whose Init grew an argument keeps whatever value
		// the declaration gives it - exactly how the MENUDEF parser builds this same item.
		//
		// The two bools are Init's `centered` and `closeonselect`. closeonselect true is what makes the
		// menu shut itself the way a MENUDEF item would (optionmenuitems.zs:202), rather than this file
		// reaching into the menu stack from inside a console command the menu is still running.
		TArray<VMValue> params;
		params.Push(item);
		params.Push(&label);
		params.Push(FName(command.GetChars()).GetIndex());
		params.Push(false);
		params.Push(true);
		VMCallWithDefaults(init->Variants[0].Implementation, params, nullptr, 0);
	}

	AuxPickerEntries = (int)desc->mItems.Size();
	AuxPickerBuilt = true;
	return true;
}

// ----------------------------------------------------------------------------------------------
// SHOWING IT.
// ----------------------------------------------------------------------------------------------

// Armed means "keep trying to show the picker, and keep putting it back if it is closed". Forced means
// the same, but ignoring a device that is already recorded, which is what the console command needs so
// the picker can be tested on a device that has already answered. Both end the same way: aux_setdevice.
static bool AuxPickerArmed = false;
static bool AuxPickerForced = false;

static bool AuxOpenPicker()
{
	if (!AuxBuildPicker())
		return false;

	// M_StartControlPanel returns without doing anything when a menu is already up (menu.cpp:454), so
	// this is checked rather than relied upon: opening over IntroHandler's blocker is exactly what must
	// not happen.
	if (CurrentMenu != nullptr)
		return false;

	M_StartControlPanel(false);
	M_SetMenu(FName(AuxPickerMenuName), -1);

	// M_StartControlPanel has already set menuactive = MENU_On and hidden the console (menu.cpp:450-465),
	// so a M_SetMenu that produces no menu would leave the engine believing a menu is open with
	// CurrentMenu null - a paused title screen with nothing on it and no input that can close it. Undone
	// here rather than left to the caller, because this is the only place the two calls are paired.
	if (CurrentMenu == nullptr)
	{
		M_ClearMenus();
		return false;
	}

	return true;
}

// Once per frame, from D_Display. Costs one bool test on every device that is not being asked, which is
// every device after the first launch.
void I_AuxDevicePickerFrame()
{
	// The reset button's own poll, which undoes what this file records. FIRST, because it has to run on
	// every frame of every launch while the picker's own work stops at the test below on all but the first.
	AuxDeviceResetFrame();

	if (!AuxPickerArmed)
		return;

	// The choice being recorded is what ends the obligation, including when it was recorded from the
	// console rather than from the menu.
	if (!AuxPickerForced && AuxDeviceChoiceIsSet())
	{
		AuxPickerArmed = false;
		return;
	}

	// The "press any key" title screen and nothing else: TITLEMAP up, no menu of anyone's, console up.
	// This is the state IntroHandler leaves behind once its intro blocker closes itself, and waiting for
	// it is what keeps the picker from fighting either that blocker or the player's console.
	if (gamestate != GS_TITLELEVEL || CurrentMenu != nullptr || menuactive != MENU_Off || ConsoleState != c_up)
		return;

	// REOPENED FOR AS LONG AS THE CHOICE IS OUTSTANDING, which is what makes the picker undismissable.
	// A gamepad B press reaches the stock option menu's MKEY_Back and closes it, and there is no way to
	// refuse that from here: blocking it needs a MenuEvent override, so a menu class, so ZScript we
	// cannot compile before Selaco's. Self-parenting to survive Close() is worse - it falls through to
	// Destroy() (menu.cpp) while CurrentMenu still points at the menu. So the close is allowed and
	// simply undone: this poll runs in D_Display before anything is drawn, and M_StartControlPanel is
	// passed makesound=false, so the reopen costs one silent frame and the closed state is never seen.
	// The player hears the back sound and the picker does not move, which is the intent.
	//
	// aux_setdevice is what ends this, and it is the only thing that does - it disarms on a recorded
	// choice. Force-stopping the app is the other way out and deliberately leaves the flag untouched.
	if (AuxOpenPicker())
		return;

	// Everything the picker needs was proved before it was armed, so a failure here is something new and
	// retrying it every frame would only spam. Give up once, and give the player back the dialogs that
	// were suppressed on this picker's behalf.
	Printf(TEXTCOLOR_YELLOW "%s: the picker could not be opened, falling back to Selaco's own dialogs\n",
		AuxSubsystem);
	AuxPickerArmed = false;
	AuxPickerForced = false;
	if (AuxDeviceChoiceIsOwed())
		AuxDeviceChoiceClear();
}

// ----------------------------------------------------------------------------------------------
// DECIDING, once, from D_DoomMain after M_Init.
//
// After M_Init because that is what parses every MENUDEF lump, and the picker borrows its look from one
// of Selaco's descriptors and its item class from a compiled ZScript class. Before the first world tick
// because that is when TOSEventHandler would open their dialogs.
// ----------------------------------------------------------------------------------------------

void I_AuxDevicePickerInit()
{
	// The Handhelds page's reset button, which is how the player gets back to the picker. FIRST, and
	// outside every early return below, because it has to be installed on the launches where a device is
	// already recorded - which is the only launch on which anyone would want it.
	AuxDeviceResetInitMenu();

	// A DEVICE IS ALREADY RECORDED, or this install declined one: return having written nothing, touched
	// nothing of Selaco's and printed nothing but a line naming the device. This is the whole of what an
	// existing install sees.
	if (AuxDeviceChoiceIsSet())
	{
		if (strcmp(AuxDeviceChoice(), AuxDeviceDeclined) == 0)
			Printf("%s: no device profile on this install, picker not shown\n", AuxSubsystem);
		else
			Printf("%s: device=\"%s\", picker not shown\n", AuxSubsystem, AuxDeviceChoice());
		return;
	}

	const bool owed = AuxDeviceChoiceIsOwed();

	// SELACO IS ASKED BEFORE ANY WORK IS DONE, so an install that is simply not on its first launch
	// costs one VM call and nothing else - no profile scan, no descriptor, and nothing written anywhere
	// but our own cvar. Skipped when a picker is already owed, because then the answer is known: a
	// previous launch is what cleared their flags.
	if (!owed)
	{
		bool pending = false;
		if (!AuxSelacoFirstRunPending(pending))
		{
			// Could not ask, so nothing is assumed: their dialogs run, nothing is recorded, and the next
			// launch is free to try again.
			Printf(TEXTCOLOR_YELLOW "%s: cannot tell whether this is a first launch, leaving Selaco's dialogs alone\n",
				AuxSubsystem);
			return;
		}

		if (!pending)
		{
			// Already been through their dialogs, so this is not a first launch and no picker is owed.
			// Recorded so every later launch short-circuits at the top of this function instead of asking
			// again, and so that nothing is ever suppressed on this install's behalf.
			AuxDeviceChoiceSet(AuxDeviceDeclined);
			Printf("%s: Selaco's first run is already done, no picker and no device recorded\n",
				AuxSubsystem);
			return;
		}
	}

	// EVERYTHING IS PROVED BEFORE ANYTHING OF SELACO'S IS TOUCHED. Both halves, because arming without
	// being able to suppress puts their dialogs on top of the picker, and suppressing without being able
	// to show the picker leaves the player with neither.
	if (!AuxResolveBridge() || !AuxBuildPicker())
	{
		Printf(TEXTCOLOR_YELLOW "%s: no picker this launch, Selaco's own first-run dialogs will run\n",
			AuxSubsystem);

		// A previous launch suppressed them for a picker that this launch cannot show, so the
		// suppression is undone rather than left to hide both dialogs. This is the case the reserved
		// "owed" value exists for.
		if (owed)
			AuxDeviceChoiceClear();
		return;
	}

	// Written BEFORE the suppression below, so that a kill between the two leaves the obligation
	// recorded and the dialogs intact rather than the other way round.
	if (!owed)
		AuxDeviceChoiceSet(AuxDeviceOwed);

	if (!AuxSuppressFirstRun())
	{
		// Cannot happen with a resolved bridge short of a script abort, and handled anyway: the picker
		// is still armed, and the player simply answers Selaco's dialogs as well.
		Printf(TEXTCOLOR_YELLOW "%s: Selaco's first-run dialogs could not be suppressed, they will appear too\n",
			AuxSubsystem);
	}

	AuxPickerArmed = true;
	Printf("%s: build=%s (applier %s)\n", AuxSubsystem, AuxDevicePickerBuild, AuxProfileBuildId());
	Printf("%s: first launch - offering %d device%s%s\n", AuxSubsystem, AuxPickerEntries,
		AuxPickerEntries == 1 ? "" : "s", owed ? ", still owed from a previous launch" : "");
}

// ----------------------------------------------------------------------------------------------
// THE TWO CONSOLE COMMANDS.
// ----------------------------------------------------------------------------------------------

// What every menu entry runs, and the only place a device is recorded.
CCMD(aux_setdevice)
{
	Printf("%s: build=%s\n", AuxSubsystem, AuxDevicePickerBuild);

	if (argv.argc() < 2)
	{
		Printf("Usage: aux_setdevice <profile>   (see aux_listprofiles)\n");
		return;
	}

	const char *stem = argv[1];

	// THE PROFILE FIRST, ALWAYS. Nothing below this point runs if it did not apply, so there is no path
	// on which a device is recorded, or Selaco's dialogs suppressed, without the work their dialogs
	// would have done having happened first.
	AuxProfileResult result;
	if (!AuxProfileApply(stem, result))
	{
		Printf(TEXTCOLOR_YELLOW "%s: '%s' was not applied - the device choice is unchanged\n",
			AuxSubsystem, stem);
		return;
	}

	// The one partial success that must not be recorded. SetSteamdeckPresets is the reason suppressing
	// their dialog is safe at all (CLAUDE.md's three bugs), so a profile that asked for it and did not
	// get it is not a device this port can claim to have configured.
	if (result.steamdeckWanted && !result.steamdeckApplied)
	{
		Printf(TEXTCOLOR_YELLOW "%s: '%s' could not call SetSteamdeckPresets - not recorded, Selaco's dialogs kept\n",
			AuxSubsystem, stem);
		if (AuxDeviceChoiceIsOwed())
			AuxDeviceChoiceClear();
		AuxPickerArmed = false;
		AuxPickerForced = false;
		return;
	}

	AuxDeviceChoiceSet(stem);
	AuxPickerArmed = false;
	AuxPickerForced = false;

	const bool suppressed = AuxSuppressFirstRun();
	if (!suppressed)
	{
		// The profile is applied and recorded, so this is only a cosmetic duplicate: the player answers
		// Selaco's dialogs on top of a device that is already configured.
		Printf(TEXTCOLOR_YELLOW "%s: Selaco's own first-run dialogs could not be cleared, they will still appear\n",
			AuxSubsystem);
	}

	// PERSISTED HERE, NOT LEFT TO THE QUIT. aux_device is CVAR_ARCHIVE|CVAR_GLOBALCONFIG and Selaco's g_tos
	// and g_promptSpectacle are CVARINFO cvars, so all three reach the ini only when something archives
	// them - and on Android the normal way out of the game is the task switcher, which kills the process
	// without ever running D_DoomMain's shutdown. Without this the recorded device is lost on every such
	// launch and the first-run picker comes back; this is the same failure AuxDoReset
	// (i_auxdevicereset.cpp) already calls M_SaveDefaults for, measured on device.
	//
	// AFTER the suppression above rather than before it, so the recorded choice and the cleared first-run
	// flags land in the SAME write. Saving between the two would persist a configured device while leaving
	// g_tos at 0, i.e. Selaco's own dialogs on top of an install the picker has already answered for.
	// Failure is only worth a line: all three facts are in memory either way and the shutdown retries.
	if (!M_SaveDefaults(nullptr))
	{
		Printf(TEXTCOLOR_YELLOW "%s: writing the config failed, the device choice may not survive a kill\n",
			AuxSubsystem);
	}

	Printf("%s: device=\"%s\" (%s) - %d cvars written, %d problems, Selaco's first-run dialogs %s\n",
		AuxSubsystem, result.display.GetChars(), stem, result.cvarsWritten, result.problems,
		suppressed ? "cleared" : "left alone");
}

// Force the picker without wiping the recorded device, which is how it is tested on a device that has
// already been through it. Arms rather than opens, for the reason the poll exists: opened here it would
// be replaced by IntroHandler's intro blocker if the title screen is still in its intro, and this is
// meant to be runnable from the shipped autoexec.
CCMD(aux_devicepicker)
{
	Printf("%s: build=%s (applier %s)\n", AuxSubsystem, AuxDevicePickerBuild, AuxProfileBuildId());
	Printf("%s: device=\"%s\"\n", AuxSubsystem, AuxDeviceChoice());

	if (!AuxBuildPicker())
		return;

	AuxPickerForced = true;
	AuxPickerArmed = true;
	Printf("%s: forced - %d device%s, shown at the next idle title screen\n", AuxSubsystem,
		AuxPickerEntries, AuxPickerEntries == 1 ? "" : "s");
}

// Forget every resolve this file latched, for the restart teardown. See I_AuxForgetScriptState in
// i_auxvmreflect.cpp for why it exists and when it runs. Nothing here does VM work; every line is a store.
void I_AuxDevicePickerForgetScriptState()
{
	// The bridge is keyed on the IntroHandler class by pointer, which the restart defeats by freeing every
	// PClass and then re-parsing: a recycled address makes the compare match and skips the resolve, leaving
	// the three VMCalls below aimed at freed VMFunctions.
	AuxBridgeAttempted = false;
	AuxBridgeResolved = false;
	AuxBridgeClass = nullptr;
	AuxFuncStartupCheck = nullptr;
	AuxFuncClearTOS = nullptr;
	AuxFuncClearVisibility = nullptr;

	// AuxPickerBuilt already re-checks the live descriptor, so this is belt and braces rather than the fix;
	// it is cleared anyway so the next build's log line counts the entries it really added.
	AuxPickerBuilt = false;
	AuxPickerEntries = 0;

	// THE ONE ENTRY HERE THAT IS USER-VISIBLE RATHER THAN A DANGLING POINTER. Both are write-true-only from
	// I_AuxDevicePickerInit, and every one of its early returns leaves them alone - so a restart carried a
	// previous launch's armed state into a launch that decided not to offer a picker. With AuxPickerForced
	// set that is undismissable by design, which turned `aux_devicepicker` plus a restart into a picker
	// reopening on the title screen of an install that already had a device. The next Init re-arms if a
	// picker is genuinely owed.
	AuxPickerArmed = false;
	AuxPickerForced = false;
}
