/*
** i_auxpanel.cpp
** Second-screen panel for dual-screen Android handhelds (AYN Thor).
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
** The engine never learns that a second screen exists. There is no second surface, no second
** swapchain and no present queue: the panel is an ordinary Android Presentation drawing an ordinary
** View, fed a few primitives per push.
**
** THAT IS NO LONGER "NOTHING VULKAN", and the difference is worth stating plainly because an earlier
** version of this header claimed the renderer was bit-identical to a build without the feature. It is
** not. Getting engine 2D content to the panel costs three things on the main render path:
** VK_IMAGE_USAGE_TRANSFER_SRC_BIT on every hardware canvas (vk_hwtexture.cpp), a copy plus two layout
** barriers recorded into the frame's draw command buffer, and a WaitForCommands(false) that drains
** every outstanding fence - measured at ~30 ms in a real level, a whole frame at vid_maxfps 30. What
** survives of the original argument is the part that matters: none of it is a SHARED or long-lived
** resource, so a failure here cannot wedge the main screen's swapchain, and the stall is why the
** readback is edge-triggered rather than paced.
**
** WHAT CROSSES THE BOUNDARY. nativeAuxEnable(boolean) comes IN, from the Android UI thread,
** telling native whether a panel is actually up. Two static Java methods on com.selaco.game.AuxPanel
** go OUT, resolved once and then latched (AuxResolve):
**   pushPixels(ByteBuffer, int, int)      - the AUXCANVAS readback, edge-triggered on a content
**                                           change (and paced in mode 4) because each readback stalls
**                                           the GPU (i_auxcanvas.cpp). The buffer is a direct
**                                           ByteBuffer over the persistent staging mapping, wrapped
**                                           once and reused; Java copies it synchronously.
**   clearPixels()                         - drops the last pushed frame on level exit, best-effort,
**                                           so the panel falls through to Selaco's startup splash.
**
** WHO OWNS THE CANVAS is decided here, in I_AuxPanelFrame, from aux_canvas_zscript. Exactly one
** drawer runs per interval, because all of them write the same texture: 0 draws the C++ test pattern
** (i_auxcanvas.cpp) and 3 the whole PDAMenu3 desktop through VM reflection (i_auxcodexview.cpp),
** falling back to no content at all on any game where its class does not exist. The numbering has
** holes in it: 1 and 2 were removed and are deliberately not reused - see the mode dispatch below.
**
** MODE 4 IS THE ODD ONE OUT and the only one that draws from somewhere else. It is "Wii U mode": when
** the player opens the PDA for real, the engine's own M_Drawer is redirected into the canvas from
** DrawOverlays (d_main.cpp) by pointing `twod` at it, so the menu the engine is already ticking and
** feeding input to lands on the second screen while the game stays on the first. That happens LATER in
** the frame than this function, so mode 4 has no phase 1 here - only the publish decision and the
** shared phase 2. Its content is live rather than static, so it is paced rather than edge-triggered.
** See MODE 4 in i_auxmenuview.cpp.
**
** MODE 5 IS THE SHIPPING COMBINATION, and it is a dispatch rather than an implementation: it resolves
** per frame to 3 while the PDA is shut and 4 while it is open, so the panel carries the read-only
** dashboard during gameplay and the live PDA when the player opens it. Both edges fall out of the
** existing mode-change trigger - see the comment at the resolve in I_AuxPanelFrame - which is why mode 5
** adds no edge detection and no state of its own.
**
** The four structural questions this file was built to falsify all have answers, on hardware: a
** Presentation does show on the second display while another app's activity owns it,
** FLAG_NOT_FOCUSABLE does keep AppActive true so neither screen freezes, FindClass does work from
** the game thread, and the cost to the main screen is the readback stall rather than the pushes.
** What has NOT changed is the rule that made them answerable: every new mechanism arrives behind its
** own latch and its own cvar, so a failure can still be attributed to one piece.
*/

#include <atomic>
#include <stdint.h>

#include <SDL_system.h>
#include <jni.h>

#include "c_cvars.h"
#include "doomstat.h"
#include "gamestate.h"
#include "i_time.h"
#include "printf.h"

