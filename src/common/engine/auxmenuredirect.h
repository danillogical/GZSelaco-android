#pragma once

/*
** auxmenuredirect.h
**
** THE LIVE CODEX (mode 4) - let the engine run the game's own menu completely normally, and change
** only WHERE it draws.
**
** On a dual-screen handheld (the AYN Thor) Selaco's PDA/codex belongs on the lower panel, with the
** game still visible on the upper one. Everything needed for that already exists: the menu becomes
** CurrentMenu, the engine ticks it, routes gamepad input to it and calls M_Drawer() every frame. The
** ONLY thing this changes is the drawer the 2D commands land in - `twod` is a plain non-const global
** pointer (v_2ddrawer.cpp:47), so pointing it at an offscreen canvas's F2DDrawer for the duration of
** M_Drawer sends the whole menu there instead of over the game. Nothing about the menu system, the
** input path or the tick has to know.
**
** WHY THIS IS AN RAII GUARD IN A SHARED HEADER RATHER THAN AN #ifdef AROUND THE CALL.
**
** RAII because a VM abort inside a menu drawer unwinds as a C++ exception (CVMAbortException derives
** from std::exception) straight past any manual restore. A leaked swap would leave `twod` pointing at
** a canvas drawer for the rest of the frame and every frame after it, which sends the ENTIRE game
** HUD, console and stat display to the second screen and leaves the main one blank. That is a much
** worse failure than the feature not working.
**
** A shared header because the alternative is an #ifdef in the middle of DrawOverlays, and the whole
** convention in this fork is that a four-line guard in a shared file survives a merge from upstream
** where a forked function does not. Off Android this class has an empty body and compiles to nothing.
**
** The implementation lives in common/platform/posix/android/i_auxlivecodex.cpp, which owns mode 4; the
** reflection helpers it uses are shared with mode 3 through i_auxvmreflect.h.
*/

class F2DDrawer;
class DMenu;
class FCanvas;

#ifdef __ANDROID__

// Active only while aux_codex_mode is 4, the second-screen panel is live, and Selaco's PDAMenu3
// is the current menu. In every other case the constructor does nothing at all and M_Drawer draws to
// the main screen exactly as it always did - which is also the correct fallback for a non-Selaco IWAD,
// for a device with one screen, and for any failure inside the mode.
class FAuxMenuRedirect
{
public:
	FAuxMenuRedirect();
	~FAuxMenuRedirect();

	FAuxMenuRedirect(const FAuxMenuRedirect &) = delete;
	FAuxMenuRedirect &operator=(const FAuxMenuRedirect &) = delete;

private:
	F2DDrawer *Saved = nullptr;
	FCanvas *Canvas = nullptr;

	// The menu whose DontBlur was overridden. Held rather than re-read from CurrentMenu on the way
	// out, because a menu action during M_Drawer can change CurrentMenu and restoring the flag on a
	// different object than it was taken from would corrupt that menu's state instead - but the
	// restore is still gated on the two being EQUAL, because a menu that closed itself has been
	// Destroy()ed and may already have been swept, which would make the write a use-after-free.
	DMenu *Blurred = nullptr;
	bool SavedDontBlur = false;
	bool Redirected = false;
};

#else

// Inert. The constructor and destructor are user-provided rather than defaulted on purpose: that
// makes the type non-trivially-destructible, which is what stops -Wunused-variable firing on the
// guard object in DrawOverlays on every non-Android target.
class FAuxMenuRedirect
{
public:
	FAuxMenuRedirect() {}
	~FAuxMenuRedirect() {}

	FAuxMenuRedirect(const FAuxMenuRedirect &) = delete;
	FAuxMenuRedirect &operator=(const FAuxMenuRedirect &) = delete;
};

#endif
