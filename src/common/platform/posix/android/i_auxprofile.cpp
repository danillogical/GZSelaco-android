/*
** i_auxprofile.cpp
** Per-device profiles: one declarative key/value file per handheld, applied on demand.
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
** WHY A DECLARATIVE FILE AND NOT AN EXEC'D CONSOLE SCRIPT.
**
** A profile is a community contribution: the point of the feature is that a PR can add a handheld, or
** retune one, without touching C++. An exec'd .cfg would make that PR arbitrary console commands, with
** no way to say what a profile is ALLOWED to do and no way to define "unspecified". A fixed key set
** makes both well-defined - every key is optional, an absent key leaves the thing it controls exactly
** as the game already had it, and a key nobody recognises is one yellow line rather than a surprise.
** See ProfileSpec for why "absent" means "leave alone" rather than "substitute a value chosen here".
**
** WHY A PRESET IS READ OUT OF SELACO'S OWN OptionValue BLOCK AND NEVER ENUMERATED HERE.
**
** `OptionValue "GFXPresetDeckLow" { 1, "r_shadowQuality"  6, "gl_texture_filter" ... }`
** (Selaco's MENUDEF.zsc) is already parsed by the engine into FOptionValues (menu.h:283) and reachable
** through the global OptionValues map (menu.h:295). Note the layout is INVERTED against a normal option
** list: Pair::Value is the value to set and Pair::Text is the CVAR NAME (ParseOptionValue,
** menudef.cpp:861-882).
**
** Reading that block is the whole safety argument for this file. The alternative - writing the cvar list
** out here, or recovering it from an ini - silently DROPS every cvar sitting at its engine default,
** because an ini only records what differs from the default. That has already happened in this project:
** it dropped seven cvars from a hand-built DeckLow alias and the result was visibly not DeckLow. Reading
** the block is immune to that, and to any rename or retune Selaco makes, because it is the same data
** their own menu applies.
**
** The set itself mirrors OptionMenuItemTooltipPreset.setCV (Selaco's options_items.zs:71-80) exactly:
** an Int-typed cvar is set from int(value) (C truncation, not a round) and EVERYTHING ELSE - Bool
** included - is set from the float. Matching it matters because the two disagree for a Bool: the float
** path lands on ToBool(value.Float != 0), which is what Selaco's own presets rely on. Going through
** SetGenericRep rather than poking the variable is also load-bearing, because that is what fires each
** cvar's own callback (gl_texture_filter clamps and calls SetTextureFilterMode, and so on).
**
** WHY SetSteamdeckPresets IS CALLED BY REFLECTION RATHER THAN REIMPLEMENTED.
**
** It is nine cvar writes (helper.zs:616), three of which are CVAR_USERINFO. Copying them here would be a
** second copy to drift, and this fork has no way to notice when Selaco retunes theirs. Calling their
** static means the profile applies whatever the shipped game means by "Steam Deck", forever.
**
** THIS FILE WRITES NO CVAR_USERINFO CVAR OF ITS OWN, and the guard below is the enforcement rather than
** a nicety: ui_scaling and friends are CVAR_USERINFO (Selaco CVARINFO:186), and writing one pushes a
** DEM_UINFCHANGED into the demo/net stream. SetSteamdeckPresets writing them is Selaco's own business
** and is left alone; a preset block gaining one would be skipped with a named warning instead, because a
** profile is not a good enough reason to put an event in the player's savegame. No shipped preset block
** contains a userinfo cvar today, so this never fires on the current game.
**
** NOTHING HERE DECIDES WHEN TO RUN. The two console commands and the two functions in i_auxprofile.h
** are the whole of this file's surface; a scan or an application happens because something asked for
** one. Which device to offer, when to offer it and what to record is i_auxdevicepicker.cpp's job, and
** keeping that out of here is what lets the applier stay a pure "do what this file says" step.
**
** FAIL SOFT, EVERYWHERE, AND LATCH NOTHING. A missing file, an unknown key, an unparseable value or an
** unresolvable preset block is one yellow line and the setting is left alone; the rest of the profile
** still applies. Nothing here can refuse a later attempt, and nothing here touches the second screen's
** own state beyond the one cvar the player could have set from the menu anyway.
*/

