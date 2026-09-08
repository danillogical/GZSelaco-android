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
	return AndroidInternalPath() + "savegames/";
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
	outputAr.Push(M_GetSavegamesPath());
	int cnt = 1;

	// Also offer saves dropped into external storage, so they can be moved on
	// and off the device without root.
	FString externalPath = AndroidExternalPath() + "savegames/";
	if (M_GetSavegamesPath().CompareNoCase(externalPath.GetChars()))
	{
		outputAr.Push(externalPath);
		cnt++;
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
