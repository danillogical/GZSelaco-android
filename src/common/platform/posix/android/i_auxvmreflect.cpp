/*
** i_auxvmreflect.cpp
** Resolve and prove the ZScript symbols the second-screen panel calls and writes.
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
** The safety argument for all of this is in i_auxvmreflect.h. What is here is the mechanism.
*/

// dobjtype.h and dobjgc.h are not self-contained (they lean on FName, FString and the DObject
// declaration being in scope already), so dobject.h leads - it is the header that pulls that chain
// in, and every engine translation unit that touches PClass reaches it the same way.
#include <exception>     // std::exception, the base TightenTabStrip catches a VM abort through
#include "dobject.h"
#include "cmdlib.h"      // countof, for the tab field table
#include "dobjtype.h"
#include "menu.h"        // DMenu, which Arg_Menu is checked against
#include "printf.h"
#include "symbols.h"
#include "types.h"
#include "v_2ddrawer.h"  // FCanvas, which Arg_Canvas and Field_CanvasPtr are checked against
#include "vm.h"
#include "vmintern.h"    // VMScriptFunction::NumArgs, the count VMFillParams actually reads
#include "zstring.h"

#include "i_auxvmreflect.h"

// ZScript's `struct CVar native`, which is what UIMenu.ui_scaling (menu.zs:28) is a pointer to. Named
// here because Field_CVarPtr compares against it by name; see the null store in BuildStandbyView.
static const char *const CVarStructName = "CVar";