#include <dirent.h>
#include <float.h>
#include <stdlib.h>
#include <string.h>

#include <exception>     // std::exception, the base a VM abort is caught through

#include "c_cvars.h"
#include "c_dispatch.h"
#include "cmdlib.h"      // progdir, which is where the extracted assets land
#include "dobject.h"
#include "dobjtype.h"
#include "files.h"
#include "menu.h"        // OptionValues, the map the preset blocks live in
#include "printf.h"
#include "vm.h"
#include "zstring.h"

#include "i_auxprofile.h"
#include "i_auxvmreflect.h"

// Same as the two mode files: the reflection helpers live in AuxView because i_auxcanvas.cpp would
// otherwise redefine three of its names at file scope.
using namespace AuxView;

// Unique to this revision, printed by both CCMDs and therefore present in the stripped library. The
// point is to be able to prove a change is actually IN the APK before reading anything into device
// behaviour - `strings libSelaco.so | grep AUXPROFILE_BUILD` - because every exit code in the Android
// packaging chain reports success whether or not it rebuilt.
static const char *const AuxProfileBuild = "AUXPROFILE_BUILD_20260918_M7A_R1";

const char *AuxProfileBuildId()
{
	return AuxProfileBuild;
}

// Where the extracted assets land. i_main.cpp:191 sets progdir to the app's external files dir on
// Android, which is the same directory SelacoActivity.extractAssets writes to and where the user drops
// their game data - so the profiles are found next to the pk3s rather than at a second hardcoded path.
static const char *const ProfileSubdir = "profiles";

// The prefixes a profile's preset names are completed against. A profile says `gfx_preset DeckHigh` and
// this makes it GFXPresetDeckHigh, so a contributor writes the name they see in Selaco's own menu rather
// than an internal identifier, and cannot name a block that is not a preset.
static const char *const GfxPresetPrefix = "GFXPreset";
static const char *const VisibilityPresetPrefix = "VisibilityPreset";

namespace
{

// Every key, and what an ABSENT key means.
//
// ONLY ONE KEY HAS A DEFAULT VALUE, and that is the whole design: for the three keys that write a cvar,
// an absent key means LEAVE THAT CVAR ALONE - the game's own default stands, untouched. It does not mean
// "substitute a number chosen here".
//
// That was not the first shape of this and the reason for the change is worth keeping: a profile-level
// default shadows the engine's. second_screen defaulting to 0 inverted aux_panel's own `true`, so a
// profile that simply did not mention the second screen silently turned it off; max_fps defaulting to 30
// - a figure that is only right for the AYN Thor's 60 Hz panel - would have capped a Steam Deck at 30
// when the dialog it replaces leaves it at 200. Both were invisible in the profile that suffered them,
// because the offending value was not written down anywhere.
//
// Leaving the cvar alone has three properties the invented default did not: there is no second set of
// numbers to keep in sync with the engine, "unspecified" reads the way a reader expects, and ADDING A KEY
// LATER CANNOT RETROACTIVELY CHANGE WHAT AN EXISTING PROFILE DOES.
//
// steamdeck is the deliberate exception and keeps its default of 1: every handheld wants it, and CLAUDE.md
// records three separate bugs (walking instead of running, 20x view bob, a stale gamepad layout) caused by
// SetSteamdeckPresets() not running.
//
// The `*Set` flags are therefore load-bearing rather than only cosmetic: for the three cvar keys they are
// what decides whether anything is written at all. They also let the CCMD report set-versus-untouched per
// key, which on a device is the only way to tell a profile that was read from one that silently was not.
struct ProfileSpec
{
	FString name;
	FString gfxPreset;
	FString visibility;
	bool steamdeck = true;

	// No meaningful initialiser: these are only ever read when the matching *Set flag is true. Given a
	// value anyway so that a future read without the guard is at least deterministic.
	bool secondScreen = false;
	double screenSize = 0.0;
	double maxFps = 0.0;

	bool nameSet = false;
	bool gfxPresetSet = false;
	bool visibilitySet = false;
	bool steamdeckSet = false;
	bool secondScreenSet = false;
	bool screenSizeSet = false;
	bool maxFpsSet = false;
};

// Running totals for the one summary line, so the report is a count rather than a judgement.
struct ApplyTally
{
	int cvarsWritten = 0;
	int problems = 0;
};

}   // namespace

