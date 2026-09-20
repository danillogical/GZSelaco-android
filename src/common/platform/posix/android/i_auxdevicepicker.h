#pragma once

/*
** i_auxdevicepicker.h
** The recorded device choice, and the two halves of the Selaco first-run bridge, for anything that
** needs to read or change them from outside i_auxdevicepicker.cpp.
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
** THE CHOICE IS READ AND WRITTEN THROUGH THESE FOUR FUNCTIONS AND NEVER THROUGH THE CVAR, and that is
** the point of the header: `aux_device` has three reserved values as well as a profile name (see
** i_auxdevicepicker.cpp), so a caller that wrote the cvar directly would be choosing one of them by
** accident. AuxDeviceChoiceClear is the only way to get back to "ask me again" and it deliberately
** takes Selaco's dialogs with it, because those two facts have to move together.
*/

// The recorded choice, as the profile file stem AuxProfileApply takes. Never null; empty means the
// player has not been asked yet. May also be one of the reserved values, which is why the two
// predicates below exist rather than callers comparing strings.
const char *AuxDeviceChoice();

// True once this install has an answer: either a device profile stem, or the recorded non-choice an
// install that predates the picker is seeded with. False while nothing has been asked yet AND false while
// a picker is still owed, which together are exactly the states in which the picker should be offered.
bool AuxDeviceChoiceIsSet();

// Record a device. The caller is expected to have applied the profile FIRST: this writes nothing but
// the choice, and the ordering is the whole safety argument (see i_auxdevicepicker.cpp).
void AuxDeviceChoiceSet(const char *profileStem);

// Forget the device and hand Selaco's own first-run dialogs back, which is the only correct way to
// undo a choice: leaving the choice cleared while their dialogs stay suppressed is precisely the
// state CLAUDE.md's three first-run bugs come from. Returns false if their cvars could not be
// reached, having said which.
bool AuxDeviceChoiceClear();