namespace AuxView
{

// Returns false if the type is not the kind we mean; on success reports how many VM registers the
// argument occupies, mirroring VMFunction::CreateRegUse (vmframe.cpp:88-96).
//
// argClass is read by Arg_ObjectOf and by nothing else: it is the class every instance we intend to pass
// through that parameter has already been proved to be a kind of.
static bool ArgMatches(PType *type, EArgKind kind, int &regs, PClass *argClass)
{
	regs = 0;
	if (type == nullptr)
		return false;

	switch (kind)
	{
	case Arg_Vector2:
		// A Vector2 is one declared argument but TWO registers (types.cpp:365), which is the whole
		// reason the register count cannot be inferred from the argument count.
		regs = 2;
		return type == TypeVector2;

	case Arg_Float:
		regs = 1;
		return type == TypeFloat64;

	case Arg_Bool:
		regs = 1;
		return type == TypeBool;

	case Arg_Int:
		regs = 1;
		return type == TypeSInt32;

	case Arg_String:
		// One register. A ZScript String argument is passed as a VMValue holding an FString* which the
		// callee copies out of, which is what lets a caller hand over the address of a local FString.
		// Equality against TypeString because there is exactly one string type.
		regs = 1;
		return type == TypeString;

	case Arg_Canvas:
		// ZScript's Canvas is the native FCanvas: DECLARE_CLASS registers it and PClass strips the
		// leading letter (dobjtype.cpp:351), so RUNTIME_CLASS(FCanvas) IS the Canvas class script
		// sees. Descendant rather than equal so a parameter declared as a base of Canvas still
		// matches - the cast we perform is still provably valid.
		regs = 1;
		return type->isObjectPointer()
			&& RUNTIME_CLASS(FCanvas)->IsDescendantOf(static_cast<PObjectPointer *>(type)->PointedClass());

	case Arg_Menu:
		// PDAMenu3.init and PromptMenu.initNew both take a Menu parent, and ZScript's Menu is the native
		// DMenu. Descendant so a parameter declared as a base of Menu still matches; the value passed is
		// either null or a DMenu* the engine already holds, so this proves the register layout and the
		// validity of the cast at once.
		regs = 1;
		return type->isObjectPointer()
			&& RUNTIME_CLASS(DMenu)->IsDescendantOf(static_cast<PObjectPointer *>(type)->PointedClass());

	case Arg_ObjectOf:
		// A script object pointer we pass a REAL instance through, so the direction of the test is the
		// same as Arg_Canvas's: what has to hold is that the value is assignable to the declared
		// parameter, i.e. that argClass derives from the declared pointee. Every call site pairs this
		// with an IsKindOf(argClass) on the instance itself, which is what makes the pair a proof.
		regs = 1;
		return argClass != nullptr && type->isObjectPointer()
			&& static_cast<PObjectPointer *>(type)->PointedClass() != nullptr
			&& argClass->IsDescendantOf(static_cast<PObjectPointer *>(type)->PointedClass());
	}
	return false;
}


VMFunction *ResolveMethod(PClass *cls, const char *funcname, const char *subsystem,
	const EArgKind *argkinds, unsigned nargs, int *outRegs, PClass *argClass, const char *disabledNote)
{
	// noCreate: never add a name to the table just to look one up.
	FName name(funcname, true);
	if (name == NAME_None)
	{
		Printf(TEXTCOLOR_YELLOW "%s: %s.%s does not exist, %s\n",
			subsystem, cls->TypeName.GetChars(), funcname, disabledNote);
		return nullptr;
	}

	PFunction *sym = dyn_cast<PFunction>(cls->FindSymbol(name, true));
	if (sym == nullptr || sym->Variants.Size() != 1)
	{
		Printf(TEXTCOLOR_YELLOW "%s: %s.%s is not a single-variant function, %s\n",
			subsystem, cls->TypeName.GetChars(), funcname, disabledNote);
		return nullptr;
	}

	const PFunction::Variant &variant = sym->Variants[0];

	// A ZScript "static" class method is identified by the ABSENCE of an implied self parameter, not
	// by a positive flag: zcc_compile.cpp:2507 clears VARF_Method and sets VARF_Final for it, and
	// never sets VARF_Static, which is only ever applied to fields (:1291, :1551, :1624). Testing
	// VARF_Static here rejected ManualHandler.Instance on device even though it is declared static.
	// Nothing either mode calls is a static, so the absence of a self is simply rejected here;
	// ResolveStaticMethod below is what the profile applier uses for one that genuinely is.
	const bool isMethod = !!(variant.Flags & VARF_Method);
	if (!isMethod || (variant.Flags & VARF_Action))
	{
		// An action takes three implicit arguments instead of one, so it is rejected either way.
		Printf(TEXTCOLOR_YELLOW "%s: %s.%s is not a plain instance method, %s\n",
			subsystem, cls->TypeName.GetChars(), funcname, disabledNote);
		return nullptr;
	}

	VMFunction *func = variant.Implementation;

	// Dispatch a virtual through the instance's own vtable, exactly as IFVIRTUALPTR does
	// (vm.h:818-826), so a subclass override is what actually runs. FindSymbol already returns the
	// most-derived declaration for this class, so these agree - but the vtable is the authority.
	if (func != nullptr && (func->VarFlags & VARF_Virtual) && func->VirtualIndex != ~0u
		&& cls->Virtuals.Size() > func->VirtualIndex)
	{
		func = cls->Virtuals[func->VirtualIndex];
	}

	if (func == nullptr || (func->VarFlags & (VARF_Native | VARF_Abstract)))
	{
		// Native would mean a different calling convention and no NumArgs to check against;
		// abstract aborts the VM on call (vmframe.cpp:317-320).
		Printf(TEXTCOLOR_YELLOW "%s: %s.%s is missing, native or abstract, %s\n",
			subsystem, cls->TypeName.GetChars(), funcname, disabledNote);
		return nullptr;
	}

	// Register 0 is self, so the whole prototype is shifted by one against the declared argument list.
	const unsigned selfArgs = 1u;

	PPrototype *proto = func->Proto;
	if (proto == nullptr || proto->ArgumentTypes.Size() != nargs + selfArgs)
	{
		// Note that an override which omits trailing optional arguments still reports the FULL list
		// here: FindVirtualIndex extends the prototype to the base's argument list (dobjtype.cpp,
		// "Extend the prototype"), which is why PDAAppWindow.layout declares two parameters and still
		// takes three.
		//
		// The count is printed unsigned, so it is clamped rather than subtracted: a zero-argument
		// prototype found where a self was expected would otherwise underflow to four billion.
		const unsigned declared = proto != nullptr && proto->ArgumentTypes.Size() >= selfArgs
			? proto->ArgumentTypes.Size() - selfArgs : 0u;
		Printf(TEXTCOLOR_YELLOW "%s: %s.%s takes %u arguments, not %u, %s\n",
			subsystem, cls->TypeName.GetChars(), funcname, declared, nargs, disabledNote);
		return nullptr;
	}

	// The self pointer. Casting the instance to a self type it does not satisfy is the one mistake
	// here that corrupts memory rather than misdraws. Read off the function we are ACTUALLY calling
	// rather than off the symbol's variant, because a vtable entry may belong to a different class;
	// ArgumentTypes[0] is where AddVariant reads self from too (symbols.cpp:98-102).
	{
		PPointer *selfPtr = proto->ArgumentTypes[0] != nullptr ? proto->ArgumentTypes[0]->toPointer() : nullptr;
		PClassType *selfClass = selfPtr != nullptr ? PType::toClass(selfPtr->PointedType) : nullptr;
		if (selfClass == nullptr || selfClass->Descriptor == nullptr || !cls->IsDescendantOf(selfClass->Descriptor))
		{
			Printf(TEXTCOLOR_YELLOW "%s: %s is not a valid self for %s, %s\n",
				subsystem, cls->TypeName.GetChars(), funcname, disabledNote);
			return nullptr;
		}
	}

	int regs = (int)selfArgs;
	for (unsigned i = 0; i < nargs; i++)
	{
		int argregs = 0;
		if (!ArgMatches(proto->ArgumentTypes[i + selfArgs], argkinds[i], argregs, argClass))
		{
			Printf(TEXTCOLOR_YELLOW "%s: %s.%s argument %u has an unexpected type, %s\n",
				subsystem, cls->TypeName.GetChars(), funcname, i + 1, disabledNote);
			return nullptr;
		}

		// An out parameter is passed as a POINTER to storage rather than by value (vmframe.cpp:81-84),
		// so it would change the register layout under an argument type that still looks correct.
		if (func->ArgFlags.Size() > i + selfArgs && (func->ArgFlags[i + selfArgs] & VARF_Out))
		{
			Printf(TEXTCOLOR_YELLOW "%s: %s.%s argument %u is an out parameter, %s\n",
				subsystem, cls->TypeName.GetChars(), funcname, i + 1, disabledNote);
			return nullptr;
		}

		regs += argregs;
	}

	// The decisive check. NumArgs is what VMFillParams loops over (vmexec.cpp:210) and what its own
	// assert compares against (:204), so agreeing with it is exactly the guarantee that the callee
	// reads no further than the array we pass.
	const VMScriptFunction *sfunc = static_cast<const VMScriptFunction *>(func);
	if (sfunc->NumArgs != regs)
	{
		Printf(TEXTCOLOR_YELLOW "%s: %s.%s wants %d parameters, we would pass %d, %s\n",
			subsystem, cls->TypeName.GetChars(), funcname, (int)sfunc->NumArgs, regs, disabledNote);
		return nullptr;
	}

	*outRegs = regs;
	return func;
}

VMFunction *ResolveStaticMethod(PClass *cls, const char *funcname, const char *subsystem, int *outRegs)
{
	// noCreate: never add a name to the table just to look one up.
	FName name(funcname, true);
	if (name == NAME_None)
	{
		Printf(TEXTCOLOR_YELLOW "%s: %s.%s does not exist\n", subsystem, cls->TypeName.GetChars(), funcname);
		return nullptr;
	}

	PFunction *sym = dyn_cast<PFunction>(cls->FindSymbol(name, true));
	if (sym == nullptr || sym->Variants.Size() != 1)
	{
		Printf(TEXTCOLOR_YELLOW "%s: %s.%s is not a single-variant function\n",
			subsystem, cls->TypeName.GetChars(), funcname);
		return nullptr;
	}

	const PFunction::Variant &variant = sym->Variants[0];

	// The mirror image of ResolveMethod's check, and it relies on the same finding: a ZScript static is
	// identified by the ABSENCE of an implied self (zcc_compile.cpp:2507 clears VARF_Method for it and
	// never sets VARF_Static, which only ever applies to fields). So a static is VARF_Method CLEAR -
	// testing VARF_Static here would reject a function that genuinely is one.
	if ((variant.Flags & VARF_Method) || (variant.Flags & VARF_Action))
	{
		Printf(TEXTCOLOR_YELLOW "%s: %s.%s is not a static (it takes a self)\n",
			subsystem, cls->TypeName.GetChars(), funcname);
		return nullptr;
	}

	// No vtable dispatch: a static cannot be virtual, so the symbol's implementation is what runs.
	VMFunction *func = variant.Implementation;
	if (func == nullptr || (func->VarFlags & (VARF_Native | VARF_Abstract)))
	{
		// Native would mean a different calling convention and no NumArgs to check against;
		// abstract aborts the VM on call (vmframe.cpp:317-320).
		Printf(TEXTCOLOR_YELLOW "%s: %s.%s is missing, native or abstract\n",
			subsystem, cls->TypeName.GetChars(), funcname);
		return nullptr;
	}

	// No self and no declared arguments, so the prototype must be empty. Checked rather than assumed:
	// this is what stops us calling a function that gained a parameter in a Selaco update.
	PPrototype *proto = func->Proto;
	if (proto == nullptr || proto->ArgumentTypes.Size() != 0)
	{
		Printf(TEXTCOLOR_YELLOW "%s: %s.%s takes %u arguments, not 0\n", subsystem,
			cls->TypeName.GetChars(), funcname,
			proto != nullptr ? proto->ArgumentTypes.Size() : 0u);
		return nullptr;
	}

	// The decisive check, exactly as in ResolveMethod: NumArgs is what VMFillParams loops over
	// (vmexec.cpp:210), so agreeing with it is the guarantee that the callee reads no further than the
	// (empty) array we pass.
	const VMScriptFunction *sfunc = static_cast<const VMScriptFunction *>(func);
	if (sfunc->NumArgs != 0)
	{
		Printf(TEXTCOLOR_YELLOW "%s: %s.%s wants %d parameters, we would pass 0\n",
			subsystem, cls->TypeName.GetChars(), funcname, (int)sfunc->NumArgs);
		return nullptr;
	}

	*outRegs = 0;
	return func;
}

PField *ResolveField(PClass *cls, const char *fieldname, EFieldKind kind, PClass *viewCls,
	const char *subsystem, const char *disabledNote)
{
	// noCreate, for the same reason as in ResolveMethod: never add a name to the table to look one up.
	FName name(fieldname, true);
	PField *field = name != NAME_None ? dyn_cast<PField>(cls->FindSymbol(name, true)) : nullptr;
	if (field == nullptr)
	{
		Printf(TEXTCOLOR_YELLOW "%s: %s.%s is not a field, %s\n",
			subsystem, cls->TypeName.GetChars(), fieldname, disabledNote);
		return nullptr;
	}

	// A static or meta field's Offset is not an offset into the instance at all, which would make the
	// store below write somewhere other than where it appears to.
	//
	// BitValue is deliberately NOT tested. PField's constructor (symbols.cpp:140-148) leaves it out of
	// its initialiser list and assigns it only inside `if (bitvalue != 0)`, so for every non-bitfield
	// it holds uninitialised garbage - testing it rejected PDAMenu3.mainView, an ordinary instance
	// field, on device. It is also unnecessary: symbols.cpp:161 I_Errors on any bit field that is not
	// internally declared, so no script-declared field is ever one.
	if (field->Flags & (VARF_Native | VARF_Static | VARF_Meta))
	{
		Printf(TEXTCOLOR_YELLOW "%s: %s.%s is not a plain instance field, %s\n",
			subsystem, cls->TypeName.GetChars(), fieldname, disabledNote);
		return nullptr;
	}

	bool ok = false;
	switch (kind)
	{
	case Field_Bool:
		ok = field->Type == TypeBool;
		break;

	case Field_Int:
		// A plain ZScript `int`, the same identity Arg_Int checks. Equality rather than a signed/unsigned
		// family test because the read is a 4-byte load interpreted as a signed int and nothing else.
		ok = field->Type == TypeSInt32;
		break;

	case Field_Float:
		ok = field->Type == TypeFloat64;
		break;

	case Field_String:
		// A ZScript String is an FString living inline in the object, constructed by the class's own
		// special-inits, so the address below may be assigned to like any other FString. Equality
		// against TypeString rather than a structural test because there is exactly one string type.
		ok = field->Type == TypeString;
		break;

	case Field_ViewPtr:
		// Declared as a UIView or a subclass of one, so the runtime check before each write below can
		// be an IsKindOf against UIView and nothing wider.
		ok = field->Type != nullptr && field->Type->isObjectPointer()
			&& static_cast<PObjectPointer *>(field->Type)->PointedClass() != nullptr
			&& static_cast<PObjectPointer *>(field->Type)->PointedClass()->IsDescendantOf(viewCls);
		break;

	case Field_CVarPtr:
		// A pointer to the NATIVE STRUCT CVar, which is not an object pointer: a variable of a native
		// struct type is always compiled to NewPointer(struct) (zcc_compile.cpp:2208). That distinction
		// is what makes the null store below safe without a write barrier - the GC only traces object
		// pointers, so this field is not a reference it can see and nulling it cannot orphan anything.
		{
			// noCreate on the comparison name too, and NAME_None rejected explicitly: without that a
			// missing "CVar" in the name table would compare equal to any unnamed type.
			const FName cvarName(CVarStructName, true);
			PPointer *ptr = field->Type != nullptr ? field->Type->toPointer() : nullptr;
			PStruct *pointee = ptr != nullptr && ptr->PointedType != nullptr && ptr->PointedType->isStruct()
				? static_cast<PStruct *>(ptr->PointedType) : nullptr;
			ok = ptr != nullptr && !field->Type->isObjectPointer() && cvarName != NAME_None
				&& pointee != nullptr && pointee->isNative
				&& pointee->TypeName == cvarName;
		}
		break;

	case Field_CanvasPtr:
		// ZScript's Canvas is the native FCanvas, the same identity Arg_Canvas relies on: DECLARE_CLASS
		// registers it and PClass strips the leading letter (dobjtype.cpp:351). Descendant rather than
		// equal so a field declared as a base of Canvas still matches - the pointer we store is still
		// provably assignable to it.
		ok = field->Type != nullptr && field->Type->isObjectPointer()
			&& static_cast<PObjectPointer *>(field->Type)->PointedClass() != nullptr
			&& RUNTIME_CLASS(FCanvas)->IsDescendantOf(static_cast<PObjectPointer *>(field->Type)->PointedClass());
		break;

	case Field_ObjArray:
		// A FIXED, INLINE array of object pointers - ZScript `Thing things[N];`. Every clause here is a
		// precondition of the pointer arithmetic that reads it, not decoration: an array read with a
		// wrong stride or a wrong count walks off the end of the object, which is an out-of-bounds read
		// rather than a misdraw.
		{
			// isArray() is set by PArray's constructor (types.cpp:1837) and NOT by PDynArray's
			// (:2167-2172), so a dynamic array is already excluded. PStaticArray DERIVES from PArray and
			// does set it, and has to be rejected explicitly: its storage is a pointer plus a count
			// rather than N elements inline, so the same arithmetic would dereference the wrong thing.
			PArray *arr = field->Type != nullptr && field->Type->isArray() && !field->Type->isStaticArray()
				? static_cast<PArray *>(field->Type) : nullptr;

			PClass *pointed = arr != nullptr && arr->ElementType != nullptr && arr->ElementType->isObjectPointer()
				? static_cast<PObjectPointer *>(arr->ElementType)->PointedClass() : nullptr;

			ok = arr != nullptr
				&& arr->ElementCount > 0
				// The stride and the extent the read below relies on, cross-checked against the type's
				// own total size. PArray computes Size as ElementSize * ElementCount (types.cpp:1836), so
				// this cannot disagree unless the layout is not what this code was written against.
				&& arr->ElementSize == sizeof(DObject *)
				&& (size_t)arr->ElementSize * arr->ElementCount == (size_t)arr->Size
				// viewCls is reused here as the expected ELEMENT class, the same way Field_ViewPtr uses
				// it as the expected pointee. Descendant so a base-typed array still matches; the runtime
				// IsKindOf before each element is read is what makes the cast valid per element.
				&& pointed != nullptr && viewCls != nullptr && pointed->IsDescendantOf(viewCls);
		}
		break;

	case Field_MenuPtr:
		// UIView.parentMenu (view.zs:124), declared as a UIMenu, and the one field here we store a MENU
		// into rather than a view. viewCls is reused as the expected pointee ancestor exactly as
		// Field_ObjArray reuses it as the expected element class: the menu we are about to store has to
		// derive from whatever the field is declared as, or the store is not type-safe for script.
		ok = field->Type != nullptr && field->Type->isObjectPointer()
			&& static_cast<PObjectPointer *>(field->Type)->PointedClass() != nullptr
			&& viewCls != nullptr
			&& viewCls->IsDescendantOf(static_cast<PObjectPointer *>(field->Type)->PointedClass());
		break;
	}

	if (!ok)
	{
		Printf(TEXTCOLOR_YELLOW "%s: %s.%s has an unexpected type, %s\n",
			subsystem, cls->TypeName.GetChars(), fieldname, disabledNote);
		return nullptr;
	}

	return field;
}

DObject *ReadObjectField(DObject *obj, const PField *field)
{
	return *(DObject **)((uint8_t *)obj + field->Offset);
}

FString *StringFieldAddr(DObject *obj, const PField *field)
{
	return (FString *)((uint8_t *)obj + field->Offset);
}

// ---------------------------------------------------------------------------------------------
// The tab strip. The argument for all of this is in i_auxvmreflect.h; what is here is the mechanism.
// ---------------------------------------------------------------------------------------------

// THE ONE NUMBER THIS IS ALL FOR: the horizontal text padding each tab gets, replacing Selaco's own 42
// (pda_menu.zs:238 and once per tab after it). (42 - 20) * 2 * 6 = 264 design pixels handed back, which is
// what brings the whole strip - trigger icons included - inside the narrower logical box the second screen
// lays the desktop out against. Vertical padding stays 0 exactly as Selaco sets it; only the width is short.
//
// A NAMED CONSTANT because it is a figure to be judged by eye on the panel rather than derived, so moving
// it has to stay a one-line change.
static const double TabTextPaddingX = 20.0;

// The six tabs, declared together at pda_menu.zs:41 and all constructed unconditionally in init.
static const char *const TabFieldNames[] =
{
	"readerButt", "mapButt", "objectivesButt", "statsButt", "tiersButt", "manualButt"
};

// Resolved once per class and reused by both modes, which host the same PDAMenu3 and so share the resolve.
//
// KEYED ON THE CLASS rather than on a bare "already tried" flag: a script recompile builds new PClass and
// PField objects and frees the old ones, so a stale PField here would be a write through a freed offset
// rather than a missed nicety. One attempt per class either way - on failure the class is still recorded
// and the symbols stay null, so the yellow line is said once and never per frame.
static PClass *TabPaddingClass = nullptr;
static PClass *TabPaddingTabClass = nullptr;
static PField *TabPaddingFields[countof(TabFieldNames)] = {};
static VMFunction *FuncSetTextPadding = nullptr;
static int SetTextPaddingRegs = 0;

// Resolve the six fields, the tab class and setTextPadding, leaving FuncSetTextPadding null on any miss.
// Split out so the caller below reads as resolve-then-apply; it records nothing but this file's own cache.
static void ResolveTabPadding(DObject *menu, PClass *menuCls, PClass *viewCls)
{
	// Named for the tab strip and not for either codex, because both modes narrow their tabs through here:
	// the default "AuxStandbyCodex" would be wrong half the time, and no caller is a better answer than the
	// feature itself. The wording matches the failure line further down so a log reads as one story.
	static const char *const Subsystem = "AuxTabStrip";
	static const char *const Note = "the tab strip keeps Selaco's own padding";

	for (unsigned i = 0; i < countof(TabFieldNames); i++)
	{
		TabPaddingFields[i] = ResolveField(menuCls, TabFieldNames[i], Field_ViewPtr, viewCls, Subsystem, Note);
		if (TabPaddingFields[i] == nullptr)
			return;
	}

	// setTextPadding is resolved against the class of a REAL tab, not against a name looked up by hand,
	// because ResolveMethod picks the entry out of that class's own vtable and the override that actually
	// runs is the one this code has to be checked against. PDATab does not override it (tabs.zs:1) and
	// UIButton's is what runs (button.zs:258) - but that is a fact about today's script, so it is proved
	// rather than assumed, and every tab is then required to be exactly this class.
	DObject *firstTab = ReadObjectField(menu, TabPaddingFields[0]);
	if (firstTab == nullptr || !firstTab->IsKindOf(viewCls))
	{
		Printf(TEXTCOLOR_YELLOW "AuxTabStrip: %s.%s is not a %s, the tab strip keeps Selaco's own padding\n",
			menuCls->TypeName.GetChars(), TabFieldNames[0], viewCls->TypeName.GetChars());
		return;
	}

	// setTextPadding(double left, double top, double right, double bottom) - four declared arguments, one
	// register each, five with self. All four have defaults, which does not change NumArgs: the caller
	// fills omitted ones in, so the callee still declares and reads five.
	static const EArgKind PaddingArgs[] = { Arg_Float, Arg_Float, Arg_Float, Arg_Float };
	PClass *tabCls = firstTab->GetClass();
	VMFunction *func = ResolveMethod(tabCls, "setTextPadding", Subsystem, PaddingArgs, 4, &SetTextPaddingRegs,
		nullptr, Note);
	if (func == nullptr)
		return;

	TabPaddingTabClass = tabCls;
	FuncSetTextPadding = func;
}

void TightenTabStrip(DObject *menu, PClass *menuCls, PClass *viewCls)
{
	if (menu == nullptr || menuCls == nullptr || viewCls == nullptr)
		return;

	if (menuCls != TabPaddingClass)
	{
		// Recorded BEFORE the resolve, so a resolve that fails still counts as the one attempt for this
		// class and cannot be retried every frame.
		TabPaddingClass = menuCls;
		TabPaddingTabClass = nullptr;
		FuncSetTextPadding = nullptr;
		SetTextPaddingRegs = 0;
		for (unsigned i = 0; i < countof(TabFieldNames); i++)
			TabPaddingFields[i] = nullptr;

		ResolveTabPadding(menu, menuCls, viewCls);
	}

	if (FuncSetTextPadding == nullptr)
		return;

	// APPLIED EVERY CALL, not once: the resolve is per class but the padding is per INSTANCE, and mode 4
	// gets a brand new PDAMenu3 every time the player opens their PDA. Writing the same four values again
	// is idempotent apart from re-setting requiresLayout, which the layout the caller is about to run
	// consumes anyway.
	unsigned tightened = 0;
	try
	{
		for (unsigned i = 0; i < countof(TabFieldNames); i++)
		{
			DObject *tab = ReadObjectField(menu, TabPaddingFields[i]);
			if (tab == nullptr || tab->GetClass() != TabPaddingTabClass)
			{
				// Exactly the class setTextPadding was resolved against, not merely a kind of it: a
				// subclass could override it with something this code has not read. Same rule, and the
				// same reason, as both modes' "is not a plain UIView" checks on mainView.
				Printf(TEXTCOLOR_YELLOW "AuxTabStrip: %s.%s is not a plain %s, the tab strip keeps the "
					"padding it has\n", menuCls->TypeName.GetChars(), TabFieldNames[i],
					TabPaddingTabClass->TypeName.GetChars());
				return;
			}

			VMValue params[] = { tab, TabTextPaddingX, 0.0, TabTextPaddingX, 0.0 };
			VMCall(FuncSetTextPadding, params, SetTextPaddingRegs, nullptr, 0);
			tightened++;
		}
	}
	catch (const std::exception &e)
	{
		// A partial application is left standing rather than unwound: some tabs tight and some wide is
		// cosmetically odd but lays out and draws perfectly, whereas putting 42 back would be six more
		// calls through the thing that just aborted. The count is printed because "some of them" is the
		// one detail that makes an odd-looking strip legible as this rather than as a layout bug.
		Printf(TEXTCOLOR_YELLOW "AuxTabStrip: setting the tab padding aborted after %u of %u tabs (%s), "
			"the strip keeps the padding it has\n", tightened, (unsigned)countof(TabFieldNames), e.what());
	}
}

}   // namespace AuxView

