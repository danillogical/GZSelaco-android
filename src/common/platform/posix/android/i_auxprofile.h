#pragma once

/*
** i_auxprofile.h
** The per-device profile applier (i_auxprofile.cpp), reachable from the first-launch device
** picker (i_auxdevicepicker.cpp) as well as from its own two console commands.
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
** THE TWO FUNCTIONS BELOW ARE THE CCMDs' OWN BODIES, LIFTED OUT UNCHANGED. `aux_listprofiles`
** and `aux_applyprofile` are now three lines each around them, so the picker and the console
** command cannot drift apart: the same scan decides what the picker offers and what the listing
** prints, and the same applier does the work whichever one asked for it.
**
** BOTH STILL DO THEIR OWN PRINTING, and the caller is expected to let them. Their per-key and
** per-preset lines are how a device run is read back (see i_auxprofile.cpp's header), so a caller
** that wanted them silent would be throwing away the only evidence there is.
*/

#include "tarray.h"
#include "zstring.h"

// One discovered profile. Two strings rather than one because they are not the same thing: `stem`
// is the filename AuxProfileApply takes and the value the device choice is recorded as, while
// `display` is the profile's own `name` key - which may contain spaces, and falls back to the stem.
struct AuxProfileEntry
{
	FString stem;
	FString display;
};

// What one application actually did, so a caller can judge the result rather than only whether the
// file opened.
//
// steamdeckWanted/steamdeckApplied ARE THE ONE FIELD PAIR THAT EXISTS FOR THE PICKER, and they are
// the reason this is a struct and not a bool: suppressing Selaco's first-run dialogs is only safe
// because UIHelper.SetSteamdeckPresets() ran instead, so the picker has to be able to see that it
// did. CLAUDE.md records three bugs from that call not happening.
struct AuxProfileResult
{
	FString display;
	int keysSet = 0;
	int cvarsWritten = 0;
	int problems = 0;
	bool steamdeckWanted = false;
	bool steamdeckApplied = false;
};

// Every *.cfg in the profiles directory, in the order the picker should offer them. False means the
// directory could not be read at all, which it has already said; an empty list is a successful scan
// of an empty directory.
bool AuxProfileScan(TArray<AuxProfileEntry> &out);

// Apply one profile by file stem. False means nothing at all was applied - a name that is not a
// name, or a file that could not be read. True means the file was used, which does NOT mean
// everything in it worked: read `result` for that.
bool AuxProfileApply(const char *name, AuxProfileResult &result);

// The profile subsystem's build sentinel, so the picker's own log can name the applier revision it
// is actually talking to rather than the one its source was written against.
const char *AuxProfileBuildId();
