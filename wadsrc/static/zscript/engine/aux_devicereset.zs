//===========================================================================
//
// AuxDeviceResetPrompt
//
// The script half of the Android port's "Reset Device Choice" button, which
// replaces Selaco's "Choose Optimal Settings" on the Handhelds options page.
// The rest of the button - the item, the prompt and the reset itself - is
// C++, in src/common/platform/posix/android/i_auxdevicereset.cpp.
//
// THIS CLASS EXISTS BECAUSE A CALLBACK CANNOT BE SYNTHESISED FROM C++. Selaco's
// PromptMenu reports which button the player pressed through its own
// `Function<ui void(Object, int)> onClosed` field, and the value such a field
// holds is a PFunction the compiler had to have built from a real declaration.
// So the declaration lives here and C++ writes it into their field by
// reflection.
//
// IT NAMES NO SELACO TYPE, WHICH IS THE ONLY REASON IT CAN LIVE HERE. This lump
// is in gzdoom.pk3 and therefore compiles BEFORE the player's Selaco.ipk3, so a
// class from theirs would not exist yet. Object and int are core ZScript types,
// which is what lets the signature match their field with neither side naming
// the other.
//
// NOT IN A pk3 LOADED AFTER THE IWAD, which was tried first and does not work:
// Selaco's ipk3 carries its own iwadinfo.txt, so the fork's Load entry is never
// read and such a pk3's ZSCRIPT is never compiled at all.
//
//===========================================================================

class AuxDeviceResetPrompt
{
	// Called by PromptMenu.handleControl through onClosed (prompt.zs:300-301); `button` is the
	// controlID of the button that was activated, which is its index in the prompt's own list.
	//
	// IT REPORTS THE BUTTON AND DECIDES NOTHING. Which index means "confirm" is chosen by the C++
	// that built the prompt, so it is the C++ that compares against it - otherwise the meaning of
	// the index would be written down in two places free to drift apart. -1 in the cvar means "no
	// answer", so every value this writes is an answer.
	//
	// A CVAR RATHER THAN A CCMD because UI scope cannot run one, and the work has to happen in C++
	// anyway. Setting a cvar IS allowed from here: CVar.SetInt only refuses outside menu code
	// (vmnatives.cpp:985-991), and every path that activates a prompt button runs inside DMenu's
	// OnUIEvent, OnInputEvent or MenuEvent, each of which raises DMenu::InMenu (menu.cpp:290-328).
	//
	// PLAIN RATHER THAN `ui`, and plain is the load-bearing choice: a function with no scope
	// qualifier in a class with none is Side_PlainData, and CheckSidesForFunctionPointer accepts
	// Side_PlainData into a pointer of ANY scope (scopebarrier.cpp:211-217). So this one
	// declaration remains assignable to their field whether they declare it ui, play or clearscope.
	//
	// `receiver` is accepted and ignored. It is the second half of their calling convention rather
	// than something this needs: C++ puts an instance of this class in it so the prompt is left in
	// the same shape Selaco's own callers leave it, and there is no state for it to carry.
	static void Answered(Object receiver, int button)
	{
		let cv = CVar.FindCVar('aux_devicereset');
		if(cv) cv.SetInt(button);
	}
}
