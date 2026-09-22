/*
** i_specialpaths.cpp
** Gets special system folders where data should be stored. (Android version)
**
**---------------------------------------------------------------------------
** Copyright 2013-2016 Randy Heit
** Copyright 2016 Christoph Oelckers
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
** An Android app has no $HOME it may write to, so none of the Unix paths apply.
** Everything is rebased onto the two directories SDL hands us:
**
**   internal  /data/data/<pkg>/files
**             Private to the app and wiped on uninstall. Config, saves and the
**             node cache live here.
**
**   external  /sdcard/Android/data/<pkg>/files
**             Still app-scoped, but visible over MTP and adb, so the user can
**             drop game data in and pull screenshots out. Since Android 4.4 an
**             app needs NO runtime permission for its own external files dir,
**             which is why the game data goes here rather than somewhere that
**             would drag in the scoped-storage permission dance.
*/

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>		// access()

#include <SDL.h>

#include "i_system.h"
#include "cmdlib.h"
#include "printf.h"
#include "engineerrors.h"

#include "version.h"	// for GAMENAME

//===========================================================================
//
// Android storage roots
//
// Both are owned by SDL and stay valid for the life of the process. They can
// be null very early in startup or if the activity is not up yet, in which
// case we fall back to a relative path rather than crashing - callers that
// matter run well after SDL_Init.
//
//===========================================================================

static FString AndroidInternalPath()
{
	const char *path = SDL_AndroidGetInternalStoragePath();
	FString result = path != nullptr ? path : ".";
	if (result.Len() == 0 || result[result.Len() - 1] != '/')
		result += "/";
	return result;
}

static FString AndroidExternalPath()
{
	// External storage can genuinely be unavailable (ejected, or shared over
	// USB), so fall back to internal rather than handing out a broken path.
	const char *path = SDL_AndroidGetExternalStoragePath();
	if (path == nullptr)
		return AndroidInternalPath();

	FString result = path;
	if (result.Len() == 0 || result[result.Len() - 1] != '/')
		result += "/";
	return result;
}

// The PUBLIC game-data folder, if we can write to it.
//
// This is the only location that survives an uninstall. Both app-scoped dirs - internal
// /data/data/<pkg>/files and external /sdcard/Android/data/<pkg>/files - are deleted with the app,
// so saves kept in either are destroyed by an uninstall, by "clear storage", or by the
// INSTALL_FAILED_UPDATE_INCOMPATIBLE that any signing-key change forces you to resolve that way.
//
// Writability is tested rather than assumed, because the developer workflow adb-pushes the ipk3 into
// the app-private dir, which makes hasGameData() true, which means SelacoActivity never asks for
// MANAGE_EXTERNAL_STORAGE. A normal install cannot be in that state - the ipk3 cannot reach the
// private dir without adb, so the permission prompt fires until it is granted - but a dev device can.
//
// Same candidate list and same probe-by-trying approach as the crash log (i_crashlog.cpp).
static FString AndroidPublicPath(const char *subdir)
{
	static const char *const candidates[] = {
		"/sdcard/Selaco/",
		"/storage/emulated/0/Selaco/",
		nullptr
	};

	for (int i = 0; candidates[i] != nullptr; i++)
	{
		FString dir = candidates[i];
		dir += subdir;
		CreatePath(dir.GetChars());          // void; success is decided by the access() test below
		if (access(dir.GetChars(), W_OK) == 0)
			return dir;
	}
	return FString();
}

FString GetUserFile (const char *file)
{
	FString path = AndroidInternalPath();
	CreatePath(path.GetChars());
	path += file;
	return path;
}

//===========================================================================
//
// M_GetAppDataPath													Android
//
// Returns the path for the AppData folder.
//
//===========================================================================

FString M_GetAppDataPath(bool create)
{
	FString path = AndroidInternalPath() + "appdata";
	if (create)
	{
		CreatePath(path.GetChars());
	}
	return path;
}

//===========================================================================
//
// M_GetCachePath													Android
//
// Returns the path for cache GL nodes.
//
//===========================================================================

FString M_GetCachePath(bool create)
{
	FString path = AndroidInternalPath() + "cache";
	if (create)
	{
		CreatePath(path.GetChars());
	}
	return path;
}

//===========================================================================
//
// M_GetAutoexecPath												Android
//
// Returns the expected location of autoexec.cfg.
//
//===========================================================================

