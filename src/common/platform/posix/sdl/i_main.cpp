/*
** i_main.cpp
** System-specific startup code. Eventually calls D_DoomMain.
**
**---------------------------------------------------------------------------
** Copyright 1998-2007 Randy Heit
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
*/

// HEADER FILES ------------------------------------------------------------

#include <SDL.h>
#include <unistd.h>
#include <signal.h>
#include <new>
#include <sys/param.h>
#include <locale.h>
#include <sys/stat.h>
#include <sys/utsname.h>

#include "engineerrors.h"
#include "m_argv.h"
#include "c_console.h"
#include "version.h"
#include "cmdlib.h"
#include "engineerrors.h"
#include "i_system.h"
#include "i_interface.h"
#include "printf.h"

// MACROS ------------------------------------------------------------------

// TYPES -------------------------------------------------------------------

// EXTERNAL FUNCTION PROTOTYPES --------------------------------------------

extern "C" int cc_install_handlers(int, char**, int, int*, const char*, int(*)(char*, char*));

#ifdef __APPLE__
void Mac_I_FatalError(const char* errortext);
#endif

#ifdef __linux__
void Linux_I_FatalError(const char* errortext);
#endif

// PUBLIC FUNCTION PROTOTYPES ----------------------------------------------
int GameMain();

// PRIVATE FUNCTION PROTOTYPES ---------------------------------------------

// EXTERNAL DATA DECLARATIONS ----------------------------------------------

// PUBLIC DATA DEFINITIONS -------------------------------------------------
FString sys_ostype;

// The command line arguments.
FArgs *Args;

// PRIVATE DATA DEFINITIONS ------------------------------------------------


// CODE --------------------------------------------------------------------



static int GetCrashInfo (char *buffer, char *end)
{
	if (sysCallbacks.CrashInfo) sysCallbacks.CrashInfo(buffer, end - buffer, "\n");
	return strlen(buffer);
}

void I_DetectOS()
{
	FString operatingSystem;

	const char *paths[] = {"/etc/os-release", "/usr/lib/os-release"};

	for (const char *path : paths)
	{
		struct stat dummy;

		if (stat(path, &dummy) != 0)
			continue;

		char cmdline[256];
		snprintf(cmdline, sizeof cmdline, ". %s && echo ${PRETTY_NAME}", path);

		FILE *proc = popen(cmdline, "r");

		if (proc == nullptr)
			continue;

		char distribution[256] = {};
		fread(distribution, sizeof distribution - 1, 1, proc);

		const size_t length = strlen(distribution);

		if (length > 1)
		{
			distribution[length - 1] = '\0';
			operatingSystem = distribution;
		}

		pclose(proc);
		break;
	}

	utsname unameInfo;

	if (uname(&unameInfo) == 0)
	{
		const char* const separator = operatingSystem.Len() > 0 ? ", " : "";
		operatingSystem.AppendFormat("%s%s %s on %s", separator, unameInfo.sysname, unameInfo.release, unameInfo.machine);
		sys_ostype.Format("%s %s on %s", unameInfo.sysname, unameInfo.release, unameInfo.machine);
	}

	if (operatingSystem.Len() > 0)
		Printf("OS: %s\n", operatingSystem.GetChars());
}

void I_StartupJoysticks();

int main (int argc, char **argv)
{
#if !defined (__APPLE__) && !defined (__ANDROID__)
	// Not on Android: this would replace bionic's debuggerd handler (losing the
	// tombstone and the logcat crash dump), and the handler cannot work anyway -
	// it re-execs argv[0], but the game is a .so with no executable to re-run.
	{
		int s[4] = { SIGSEGV, SIGILL, SIGFPE, SIGBUS };
		cc_install_handlers(argc, argv, 4, s, GAMENAMELOWERCASE "-crash.log", GetCrashInfo);
	}
#endif // !__APPLE__ && !__ANDROID__

	printf(GAMENAME" %s - %s - SDL version\nCompiled on %s\n",
		GetVersionString(), GetGitTime(), __DATE__);

	seteuid (getuid ());
	// Set LC_NUMERIC environment variable in case some library decides to
	// clear the setlocale call at least this will be correct.
	// Note that the LANG environment variable is overridden by LC_*
	setenv ("LC_NUMERIC", "C", 1);

	setlocale (LC_ALL, "C");

	if (SDL_Init (0) < 0)
	{
		fprintf (stderr, "Could not initialize SDL:\n%s\n", SDL_GetError());
		return -1;
	}

	printf("\n");

	Args = new FArgs(argc, argv);

#ifdef PROGDIR
	progdir = PROGDIR;
#elif defined(__ANDROID__)
	// SDL's Android entry point passes argv[0] = "app_process", so the realpath
	// dance below would leave progdir as "/" and every $progdir lookup would
	// resolve against the filesystem root. The external files dir is where the
	// game data is expected to be, and needs no runtime permission to read.
	{
		const char *externalPath = SDL_AndroidGetExternalStoragePath();
		progdir = externalPath != nullptr ? externalPath : ".";
		if (progdir.Len() == 0 || progdir[progdir.Len() - 1] != '/')
			progdir += "/";
	}
#else
	char program[PATH_MAX];
	if (realpath (argv[0], program) == NULL)
		strcpy (program, argv[0]);
	char *slash = strrchr (program, '/');
	if (slash != NULL)
	{
		*(slash + 1) = '\0';
		progdir = program;
	}
	else
	{
		progdir = "./";
	}

#ifdef __APPLE__
	// Inside an .app the executable lives in Contents/MacOS, but codesign treats
	// everything in that directory as nested code and refuses to seal the game's
	// .pk3 files there. Non-code belongs in Contents/Resources, so point progdir
	// at it when we are running from a bundle.
	{
		const char *macosSuffix = ".app/Contents/MacOS/";
		const ptrdiff_t suffixLen = (ptrdiff_t)strlen(macosSuffix);
		if (progdir.Len() >= (size_t)suffixLen &&
			strcmp(progdir.GetChars() + progdir.Len() - suffixLen, macosSuffix) == 0)
		{
			progdir.Truncate(progdir.Len() - strlen("MacOS/"));
			progdir += "Resources/";
		}
	}
#endif
#endif

	//I_StartupJoysticks(); @Cockatrice - Moved this to hardware.cpp, because it requires the config file to be created

	const int result = GameMain();

	SDL_Quit();

#ifdef __ANDROID__
	// Terminate the process, do not just return.
	//
	// SDLActivity only calls mSingleton.finish(), which ends the activity but
	// leaves the process alive for Android to reuse. Returning from here therefore
	// leaves every C++ global in its torn-down post-shutdown state; the next
	// launch re-enters SDL_main in that same process and M_LoadDefaults() reads
	// the config back into cvars that no longer hold valid storage, segfaulting in
	// FBaseCVar::SetGenericRep. That is why "Quit Game" followed by reopening
	// crashed, while the launch after the crash worked - the crash was what
	// finally gave us a fresh process.
	//
	// _exit rather than exit: the engine has already run its own shutdown and
	// saved the config, and running atexit handlers over half-destroyed globals is
	// exactly what we are trying to avoid.
	_exit(result);
#endif

	return result;
}
