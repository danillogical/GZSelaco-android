#pragma once

/*
** i_auxdevicereset.h
** The two entry points of the Handhelds page's "Reset Device Choice" button.
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
** BOTH ARE CALLED FROM i_auxdevicepicker.cpp AND NOT FROM d_main.cpp, deliberately. The picker
** already owns the one-shot after M_Init and the once-per-frame poll in D_Display, this button is
** how the player undoes what the picker recorded, and the alternative is two more lines in the one
** upstream file this fork is trying to keep its footprint small in.
*/

// Replace Selaco's "Choose Optimal Settings" item on the Handhelds page with the reset button.
// Called once, after M_Init has parsed every MENUDEF. On any failure their item is left exactly as
// it shipped, having said what the player will not see.
void AuxDeviceResetInitMenu();

// Once per frame. Opens the confirmation prompt when the button has asked for one, and carries out
// the reset when the prompt reports that the player confirmed it. Costs two bool tests on a frame
// where neither is pending, which is every frame. Does not return when the reset goes ahead: it quits.
void AuxDeviceResetFrame();