// ---------------------------------------------------------------------------------------------
// THE RESTART TEARDOWN HOOK: forget everything this port has resolved out of script.
//
// Called from D_Cleanup (d_main.cpp) on the `restart` CCMD, immediately before PClass::StaticShutdown().
// That function deletes every PClass, deletes every VMFunction, releases every symbol table and frees the
// class data allocator - and the engine then reinitialises IN PROCESS and re-parses all script. So from
// that line onward every PClass*, PField*, PFunction* and VMFunction* cached anywhere in this port is
// dangling, together with the AUXCANVAS FCanvas (a DObject owned by a texture D_Cleanup has already
// deleted) and the DObjects two GC marker functions still mark.
//
// THE MARKED DObjects ARE THE REPORTED CRASH. There is no RemoveMarkerFunc and GC's marker array is never
// cleared, so both markers keep reading their globals for the life of the process: StandbyMenu and
// AuxReceiver were therefore marked live through every collection in D_Cleanup, outlived the class
// deletion, and the first collection after the restart - DestroyAllThinkers loading TITLEMAP - walked them
// through a freed PClass in DObject::PropagateMark.
//
// WHY IT IS SAFE HERE, and it is the only property that matters: every one of the eight functions it calls
// does nothing but store to its own file statics. No VMCall, no Destroy(), no scripted onDestroy, no sound,
// no music, no texture or menu access, nothing that can throw. In particular this is NOT StandbyViewDiscard
// or an equivalent - that one calls menu->Destroy(), which dispatches PDAMenu3's scripted onDestroy and
// I_SetMusicVolume through it, both illegal at a point where the sound system is already down and the
// classes are about to go. Unrooting the objects and letting the collection inside StaticShutdown sweep
// them is strictly safer: that collection runs with bVMOperational already false, so DObject::Destroy
// calls no script at all, and it runs before the PClasses are deleted, so ~DObject can still read them.
//
// Safe to call when nothing was ever built - every store is unconditional and every target starts at the
// same value it is being put back to - and safe to call twice, which it is: GameMain calls D_Cleanup a
// second time on the way out.
//
// LEVEL CHANGES DO NOT COME THROUGH HERE. They keep going through StandbyViewDiscard and the per-frame
// edges, deliberately: this throws away every resolve, which would mean a full rebuild, and a map change is
// the case the standby codex is specifically built to survive without one.