// User-facing toggle. Off must be genuinely inert - no probe, no canvas draw, no readback, no JNI -
// not merely hidden, so a device that leaves it off pays nothing beyond the read of this bool.
CVAR(Bool, aux_panel, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

// Whether Java currently has a panel up. Written ONLY by nativeAuxEnable, from the Android UI
// thread; read every frame by the game thread. Relaxed is sufficient: this is a pure hint, and a
// push that races a teardown is harmless because the Java entry points only ever store a value.
static std::atomic<bool> AuxLive{ false };

// Latched on the first failure and never retried.
//
// A panel that cannot work will not start working, and retrying JNI resolution every frame would
// turn a broken optional feature into a per-frame cost on the main path - the opposite of the
// point. One diagnostic line, then silence forever.
static bool AuxBroken = false;

static jclass AuxClass = nullptr;
static jmethodID AuxPushPixels = nullptr;
static jmethodID AuxClearPixels = nullptr;

// Wraps the canvas readback's persistent staging mapping. Created once - the mapping's address
// never moves - and kept as a global ref for the life of the process, same lifetime as AuxClass.
static jobject AuxPixelsBuffer = nullptr;

// The draw/readback edge. Split across two frames on purpose: the canvas is not rendered until
// RenderView, later in the same frame than this function runs, so the readback has to wait a frame
// for the draw it just queued. See the comment above I_AuxPanelFrame for the cost that makes this
// worth getting right, and I_AuxCanvasRenderPending in i_auxcanvas.cpp for the ordering proof.
static int AuxCanvasDrawRequests = 0;
static bool AuxCanvasNeedsReadback = false;
static int AuxLastMode = -1;        // -1 so the first live frame always counts as a mode change
static bool AuxWasLive = false;
static int AuxForceFrameCount = 0;

// Readback logging, rate-limited - see AuxCanvasReadbackPhase. An outlier is always worth a line
// because the cost is the whole reason the number is logged; a routine one at most once a second,
// which is enough to confirm the path is alive without burying the other diagnostics.
static const double AuxReadbackLogAlwaysMs = 50.0;
static const double AuxReadbackLogIntervalMs = 1000.0;

// Mode 4's arming of the readback.
//
// Modes 0 and 3 issue their canvas draw from inside I_AuxPanelFrame, so they set the flag directly in
// phase 1 below. Mode 4 cannot: its draw is issued by the engine from DrawOverlays, LATER in the frame
// than this function runs (d_main.cpp:907, reached from :1254, vs :1102), so it has to be able to arm phase 2 from there.
//
// These deliberately expose the SAME flag rather than adding a parallel one. There is one canvas, one
// readback and one place that decides the readback happened, and a second flag would be a second thing
// that could disagree with it. I_AuxPanelReadbackPending is what stops mode 4 stacking publishes: a
// second NeedUpdate before the first has been read back would keep the texture permanently dirty and
// starve the phase-2 gate.
void I_AuxPanelRequestReadback() { AuxCanvasNeedsReadback = true; }
bool I_AuxPanelReadbackPending() { return AuxCanvasNeedsReadback; }

extern "C" JNIEXPORT void JNICALL
Java_com_selaco_game_AuxPanel_nativeAuxEnable(JNIEnv *env, jclass cls, jboolean on)
{
	AuxLive.store(on == JNI_TRUE, std::memory_order_relaxed);
}

// Resolve the Java side once. Returns false and latches AuxBroken on any failure.
//
// FindClass is expected to work because the game thread's stack bottom is SDLMain.run(), so the
// app class loader is in scope - but that is the one API assumption in the design, which is why a
// failure here is a clean permanent disable rather than an assert.
static bool AuxResolve(JNIEnv *env)
{
	jclass local = env->FindClass("com/selaco/game/AuxPanel");
	if (env->ExceptionCheck() || local == nullptr)
	{
		env->ExceptionClear();
		Printf(TEXTCOLOR_YELLOW "AuxPanel: FindClass failed, second-screen panel disabled\n");
		AuxBroken = true;
		return false;
	}

	AuxClass = (jclass)env->NewGlobalRef(local);
	env->DeleteLocalRef(local);

	AuxPushPixels = env->GetStaticMethodID(AuxClass, "pushPixels", "(Ljava/nio/ByteBuffer;II)V");
	if (env->ExceptionCheck() || AuxPushPixels == nullptr)
	{
		env->ExceptionClear();
		Printf(TEXTCOLOR_YELLOW "AuxPanel: pushPixels not found, second-screen panel disabled\n");
		AuxBroken = true;
		return false;
	}

	AuxClearPixels = env->GetStaticMethodID(AuxClass, "clearPixels", "()V");
	if (env->ExceptionCheck() || AuxClearPixels == nullptr)
	{
		env->ExceptionClear();
		Printf(TEXTCOLOR_YELLOW "AuxPanel: clearPixels not found, second-screen panel disabled\n");
		AuxBroken = true;
		return false;
	}
	return true;
}

// Drop the last codex frame on level exit, best-effort, so the panel falls through to Selaco's
// startup splash (or black, if that has not loaded) instead of freezing on whatever the dashboard
// last drew. Called from a plain local flag in I_AuxPanelFrame, not the AuxLastMode/AuxWasLive
// machinery used elsewhere in this file - being out of sync for a frame or two here is cosmetic,
// not a correctness bug, so it does not earn that machinery's weight.
static void I_AuxPanelClearPixels()
{
	if (AuxBroken || !AuxLive.load(std::memory_order_relaxed))
		return;

	JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
	if (env == nullptr)
		return;
	if (AuxClass == nullptr && !AuxResolve(env))
		return;

	env->CallStaticVoidMethod(AuxClass, AuxClearPixels);
	if (env->ExceptionCheck())
	{
		env->ExceptionClear();
		Printf(TEXTCOLOR_YELLOW "AuxPanel: clearPixels threw, second-screen panel disabled\n");
		AuxBroken = true;
	}
}

// Hand a completed canvas readback to Java as a direct ByteBuffer over the persistent staging
// mapping, wrapping it once and reusing the wrapper on every later push - the mapping's address
// never moves for the life of the process, so there is nothing to re-wrap.
//
// Buffer-safety discipline: one native buffer is aliased by the game thread (writer, via the
// readback that just completed) and the UI thread (reader, inside pushPixels' copyPixelsFromBuffer).
// That copy finishes synchronously before this call returns, so the earliest the game thread can
// start overwriting the buffer is its NEXT readback - long after Java is done reading this one.
// That ordering is what makes one reused buffer safe without a queue or a second thread.
static void AuxPushPixelsToJava(JNIEnv *env, const uint8_t *pixels, int w, int h)
{
	if (AuxPixelsBuffer == nullptr)
	{
		jobject local = env->NewDirectByteBuffer(const_cast<uint8_t *>(pixels), (jlong)w * h * 4);
		if (env->ExceptionCheck() || local == nullptr)
		{
			env->ExceptionClear();
			Printf(TEXTCOLOR_YELLOW "AuxPanel: NewDirectByteBuffer failed, second-screen panel disabled\n");
			AuxBroken = true;
			return;
		}

		AuxPixelsBuffer = env->NewGlobalRef(local);
		env->DeleteLocalRef(local);
		if (AuxPixelsBuffer == nullptr)
		{
			Printf(TEXTCOLOR_YELLOW "AuxPanel: NewGlobalRef failed, second-screen panel disabled\n");
			AuxBroken = true;
			return;
		}
	}

	env->CallStaticVoidMethod(AuxClass, AuxPushPixels, AuxPixelsBuffer, (jint)w, (jint)h);
	if (env->ExceptionCheck())
	{
		// Describe before clearing. Clearing alone discards the exception's message, which cost a
		// diagnosis cycle here once: a thrown pushPixels was indistinguishable from any other
		// failure until the Java side was read by hand.
		env->ExceptionDescribe();
		env->ExceptionClear();
		Printf(TEXTCOLOR_YELLOW "AuxPanel: pushPixels threw, second-screen panel disabled\n");
		AuxBroken = true;
	}
}

// Diagnostic only: force a redraw and readback every N frames regardless of whether anything
// changed. 0, the default, means edge-triggered - the shipping behaviour.
//
// This exists because the readback cost can only be measured by repeating it, and the edge-triggered
// path deliberately fires almost never. Flags 0, not CVAR_ARCHIVE: same reasoning as the other two
// diagnostics in this feature, and an archived version of this one would silently reintroduce the
// dropped frame it was added to remove.
CVAR(Int, aux_canvas_force_interval, 0, 0)

// WHAT A READBACK COSTS, measured on the AYN Thor, and why the number to remember is the big one:
//
//     in-level   median 30.44 ms   (min 27.34, max 30.87)
//     title screen        0.99 ms
//
// I_AuxCanvasReadback ends in WaitForCommands(false), which waits on every outstanding fence. At the
// title screen there is almost nothing in flight so it returns immediately; in a real level it drains
// an entire frame's work. 30 ms is one whole frame at vid_maxfps 30, so the old fixed 30-frame
// interval dropped a frame about once a second. Anyone sampling this at the menu will conclude the
// readback is ~1 ms and free, which is wrong by a factor of thirty - measure in-level or do not
// measure.
//
// That is the entire reason this is edge-triggered rather than paced. The premise the synchronous
// readback design rested on is that the codex is STATIC between unlocks, so the correct number of
// readbacks per second is normally zero.

// PHASE 2 - read back, but only once the frame that renders the canvas has actually happened. The
// flag is held rather than dropped if it has not: RenderView does not run in every gamestate, and
// a readback before the render would push stale pixels.
//
// Shared by every mode. Modes 0 and 3 reach it by falling through phase 1; mode 4 calls it directly,
// because it has no phase 1 of its own - see the mode dispatch in I_AuxPanelFrame.
static void AuxCanvasReadbackPhase(JNIEnv *env)
{
	if (!AuxCanvasNeedsReadback)
		return;

	{
		extern bool I_AuxCanvasRenderPending();
		if (I_AuxCanvasRenderPending())
			return;
	}

	AuxCanvasNeedsReadback = false;

	extern bool I_AuxCanvasReadback(const uint8_t **outPixels, int *outWidth, int *outHeight);
	const uint8_t *pixels = nullptr;
	int w = 0, h = 0;
	const double t0 = I_msTimeF();
	if (I_AuxCanvasReadback(&pixels, &w, &h))
	{
		const double ms = I_msTimeF() - t0;

		// Rate-limited rather than unconditional. This is still the only place the stall shows up, but
		// mode 4 publishes every AuxMenuPublishInterval frames - roughly 15 a second - so logging each
		// one put 15 lines a second of I/O on the game thread and buried the AuxDesktopView/AuxMenuView
		// lines that failures actually surface through.
		//
		// An expensive readback is always logged, because an outlier is the interesting case and the
		// whole reason the number is here; a routine one at most once a second, which is enough to
		// confirm the path is alive and to sample the cost.
		static double AuxLastReadbackLog = 0.0;
		const double now = I_msTimeF();
		if (ms > AuxReadbackLogAlwaysMs || now - AuxLastReadbackLog >= AuxReadbackLogIntervalMs)
		{
			AuxLastReadbackLog = now;
			Printf("AuxCanvas: readback %dx%d in %.2f ms\n", w, h, ms);
		}

		AuxPushPixelsToJava(env, pixels, w, h);
	}
}

// Called once per frame from D_Display, before screen->BeginFrame().
//
// The early return is the entire cost on a single-screen device, or with aux_panel off: one cvar
// read, one relaxed atomic load and two bool tests, with no JNI, no canvas draw and no lock.
// AuxLive is false unless Java has actually shown a panel, so a device with one screen never gets
// past the first two lines.
void I_AuxPanelFrame()
{
	if (!aux_panel)
		return;

	// Best-effort splash-on-exit: a local flag rather than the AuxLastMode/AuxWasLive edge machinery
	// below, because this does not need to be airtight - a missed or doubled call here costs a
	// cosmetic frame or two, not correctness. Kept ahead of the AuxLive early return so it still
	// notices a level ending while the panel happens to be transiently down.
	{
		static bool AuxWasInLevel = false;
		const bool inLevel = (gamestate == GS_LEVEL);
		if (AuxWasInLevel && !inLevel)
			I_AuxPanelClearPixels();
		AuxWasInLevel = inLevel;
	}

	if (AuxBroken || !AuxLive.load(std::memory_order_relaxed))
	{
		// A panel that goes away and comes back has no pixels of its own, so the next live frame has
		// to push again even if nothing else changed.
		AuxWasLive = false;
		return;
	}

	JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
	if (env == nullptr)
	{
		Printf(TEXTCOLOR_YELLOW "AuxPanel: no JNI env on the game thread, panel disabled\n");
		AuxBroken = true;
		return;
	}

	if (AuxClass == nullptr && !AuxResolve(env))
		return;

	// ------------------------------------------------------------------------------------------
	// Canvas ownership and the draw/readback edge.
	//
	// Reading the cvar here also keeps its registration alive: it has no other C++ reference, and
	// the CVar decl lives in the vreg section that --gc-sections is documented to collect on this
	// target (CMakeLists.txt:245-250).
	// ------------------------------------------------------------------------------------------
	EXTERN_CVAR(Int, aux_canvas_zscript)
	const int cvarMode = aux_canvas_zscript;

	// MODE 5 - the combined mode, and the one that actually meets the requirement: the read-only dashboard
	// on the panel during gameplay, replaced by the live PDA while the player has it open. It is not a
	// third implementation - it RESOLVES, once per frame, to whichever of the two existing owners should
	// hold the canvas right now, so modes 3 and 4 keep their own code, their own latches and their own
	// individual cvar values for attributing a failure to one piece.
	//
	// Exactly one owner per frame is what the whole file's design rests on, and resolving here rather than
	// letting both run is what preserves it.
	int mode = cvarMode;
	if (cvarMode == 5)
	{
		extern bool I_AuxMenuViewIsPdaOpen();
		mode = I_AuxMenuViewIsPdaOpen() ? 4 : 3;
	}

	// MODES 1 AND 2 WERE REMOVED and their numbers are deliberately not reused, so a config or a habit
	// that still names one lands here rather than silently on something else. Both fall through to the
	// no-content branch below, which drops the frame and lets Java show Selaco's startup splash - and a
	// blank panel with no explanation is exactly the mystery this line exists to prevent. Said once,
	// because it would otherwise repeat every frame for the rest of the session.
	if (mode == 1 || mode == 2)
	{
		static bool AuxRemovedModeWarned = false;
		if (!AuxRemovedModeWarned)
		{
			AuxRemovedModeWarned = true;
			Printf(TEXTCOLOR_YELLOW "AuxPanel: aux_canvas_zscript %d was removed - mode 3 draws the whole PDA "
				"desktop, mode 5 is the default. The panel will show nothing.\n", mode);
		}
	}

	// BOTH EDGES ARE HANDLED BY THE EXISTING mode != AuxLastMode TRIGGER, which is why mode 5 needs no
	// edge detection of its own - and the falling edge is the one that matters.
	//
	// FALLING (PDA closes, 4 -> 3): the mode-change trigger below hands mode 3 a draw request, so the
	// dashboard is cleared and redrawn onto the panel immediately. Mode 4's own close-edge behaviour - a
	// black clear, because on its own it has nothing to fall back to - is simply never reached, because
	// I_AuxMenuViewFrame is not called on a frame that resolves to 3. So in mode 5 the panel returns to the
	// dashboard rather than going black, and the Java-side sPixels fallback is not needed at all.
	//
	// RISING (PDA opens, 3 -> 4): mode 4's branch zeroes AuxCanvasDrawRequests, so a mode-3 draw request
	// that had been queued but not yet issued is dropped rather than drawing the dashboard over the PDA. A
	// mode-3 readback already in flight is still completed and pushed by the shared phase 2, which is
	// correct - it publishes the last dashboard frame - and mode 4 then takes over.
	if (mode == 4)
	{
		// MODE 4 ("Wii U mode") has no phase 1. Its draw is not issued from here at all: the engine draws
		// Selaco's real PDA into the canvas from DrawOverlays, LATER in this same frame (d_main.cpp:907 vs
		// :1102), through the twod swap in FAuxMenuRedirect. So this branch only lets the mode arm the guard
		// and pace its publishing, then runs the shared readback. See MODE 4 in i_auxmenuview.cpp.
		//
		// The readback goes FIRST, before the publish decision, and the order is what sets the achievable
		// cadence. I_AuxMenuViewFrame refuses to publish while a readback is still outstanding, so asking
		// it before clearing this frame's outstanding one costs a whole extra frame per cycle - the
		// intended 2-frame period becomes 3. Reading back first means the frame that completes a cycle can
		// also start the next one.
		AuxCanvasReadbackPhase(env);

		extern void I_AuxMenuViewFrame();
		I_AuxMenuViewFrame();

		// Phase-1 bookkeeping, kept consistent for whichever mode comes next. Switching AWAY from 4 must
		// still read as a mode change, and no mode-4 frame may leave a draw request behind for a phase
		// this mode does not have - one would make mode 4 paint the C++ test pattern over the PDA.
		AuxLastMode = mode;
		AuxWasLive = true;
		AuxCanvasDrawRequests = 0;
		AuxForceFrameCount = 0;
		return;
	}

	// Ask the codex view whether it has anything new. Cheap when it does not - two latched bools, a
	// null test and one integer compare - and it must be asked every frame, not only when we intend to
	// draw, because it is also what performs the deferred build and the rebuild on an unlock change.
	bool codexAvailable = false;
	bool codexChanged = false;
	if (mode == 3)
	{
		extern bool I_AuxCodexViewUpdate(int mode, bool *outNeedsRedraw);
		codexAvailable = I_AuxCodexViewUpdate(mode, &codexChanged);
	}

	// Everything that can make the canvas stale. Nothing else may set this: a redraw costs a ~30 ms
	// readback, so a trigger that fires spuriously is a dropped frame.
	if (mode != AuxLastMode)  AuxCanvasDrawRequests = 1;   // switching owner changes the image
	if (!AuxWasLive)          AuxCanvasDrawRequests = 1;   // panel just appeared, Java has nothing
	if (codexChanged)         AuxCanvasDrawRequests = 1;   // built or rebuilt the dashboard
	AuxLastMode = mode;
	AuxWasLive = true;

	// The diagnostic pacer, and the only thing here that fires without a content change.
	if (aux_canvas_force_interval > 0)
	{
		if (++AuxForceFrameCount >= aux_canvas_force_interval)
		{
			AuxForceFrameCount = 0;
			AuxCanvasDrawRequests = 1;
		}
	}
	else
	{
		AuxForceFrameCount = 0;
	}

	// PHASE 1 - queue the draw. Returns immediately afterwards: the canvas is not rendered until
	// RenderView later this same frame (d_main.cpp:1115 vs :1102), so reading it back now would copy
	// the PREVIOUS contents and the new content would never reach the panel. See
	// I_AuxCanvasRenderPending in i_auxcanvas.cpp.
	if (AuxCanvasDrawRequests > 0)
	{
		AuxCanvasDrawRequests--;

		if (mode == 3 && codexAvailable)
		{
			// Clear first. The desktop does not cover all 1240x1080 - it leaves margins where its logical
			// box does not reach the panel edges - and the canvas texture is never cleared for us, so
			// without this whatever the previous owner drew frames the new content.
			extern void I_AuxCanvasClear();
			I_AuxCanvasClear();

			extern void I_AuxCodexViewDraw(int mode);
			I_AuxCodexViewDraw(mode);
		}
		else if (mode == 0)
		{
			// Mode 0 is the diagnostic test pattern, and it is now the ONLY way to see it: nothing falls
			// back to it any more.
			extern void I_AuxCanvasDrawTestPattern();
			I_AuxCanvasDrawTestPattern();
		}
		else
		{
			// Nothing legitimate to show: a mode 3/4/5 with no view built is every non-Selaco IWAD plus
			// every frame before a level loads, and a removed mode 1 or 2 lands here too. Drop the codex
			// frame instead of drawing something: Java then falls through to Selaco's startup splash, or
			// to black if that has not decoded yet.
			//
			// Publishing a test pattern here is what put those blue/green/red bars on the panel at the
			// title screen, and it also defeated the splash entirely - any push leaves sPixels non-null,
			// so the Java fallback chain could never be reached.
			I_AuxPanelClearPixels();
			AuxCanvasNeedsReadback = false;
			return;
		}

		AuxCanvasNeedsReadback = true;
		return;
	}

	AuxCanvasReadbackPhase(env);
}