FString M_GetAutoexecPath()
{
	// External, not internal. Unlike the config - which is private engine state and
	// belongs in internal storage - autoexec.cfg is authored by the user, so it has to
	// live somewhere they can put a file. This is also $PROGDIR (see i_main.cpp) and is
	// where SelacoActivity.extractAssets() drops the shipped default.
	//
	// Returning the internal path here meant a freshly generated config recorded
	// [Selaco.AutoExec] Path=/data/data/<pkg>/files/autoexec.cfg, which nothing can
	// write to without adb and a debug build - so the autoexec silently never ran on a
	// clean install. It only appeared to work on configs generated before this was
	// noticed, because those had already recorded a different path.
	return AndroidExternalPath() + "autoexec.cfg";
}

//===========================================================================
//
// M_GetConfigPath													Android
//
// Returns the path to the config file.
//
//===========================================================================

FString M_GetConfigPath(bool for_reading)
{
	return GetUserFile(GAMENAMELOWERCASE ".ini");
}

//===========================================================================
//
// M_GetScreenshotsPath												Android
//
// Returns the path to the default screenshots directory. External, so the
// user can actually get at the results.
//
//===========================================================================

FString M_GetScreenshotsPath()
{
	return AndroidExternalPath() + "screenshots/";
}

//===========================================================================
//
// M_GetSavegamesPath												Android
//
// Returns the path to the default save games directory.
//
//===========================================================================

FString M_GetSavegamesPath()
{
	// Public folder when it is writable, so an uninstall or a reinstall does not take the player's
	// progress with it - see AndroidPublicPath. This is the same folder the user already put
	// Selaco.ipk3 in, so it needs no explaining and they can copy saves off the device.
	//
	// Internal is the fallback for the adb developer workflow only. Note M_GetSavegamesPaths keeps
	// internal in the SEARCH list either way, so existing saves are still found and nothing has to be
	// migrated.
	//
	// Resolved once. There are five call sites (m_misc.cpp, savegamemanager.cpp) and the save menu hits
	// them per redraw, so probing would mean an mkdir plus an access() on every one. Caching is safe
	// because the answer cannot change mid-session: SelacoActivity gates startup on the permission
	// already being granted, so it is settled before the engine runs.
	static FString cached;
	static bool resolved = false;
	if (!resolved)
	{
		resolved = true;
		cached = AndroidPublicPath("savegames/");
		if (cached.IsEmpty())
		{
			// Said out loud, because the fallback is the one location an uninstall or "clear storage"
			// destroys. Silently, the player and the log both see a working save system right up until
			// the saves are gone.
			cached = AndroidInternalPath() + "savegames/";
			Printf(TEXTCOLOR_YELLOW "Savegames: no writable public folder, using app-internal storage "
				"(%s) - these saves are DELETED by an uninstall or Clear Storage\n", cached.GetChars());
		}
	}
	return cached;
}

//===========================================================================
//
// M_GetSavegamesPaths												Android
//
// Returns all paths where savegames might be located.
//
//===========================================================================

int M_GetSavegamesPaths(TArray<FString>& outputAr)
{
	// Write path first, then every location a save could already be sitting in. G_BuildSaveNames
	// walks all of them, so moving the write path to public storage does not orphan saves written
	// before that change - no migration step, no copying.
	//
	// Note the app-external entry used to be justified as "so they can be moved on and off the device
	// without root". That has not been true since Android 11: the Files app and MTP both refuse to
	// enter /sdcard/Android/data. It is kept only because saves may already be there.
	const FString candidates[] = {
		M_GetSavegamesPath(),
		AndroidInternalPath() + "savegames/",
		AndroidExternalPath() + "savegames/",
	};

	int cnt = 0;
	for (const FString &path : candidates)
	{
		if (path.IsEmpty())
			continue;

		bool seen = false;
		for (unsigned int i = 0; i < outputAr.Size(); i++)
			if (outputAr[i].CompareNoCase(path.GetChars()) == 0) { seen = true; break; }

		if (!seen)
		{
			outputAr.Push(path);
			cnt++;
		}
	}

	return cnt;
}

//===========================================================================
//
// M_GetDocumentsPath												Android
//
// Returns the path to the default documents directory. This is where the
// user is expected to place the game data.
//
//===========================================================================

FString M_GetDocumentsPath()
{
	return AndroidExternalPath();
}

//===========================================================================
//
// M_GetDemoPath													Android
//
// Returns the path to the default demo directory.
//
//===========================================================================

FString M_GetDemoPath()
{
	return M_GetDocumentsPath() + "demo/";
}

//===========================================================================
//
// M_GetNormalizedPath
//
// Normalizes the given path and returns the result.
//
//===========================================================================

FString M_GetNormalizedPath(const char* path)
{
	char *actualpath = realpath(path, NULL);
	if (!actualpath)	// does not exist yet, or is not reachable
		return FString();
	FString fullpath = actualpath;
	free(actualpath);
	return fullpath;
}