// Revision-unique, in the same shape as the other aux build ids, so `strings` on the packaged library
// answers "is this change in the binary" without a device run - the one check this project's build chain
// makes necessary, because every exit code in it can report success against a tree it did not rebuild.
static const char *const AuxRestartBuild = "AUXRESTART_BUILD_20260920_M9_R1";

void I_AuxForgetScriptState()
{
	// One line per restart, and the only output this function produces. It is here because the hook is
	// otherwise completely silent: a restart that crashed the same way as before would give no way to tell
	// "the fix is not in this binary" from "the fix does not work", which is the reading this project has
	// been caught by before. The build id is also what proves the change reached the packaged library.
	//
	// PRINT_NONOTIFY IS LOAD-BEARING, NOT TIDINESS. Without it PrintString feeds the notify buffer, and
	// FNotifyBuffer::AddString does a VMCall on StatusBar.ProcessNotify (c_notifybuffer.cpp:96-106) and reads
	// twod for the scale - a VM call from inside the teardown, which is the one thing this whole function
	// exists to avoid. NONOTIFY leaves I_PrintStr (so it still reaches logcat), the console buffer and the
	// log file, none of which touch the VM, a font or a drawer.
	Printf(PRINT_HIGH | PRINT_NONOTIFY, "AuxRestart: forgetting cached script state before the class "
		"teardown (build=%s)\n", AuxRestartBuild);

	// This file's own two caches first, because both are shared BY the files below rather than owned by one
	// of them, and neither is reset by anything else.
	//
	// The tab-padding group is keyed on the menu class, which is the right shape but compares POINTERS: the
	// restart re-parses the same script in the same order, so the new PDAMenu3 can land on the freed one's
	// address, the key matches, and six freed PFields are then used as raw byte offsets.
	AuxView::TabPaddingClass = nullptr;
	AuxView::TabPaddingTabClass = nullptr;
	for (unsigned i = 0; i < countof(AuxView::TabFieldNames); i++)
		AuxView::TabPaddingFields[i] = nullptr;
	AuxView::FuncSetTextPadding = nullptr;
	AuxView::SetTextPaddingRegs = 0;

	// The cross-mode channel, which is now the view-state manager's whole state rather than one PClass* here.
	// Its own comment says a PClass* is never freed and so the app choice is never cleared; that holds within a
	// session and StaticShutdown is where it stops holding, which is the one place "the last app this session"
	// stops meaning anything. Called before the rest for the same reason the tab-padding group is: it is shared
	// BY the two mode files rather than owned by one of them.
	I_AuxCodexViewStateForgetScriptState();

	I_AuxCanvasForgetScriptState();
	I_AuxCodexForgetScriptState();
	I_AuxStandbyCodexForgetScriptState();
	I_AuxLiveCodexForgetScriptState();
	I_AuxPanelForgetScriptState();
	I_AuxDevicePickerForgetScriptState();
	I_AuxDeviceResetForgetScriptState();

	// i_auxprofile.cpp is deliberately absent: it caches nothing engine-derived and latches nothing, so it
	// re-resolves every cvar, option-value block and UIHelper method on each call already.
}