static bool IsSpace(char c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

// Strips a trailing comment and surrounding whitespace in place. Both `//` and `#` are accepted because
// a contributor coming from a GZDoom cfg will reach for one and a contributor coming from anything else
// will reach for the other; neither can appear inside a value this format accepts.
static void StripCommentAndTrim(FString &line)
{
	const char *s = line.GetChars();
	size_t cut = line.Len();
	for (size_t i = 0; i < line.Len(); i++)
	{
		if (s[i] == '#' || (s[i] == '/' && i + 1 < line.Len() && s[i + 1] == '/'))
		{
			cut = i;
			break;
		}
	}
	line.Truncate(cut);
	line.StripLeftRight();
}

// A boolean the way a config file writer expects one, rather than only 0/1.
static bool ParseBoolValue(const char *value, bool &out)
{
	if (!stricmp(value, "1") || !stricmp(value, "true") || !stricmp(value, "yes") || !stricmp(value, "on"))
	{
		out = true;
		return true;
	}
	if (!stricmp(value, "0") || !stricmp(value, "false") || !stricmp(value, "no") || !stricmp(value, "off"))
	{
		out = false;
		return true;
	}
	return false;
}

// Rejects trailing junk rather than accepting a prefix: "1.75x" is a contributor's mistake and should
// say so, not silently become 1.75.
static bool ParseDoubleValue(const char *value, double &out)
{
	char *end = nullptr;
	const double parsed = strtod(value, &end);
	if (end == value)
		return false;
	while (end != nullptr && *end != '\0' && IsSpace(*end))
		end++;
	if (end == nullptr || *end != '\0')
		return false;
	out = parsed;
	return true;
}

// Selaco's own setCV (options_items.zs:71-80), in C++ and deliberately nothing more than it. The Int
// branch truncates exactly as ZScript's int(double) does; every other real type - Bool included - goes
// through the float, which is the behaviour their Bool preset entries depend on.
static void SetCVarAsSelacoWould(FBaseCVar *cvar, double value)
{
	UCVarValue val;
	if (cvar->GetRealType() == CVAR_Int)
	{
		val.Int = (int)value;
		cvar->SetGenericRep(val, CVAR_Int);
	}
	else
	{
		val.Float = (float)value;
		cvar->SetGenericRep(val, CVAR_Float);
	}
}

// Set one named cvar, reporting whether it was actually written. Shared by the preset path and the three
// explicit keys so a missing cvar reads the same either way.
static bool SetNamedCVar(const char *cvarname, double value, const char *context)
{
	FBaseCVar *cvar = FindCVar(cvarname, nullptr);
	if (cvar == nullptr)
	{
		Printf(TEXTCOLOR_YELLOW "AuxProfile: %s names cvar '%s', which does not exist - skipped\n",
			context, cvarname);
		return false;
	}

	// See THIS FILE WRITES NO CVAR_USERINFO CVAR OF ITS OWN in the header comment. Never fires on the
	// shipped presets; it is here so that a future preset gaining one cannot quietly start putting a
	// DEM_UINFCHANGED in the player's demo and savegame stream on our behalf.
	if (cvar->GetFlags() & CVAR_USERINFO)
	{
		Printf(TEXTCOLOR_YELLOW "AuxProfile: %s names userinfo cvar '%s' - skipped, it would enter the net stream\n",
			context, cvarname);
		return false;
	}

	SetCVarAsSelacoWould(cvar, value);
	return true;
}

// Apply one of Selaco's preset blocks by name, reporting how many cvars it actually wrote.
//
// Returns false only when the BLOCK itself could not be used; an individual cvar inside it that no
// longer exists is counted and warned about but does not fail the preset, because applying 33 of 34 is
// better for the player than applying none.
static bool ApplyPresetBlock(const char *prefix, const char *suffix, const char *what,
	FString &outBlockName, int &outApplied, int &outSkipped)
{
	outApplied = 0;
	outSkipped = 0;
	outBlockName.Format("%s%s", prefix, suffix);

	// noCreate: a name the game has never registered means the block does not exist, and looking it up
	// must not be what adds it to the table.
	FName key(outBlockName.GetChars(), true);
	FOptionValues **found = key != NAME_None ? OptionValues.CheckKey(key) : nullptr;
	if (found == nullptr || *found == nullptr)
	{
		Printf(TEXTCOLOR_YELLOW "AuxProfile: %s '%s' does not name a preset (no OptionValue \"%s\") - left alone\n",
			what, suffix, outBlockName.GetChars());
		return false;
	}

	FOptionValues *block = *found;

	// A block parsed from OptionString rather than OptionValue stores DBL_MAX in Value and puts the real
	// payload in TextValue (menudef.cpp:890-913). Its Text is a LOCALISED LABEL, not a cvar name, so
	// walking it would try to set cvars named after menu text. That is what a profile naming a list
	// ("GFXPresets") instead of a preset ("GFXPresetLow") would do, so it is checked rather than assumed.
	if (block->mValues.Size() > 0 && block->mValues[0].Value == DBL_MAX)
	{
		Printf(TEXTCOLOR_YELLOW "AuxProfile: %s '%s' names a list of presets, not a preset - left alone\n",
			what, suffix);
		return false;
	}

	for (unsigned i = 0; i < block->mValues.Size(); i++)
	{
		const FOptionValues::Pair &pair = block->mValues[i];
		if (SetNamedCVar(pair.Text.GetChars(), pair.Value, outBlockName.GetChars()))
			outApplied++;
		else
			outSkipped++;
	}

	return true;
}

// Call UIHelper.SetSteamdeckPresets(), Selaco's own handheld UI/HUD block.
static bool CallSetSteamdeckPresets(FString &outWhy)
{
	// noCreate, and a genuine "not this game" case rather than an error: a non-Selaco game has no
	// UIHelper and a profile asking for this simply cannot have it.
	FName clsName("UIHelper", true);
	PClass *cls = clsName != NAME_None ? PClass::FindClass(clsName) : nullptr;
	if (cls == nullptr)
	{
		outWhy = "no UIHelper class - not Selaco, or renamed";
		return false;
	}

	int regs = 0;
	VMFunction *func = ResolveStaticMethod(cls, "SetSteamdeckPresets", "AuxProfile", &regs);
	if (func == nullptr)
	{
		// ResolveStaticMethod has already printed which check failed.
		outWhy = "SetSteamdeckPresets did not validate";
		return false;
	}

	try
	{
		// Proved to be a zero-argument static, so nothing is passed. The array exists only so that a
		// null params pointer never reaches VMCall.
		VMValue params[1] = {};
		VMCall(func, params, 0, nullptr, 0);
	}
	catch (const std::exception &e)
	{
		// A script abort inside their function must not take the console command down with it.
		outWhy.Format("SetSteamdeckPresets aborted: %s", e.what());
		return false;
	}

	return true;
}

// Parse a profile file into spec. Unknown keys and bad values warn and are skipped; the file is still
// used for everything it got right.
static bool ParseProfileFile(const char *path, ProfileSpec &spec, int &outProblems)
{
	FileReader fr;
	if (!fr.OpenFile(path))
		return false;

	FileSys::FileData data = fr.Read();
	const char *text = (const char *)data.data();
	const size_t len = data.size();

	// Iterated by length rather than as a C string: Read() returns exactly the file's bytes with no
	// guaranteed terminator.
	size_t pos = 0;
	int lineno = 0;
	while (pos <= len)
	{
		size_t end = pos;
		while (end < len && text[end] != '\n')
			end++;

		FString line(text + pos, end - pos);
		lineno++;
		pos = end + 1;

		StripCommentAndTrim(line);
		if (line.Len() == 0)
		{
			if (end >= len)
				break;
			continue;
		}

		// Split on the first run of whitespace: key, then the remainder as the value. A value is never
		// quoted in this format, so a display name may contain spaces without any escaping rules.
		const char *s = line.GetChars();
		size_t split = 0;
		while (split < line.Len() && !IsSpace(s[split]))
			split++;

		FString key(s, split);
		FString value;
		if (split < line.Len())
		{
			value = FString(s + split, line.Len() - split);
			value.StripLeftRight();
		}

		if (value.Len() == 0)
		{
			Printf(TEXTCOLOR_YELLOW "AuxProfile: %s:%d: key '%s' has no value - ignored\n",
				path, lineno, key.GetChars());
			outProblems++;
			if (end >= len)
				break;
			continue;
		}

		// Keys are compared case-insensitively so a contributor's MAX_FPS works as well as max_fps.
		const char *k = key.GetChars();
		const char *v = value.GetChars();

		if (!stricmp(k, "name"))
		{
			spec.name = value;
			spec.nameSet = true;
		}
		else if (!stricmp(k, "gfx_preset"))
		{
			spec.gfxPreset = value;
			spec.gfxPresetSet = true;
		}
		else if (!stricmp(k, "visibility"))
		{
			spec.visibility = value;
			spec.visibilitySet = true;
		}
		else if (!stricmp(k, "steamdeck"))
		{
			if (ParseBoolValue(v, spec.steamdeck)) spec.steamdeckSet = true;
			else
			{
				// The one key that does have a default to fall back to, so it is named.
				Printf(TEXTCOLOR_YELLOW "AuxProfile: %s:%d: steamdeck '%s' is not a boolean - using default %d\n",
					path, lineno, v, (int)spec.steamdeck);
				outProblems++;
			}
		}
		else if (!stricmp(k, "second_screen"))
		{
			// A value that does not parse leaves *Set false, which means the cvar is left alone - the
			// same outcome as omitting the key. Saying so beats implying a substituted number.
			if (ParseBoolValue(v, spec.secondScreen)) spec.secondScreenSet = true;
			else
			{
				Printf(TEXTCOLOR_YELLOW "AuxProfile: %s:%d: second_screen '%s' is not a boolean - aux_panel left alone\n",
					path, lineno, v);
				outProblems++;
			}
		}
		else if (!stricmp(k, "screen_size"))
		{
			if (ParseDoubleValue(v, spec.screenSize)) spec.screenSizeSet = true;
			else
			{
				Printf(TEXTCOLOR_YELLOW "AuxProfile: %s:%d: screen_size '%s' is not a number - aux_codex_size left alone\n",
					path, lineno, v);
				outProblems++;
			}
		}
		else if (!stricmp(k, "max_fps"))
		{
			if (ParseDoubleValue(v, spec.maxFps)) spec.maxFpsSet = true;
			else
			{
				Printf(TEXTCOLOR_YELLOW "AuxProfile: %s:%d: max_fps '%s' is not a number - vid_maxfps left alone\n",
					path, lineno, v);
				outProblems++;
			}
		}
		else
		{
			// Named rather than counted silently: an unknown key is nearly always a typo, and the
			// contributor needs to see which one.
			Printf(TEXTCOLOR_YELLOW "AuxProfile: %s:%d: unknown key '%s' - ignored\n", path, lineno, k);
			outProblems++;
		}

		if (end >= len)
			break;
	}

	return true;
}

// Reject anything that could escape the profiles directory. The argument is a NAME, not a path, and the
// only reason to contain a separator is to reach somewhere it should not.
static bool ProfileNameIsSane(const char *name)
{
	if (name == nullptr || *name == '\0')
		return false;
	if (strchr(name, '/') != nullptr || strchr(name, '\\') != nullptr)
		return false;
	if (strstr(name, "..") != nullptr)
		return false;
	return true;
}

static void BuildProfilePath(const char *name, FString &out)
{
	out.Format("%s%s/%s.cfg", progdir.GetChars(), ProfileSubdir, name);
}

bool AuxProfileApply(const char *name, AuxProfileResult &result)
{
	if (!ProfileNameIsSane(name))
	{
		Printf(TEXTCOLOR_YELLOW "AuxProfile: '%s' is not a profile name - nothing applied\n", name);
		return false;
	}

	FString path;
	BuildProfilePath(name, path);

	// Printed before the parse, so that any warning the parse emits appears underneath the file it
	// came from rather than above it.
	Printf("AuxProfile: file=%s\n", path.GetChars());

	ProfileSpec spec;
	int problems = 0;
	if (!ParseProfileFile(path.GetChars(), spec, problems))
	{
		Printf(TEXTCOLOR_YELLOW "AuxProfile: cannot read %s - nothing applied\n", path.GetChars());
		return false;
	}

	// The display name defaults to the filename, so a profile that omits it still has something the
	// step-2 picker can show.
	if (!spec.nameSet)
		spec.name = name;
	Printf("AuxProfile: name=\"%s\" (%s)\n", spec.name.GetChars(),
		spec.nameSet ? "set" : "unset, using the filename");

	ApplyTally tally;
	tally.problems = problems;

	// ORDER IS THE CONTRACT: both presets first, then Selaco's Steam Deck block, then the three explicit
	// cvars last - so an explicit key in the profile always wins over anything a preset set on its way
	// past. Within that, graphics before visibility because visibility is a deliberate override of two
	// cvars (r_smokedensity, r_particleLifespan) that the graphics presets also set.
	if (spec.gfxPresetSet)
	{
		FString block;
		int applied = 0, skipped = 0;
		if (ApplyPresetBlock(GfxPresetPrefix, spec.gfxPreset.GetChars(), "gfx_preset", block, applied, skipped))
		{
			Printf("AuxProfile: gfx_preset=%s (set) -> %s: %d cvars applied, %d unresolved\n",
				spec.gfxPreset.GetChars(), block.GetChars(), applied, skipped);
			tally.cvarsWritten += applied;
			tally.problems += skipped;
		}
		else
		{
			tally.problems++;
		}
	}
	else
	{
		Printf("AuxProfile: gfx_preset=<unset> -> graphics settings left alone\n");
	}

	if (spec.visibilitySet)
	{
		FString block;
		int applied = 0, skipped = 0;
		if (ApplyPresetBlock(VisibilityPresetPrefix, spec.visibility.GetChars(), "visibility", block, applied, skipped))
		{
			Printf("AuxProfile: visibility=%s (set) -> %s: %d cvars applied, %d unresolved\n",
				spec.visibility.GetChars(), block.GetChars(), applied, skipped);
			tally.cvarsWritten += applied;
			tally.problems += skipped;
		}
		else
		{
			tally.problems++;
		}
	}
	else
	{
		Printf("AuxProfile: visibility=<unset> -> visibility settings left alone\n");
	}

	// Recorded as well as printed, because the picker cannot suppress Selaco's first-run dialogs
	// unless this call is the thing that happened instead of them (see AuxProfileResult).
	result.steamdeckWanted = spec.steamdeck;
	if (spec.steamdeck)
	{
		FString why;
		if (CallSetSteamdeckPresets(why))
		{
			result.steamdeckApplied = true;
			Printf("AuxProfile: steamdeck=1 (%s) -> UIHelper.SetSteamdeckPresets() called\n",
				spec.steamdeckSet ? "set" : "default");
		}
		else
		{
			Printf(TEXTCOLOR_YELLOW "AuxProfile: steamdeck=1 (%s) -> not applied: %s\n",
				spec.steamdeckSet ? "set" : "default", why.GetChars());
			tally.problems++;
		}
	}
	else
	{
		Printf("AuxProfile: steamdeck=0 (%s) -> SetSteamdeckPresets() not called\n",
			spec.steamdeckSet ? "set" : "default");
	}

	// The three cvar keys. Each is written ONLY if the profile actually asked for it - an absent key
	// leaves the cvar at whatever the game already had, which is the point of the design (see ProfileSpec).
	// Reported with the cvar they wrote rather than only the key, so the output can be checked against
	// `aux_panel` and friends directly.
	if (spec.secondScreenSet)
	{
		if (SetNamedCVar("aux_panel", spec.secondScreen ? 1.0 : 0.0, "second_screen"))
		{
			Printf("AuxProfile: second_screen=%d (set) -> aux_panel=%d\n",
				(int)spec.secondScreen, (int)spec.secondScreen);
			tally.cvarsWritten++;
		}
		else
		{
			tally.problems++;
		}
	}
	else
	{
		Printf("AuxProfile: second_screen=<unset> -> aux_panel left alone\n");
	}

	if (spec.screenSizeSet)
	{
		if (SetNamedCVar("aux_codex_size", spec.screenSize, "screen_size"))
		{
			Printf("AuxProfile: screen_size=%g (set) -> aux_codex_size=%g\n",
				spec.screenSize, spec.screenSize);
			tally.cvarsWritten++;
		}
		else
		{
			tally.problems++;
		}
	}
	else
	{
		Printf("AuxProfile: screen_size=<unset> -> aux_codex_size left alone\n");
	}

	if (spec.maxFpsSet)
	{
		if (SetNamedCVar("vid_maxfps", spec.maxFps, "max_fps"))
		{
			Printf("AuxProfile: max_fps=%g (set) -> vid_maxfps=%g\n", spec.maxFps, spec.maxFps);
			tally.cvarsWritten++;
		}
		else
		{
			tally.problems++;
		}
	}
	else
	{
		Printf("AuxProfile: max_fps=<unset> -> vid_maxfps left alone\n");
	}

	const int keysSet = (int)spec.nameSet + (int)spec.gfxPresetSet + (int)spec.visibilitySet
		+ (int)spec.steamdeckSet + (int)spec.secondScreenSet + (int)spec.screenSizeSet + (int)spec.maxFpsSet;

	// "unset" rather than "defaulted", because for five of the seven keys unset means nothing was
	// substituted at all.
	Printf("AuxProfile: applied %s - %d of 7 keys set, %d unset, %d cvars written, %d problems\n",
		name, keysSet, 7 - keysSet, tally.cvarsWritten, tally.problems);

	result.display = spec.name;
	result.keysSet = keysSet;
	result.cvarsWritten = tally.cvarsWritten;
	result.problems = tally.problems;
	return true;
}

// Three lines around AuxProfileApply. The usage text stays here rather than in the applier because
// only a typed command can be used wrongly this way.
CCMD(aux_applyprofile)
{
	Printf("AuxProfile: build=%s\n", AuxProfileBuild);

	if (argv.argc() < 2)
	{
		Printf("Usage: aux_applyprofile <name>   (see aux_listprofiles)\n");
		return;
	}

	AuxProfileResult result;
	AuxProfileApply(argv[1], result);
}

bool AuxProfileScan(TArray<AuxProfileEntry> &out)
{
	out.Clear();

	FString dirpath;
	dirpath.Format("%s%s", progdir.GetChars(), ProfileSubdir);

	DIR *dir = opendir(dirpath.GetChars());
	if (dir == nullptr)
	{
		Printf(TEXTCOLOR_YELLOW "AuxProfile: cannot read %s - no profiles were extracted\n", dirpath.GetChars());
		return false;
	}

	Printf("AuxProfile: scanning %s/\n", dirpath.GetChars());

	// readdir rather than an engine file-list helper because the profiles live in the real filesystem
	// next to the pk3s, not in the mounted virtual one, and this file is Android-only so POSIX is given.
	for (const struct dirent *entry = readdir(dir); entry != nullptr; entry = readdir(dir))
	{
		const size_t len = strlen(entry->d_name);
		if (len < 5 || stricmp(entry->d_name + len - 4, ".cfg") != 0)
			continue;

		AuxProfileEntry found;
		found.stem = FString(entry->d_name, len - 4);

		// The display name is read back from the file so the listing shows what the picker would show,
		// rather than only what the file is called. Note that this re-parses, so a malformed profile
		// prints its warnings here too - which is wanted: listing is when you want to find out.
		FString path;
		BuildProfilePath(found.stem.GetChars(), path);
		ProfileSpec spec;
		int ignored = 0;
		found.display = found.stem;
		if (ParseProfileFile(path.GetChars(), spec, ignored) && spec.nameSet)
			found.display = spec.name;

		out.Push(found);
	}
	closedir(dir);

	// THE ONE PLACE THE PICKER'S ORDER IS DECIDED, and it deliberately decides nothing: the entries stay
	// in the order readdir returned them, which is the filesystem's and not meaningfully stable. Sorting
	// alphabetically, or floating the profile that matches this device to the top, both belong here and
	// nowhere else - the picker builds its items straight off this array and `aux_listprofiles` prints
	// the same one, so whatever this function does, the two agree.
	return true;
}

CCMD(aux_listprofiles)
{
	Printf("AuxProfile: build=%s\n", AuxProfileBuild);

	TArray<AuxProfileEntry> profiles;
	if (!AuxProfileScan(profiles))
		return;

	for (const AuxProfileEntry &entry : profiles)
	{
		Printf("AuxProfile:   %-24s name=\"%s\"\n", entry.stem.GetChars(), entry.display.GetChars());
	}

	Printf("AuxProfile: %d profile%s\n", (int)profiles.Size(), profiles.Size() == 1 ? "" : "s");
}

