#pragma once

/*
** i_auxcodexviewstate.h
** THE VIEW-STATE MANAGER for the AYN Thor's second-screen codex: the presentation-independent state of
** "what the player was looking at", sampled from the live codex (mode 4, i_auxlivecodex.cpp) and applied
** to the standby codex (mode 3, i_auxstandbycodex.cpp).
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
** THE TWO CODEXES ARE NECESSARILY TWO PDAMenu3 INSTANCES, which is the whole reason this file exists.
** The player's is created by the engine when they open the PDA and Destroy()ed by DMenu::Close when they
** shut it, so mode 3 cannot keep drawing it and has to build its own. The DATA behind them is already
** shared - both read Selaco's real datalogs, stats and unlocks natively - so the only thing that does not
** survive the handover is VIEW state, and this is the one place that carries it across.
**
** WHY STATE AND NOT A PIXEL SNAPSHOT, so that this is not "simplified" back into one. Keeping the live
** codex's final frame instead of redrawing was tried and reverted: a frozen frame cannot survive the
** stat rebuild that any secret or unlock triggers (i_auxstandbycodex.cpp's rebuild trigger), cannot
** survive an aux_codex_size change, and can capture a mid-transition.Draw() double exposure. A scroll
** FRACTION survives all three because it is data - it is re-applied to whatever tree exists now, at
** whatever zoom, after whatever rebuild.
**
** THE THREE PIECES, all of them presentation-independent on purpose:
**
**   WHICH APP        the class of the PDAAppWindow the player last switched to.
**   WHICH ENTRY      PDAReaderWindow.currentArea / currentItem (reader.zs:330), an (area, item) pair.
**   HOW FAR DOWN     the mail pane's scroll as a 0..1 fraction, which is what makes it independent of
**                    the font, the zoom, the window size and the length of the document.
**
** ONE MANAGER RATHER THAN THREE AD-HOC CHANNELS. The app class used to be a lone static pair in
** i_auxvmreflect.cpp, described there as "the one deliberate channel between the modes"; the reasoning
** that made it safe is reproduced verbatim below because it is still the argument, and it is now the
** argument for three things rather than one. Bolting two more statics onto the side of the resolver file
** would have started a pattern where every new piece of shared view state is its own hand-rolled channel
** with its own lifetime story. There is one owner instead, and it is neither mode.
**
** AND THE SYMBOLS ARE RESOLVED ONCE, HERE, FOR BOTH DIRECTIONS - which is the other thing a manager buys.
** Mode 4 READS currentArea/currentItem and the scroll; mode 3 CALLS openEntry and writes the scroll back.
** Both need the same class chain proved, so proving it twice in two files could only ever drift.
**
** SAMPLING IS READ-ONLY, AND STRUCTURALLY SO. The live codex is the player's own menu, so SampleAppWindow
** writes nothing to it and calls no script at all: the entry is two int field loads and the scroll is
** three double field loads and a division - getNormalizedValue's own arithmetic (slider.zs:285-287)
** performed in C++. Calling the getter instead would be a VMCall on a `virtual` method from the per-frame
** path, which is both dearer and a thing that could in principle write.
**
** APPLYING CALLS SCRIPT AND THEREFORE NEEDS THE CALLER'S FProjectionScope. ApplyToWindow reaches
** PDAReaderWindow.openEntry, and mode 3 is a projection nobody asked for: the standby codex must not
** record anything against the player. The caller owns the scope and the try/catch for exactly the reason
** StandbyRelayout's caller does, and every mode-3 call site is already inside both. Mode 4 deliberately
** has NO projection scope and must not grow one - but mode 4 only ever samples, so it never calls in here
** in a direction that would need one.
**
** FAILS SOFT, EVERY PATH. A missing field, a wrong type, an unexpected subclass or a VM abort costs one
** yellow line and the feature, never the panel: with nothing applied the standby codex behaves exactly as
** it did before this existed, which is to say it opens the document at the top. Nothing in here may ever
** latch the standby codex off.
*/

class DObject;
class PClass;

namespace AuxCodexView
{

// ---------------------------------------------------------------------------------------------
// SAMPLING - mode 4 only, read-only, on every frame the player's own PDA is open.
// ---------------------------------------------------------------------------------------------
//
// SAMPLED EVERY FRAME RATHER THAN ON THE CLOSE EDGE, and that is not caution. M_ClearMenus destroys the
// menu when it closes (menu.cpp:946), so by the time any "is the PDA open" predicate goes false the
// instance may already be euthanized - and reading a field off it then is reading freed memory to answer a
// question we could have answered a frame earlier for free. It also means there is no edge to miss: a kill
// mid-session, or the player alt-tabbing away, loses nothing that was already sampled.
//
// appWindow is PDAMenu3.currentAppWindow, which mode 4 has already proved is a live PDAAppWindow. Which of
// the three pieces this call updates depends on what the window IS: the app class always, the entry and the
// scroll only when it is the reader. A window that is not the reader therefore leaves the last reading
// position standing, which is correct - switching to OBJECTIVES is not a statement about the datalog.
void SampleAppWindow(DObject *appWindow);

// ---------------------------------------------------------------------------------------------
// WHAT MODE 3 ASKS FOR.
// ---------------------------------------------------------------------------------------------

// The class of the app the player last switched to in their OWN PDA, or null if they have not opened it
// this session. Read by StandbyWantedIndex (i_auxstandbycodex.cpp), which falls through to aux_standby_app
// on null.
//
// HARMLESS IN BOTH DIRECTIONS, which is what made this safe as a bare static and still does: only a PClass*
// already proved to be a PDAAppWindow is ever stored, PClass objects are never freed inside a session, and a
// mode-4 failure simply leaves it null - which reads as "the player has not opened their PDA yet". Never
// cleared, because "the last app this session" is exactly what it means.
PClass *WantedAppClass();

// Bumped whenever a sample CHANGES the entry or the scroll, and never otherwise; 0 means nothing has been
// sampled yet. This is the apply trigger, and it has to be a generation rather than "the app changed"
// because the case the feature exists for does not change the app: the player opens the codex on the app the
// panel is already showing, scrolls down a datalog, and closes it.
//
// NEVER 0 AFTER THE FIRST SAMPLE, wrap included, so the caller's "nothing to apply" test stays a single
// compare against 0.
unsigned Generation();

// ---------------------------------------------------------------------------------------------
// APPLYING - mode 3 only, inside the caller's FProjectionScope and try/catch.
// ---------------------------------------------------------------------------------------------
//
// window is the app window the standby codex is currently showing. Returns true if anything was installed,
// which is the caller's cue to request a redraw - without one the restored position would sit in the canvas
// unread and the panel would keep showing the old image.
//
// Answers false rather than complaining when the shown window is not the reader: mode 3 asks on every
// generation change and the player may well have closed their PDA on STATS, which is not a failure.
//
// Logs exactly once per apply, naming what was restored, because "the panel did not keep my place" and "the
// panel restored a place that was already the top" are indistinguishable on a device otherwise.
bool ApplyToWindow(DObject *window);

}   // namespace AuxCodexView

// Forget every resolve and every PClass* this file holds, for the restart teardown. See
// I_AuxForgetScriptState in i_auxvmreflect.cpp for why it exists, when it runs, and why every one of these
// must remain free of VM work.
void I_AuxCodexViewStateForgetScriptState();
