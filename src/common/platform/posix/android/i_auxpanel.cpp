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
** The engine never learns that a second screen exists. Nothing Vulkan is created, submitted,
** presented or waited on here, and the renderer is bit-identical to a build without this file.
** The panel is an ordinary Android Presentation drawing an ordinary View; the only thing crossing
** the boundary is a few primitives per push.
**
** That is the entire safety argument, and it is why this approach was chosen over a second Vulkan
** surface: an optional screen must not be able to take the main one down, and the cheapest way to
** guarantee that is to give the aux path no renderer resources to share.
**
** MILESTONE SCOPE - this is deliberately the "grey rectangle plus one number" build. It pushes
** gametic and nothing else, because the four things it exists to falsify are all structural:
**   1. a Presentation can be shown on the second display while another app's activity owns it,
**   2. FLAG_NOT_FOCUSABLE really does stop AppActive going false and freezing BOTH screens,
**   3. FindClass works from the game thread,
**   4. what the panel costs the main screen's frame time.
** Reading player or level state would add crash risk that proves none of those, so it is absent
** on purpose. Do not add content here until those four have answers.
*/

#include <atomic>

#include <SDL_system.h>
#include <jni.h>

#include "doomstat.h"
#include "printf.h"

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
static jmethodID AuxPushState = nullptr;
static jmethodID AuxPushCodex = nullptr;

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

	AuxPushState = env->GetStaticMethodID(AuxClass, "pushState", "(I)V");
	if (env->ExceptionCheck() || AuxPushState == nullptr)
	{
		env->ExceptionClear();
		Printf(TEXTCOLOR_YELLOW "AuxPanel: pushState not found, second-screen panel disabled\n");
		AuxBroken = true;
		return false;
	}

	AuxPushCodex = env->GetStaticMethodID(AuxClass, "pushCodex", "(Ljava/lang/String;)V");
	if (env->ExceptionCheck() || AuxPushCodex == nullptr)
	{
		env->ExceptionClear();
		Printf(TEXTCOLOR_YELLOW "AuxPanel: pushCodex not found, second-screen panel disabled\n");
		AuxBroken = true;
		return false;
	}
	return true;
}

// Publish the codex table of contents, or an empty string to clear it.
//
// Called ONLY when the visible set changes - a gate opening, the publish gate flipping, a level
// load - never per frame. That matters for two reasons. This is the engine thread's first JNI
// OBJECT allocation (NewStringUTF), which is its first real interaction with ART's GC, and the
// payload is a few hundred bytes; doing it at frame rate would be both wasteful and a new
// suspension point in the render loop.
//
// Passing an empty string is how the panel is CLEARED, and it is deliberate rather than an
// optimisation: Java holds no codex state of its own, so the last push is always the authority.
// A gate closing pushes "" and the content is gone. Nothing on the Java side can outlive the
// unlock state that justified it, which is the retention leak a design review found in an earlier
// plan for this.
void I_AuxPanelPushCodex(const char *toc)
{
	if (AuxBroken || !AuxLive.load(std::memory_order_relaxed))
		return;

	JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
	if (env == nullptr)
		return;
	if (AuxClass == nullptr && !AuxResolve(env))
		return;

	jstring js = env->NewStringUTF(toc != nullptr ? toc : "");
	if (env->ExceptionCheck() || js == nullptr)
	{
		env->ExceptionClear();
		Printf(TEXTCOLOR_YELLOW "AuxPanel: NewStringUTF failed, codex push abandoned\n");
		return;
	}

	env->CallStaticVoidMethod(AuxClass, AuxPushCodex, js);

	// The engine thread's native frame never returns, so a leaked local ref would accumulate for
	// the life of the process rather than being reclaimed at a frame boundary.
	env->DeleteLocalRef(js);

	if (env->ExceptionCheck())
	{
		env->ExceptionClear();
		Printf(TEXTCOLOR_YELLOW "AuxPanel: pushCodex threw, second-screen panel disabled\n");
		AuxBroken = true;
	}
}

// Called once per frame from D_Display, before screen->BeginFrame().
//
// The early return is the entire cost on a single-screen device: one relaxed atomic load and two
// bool tests, with no JNI, no allocation and no lock. AuxLive is false unless Java has actually
// shown a panel, so a device with one screen never gets past line one.
void I_AuxPanelFrame()
{
	// M1 probe for the codex bridge. Runs regardless of whether a panel is up, because it publishes
	// nothing and its whole purpose is to answer "can C++ read the unlock map" on real hardware.
	// Remove this call once that question is settled.
	{ extern void I_AuxCodexProbe(); I_AuxCodexProbe(); }

	if (AuxBroken || !AuxLive.load(std::memory_order_relaxed))
		return;

	JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
	if (env == nullptr)
	{
		Printf(TEXTCOLOR_YELLOW "AuxPanel: no JNI env on the game thread, panel disabled\n");
		AuxBroken = true;
		return;
	}

	if (AuxClass == nullptr && !AuxResolve(env))
		return;

	// Primitives only - no object arguments, so there is no local reference to leak. That matters
	// here specifically: this thread's native frame never returns, so a leaked local ref would
	// accumulate for the life of the process rather than being reclaimed at a frame boundary.
	env->CallStaticVoidMethod(AuxClass, AuxPushState, (jint)gametic);

	if (env->ExceptionCheck())
	{
		env->ExceptionClear();
		Printf(TEXTCOLOR_YELLOW "AuxPanel: pushState threw, second-screen panel disabled\n");
		AuxBroken = true;
	}
}
