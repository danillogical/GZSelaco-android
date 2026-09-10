/*
** i_crashlog.cpp
**
** Persistent crash logging for Android.
**
** Why this exists. Android's own crash record is a tombstone under /data/tombstones, which is
** root-only, plus a logcat dump that lives in a ring buffer. Both are effectively unreachable for
** a beta tester, and the logcat copy is easy to lose - during this project's own development two
** crashes were rendered undiagnosable because the buffer had been cleared or had rotated. A
** tester cannot be asked to run adb.
**
** So this writes a durable log to the app's EXTERNAL files dir (progdir), which is the same place
** the game data lives and is readable over USB without adb or root.
**
** It CHAINS rather than replaces. Installing a handler on Android normally costs you bionic's
** debuggerd handler, and with it the tombstone and the logcat backtrace - which is exactly why
** i_main.cpp does not install the generic crashcatcher here. This handler writes its own record
** and then restores the previous handler and re-raises, so debuggerd still runs and produces the
** tombstone as well. You get both, not one.
**
** Everything in the handler path is async-signal-safe: write(2) only, no malloc, no stdio, and
** integers formatted by hand. dladdr is not formally on the safe list but is the standard way to
** resolve a frame in a crash handler and does not allocate for this use.
*/

#include <signal.h>
#include <unwind.h>
#include <dlfcn.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <android/log.h>

#include "cmdlib.h"
#include "version.h"
#include "printf.h"

// Snapshotted at install time so the handler never touches an FString.
static char crashLogPath[512];
static bool crashLogReady = false;

// Free-form context the engine can update as it runs, e.g. the current map. Plain storage so it
// is safe to read from a signal handler.
static char crashContext[256];

enum { kNumHandled = 5 };
static const int kHandledSignals[kNumHandled] = { SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL };
static struct sigaction oldActions[kNumHandled];

static void wstr(int fd, const char *s)
{
	if (s) (void)!write(fd, s, strlen(s));
}

static void wdec(int fd, unsigned long long v)
{
	char buf[24];
	int i = (int)sizeof(buf);
	buf[--i] = '\0';
	if (v == 0) buf[--i] = '0';
	while (v && i > 0) { buf[--i] = (char)('0' + (v % 10)); v /= 10; }
	wstr(fd, buf + i);
}

static void whex(int fd, unsigned long long v)
{
	static const char *digits = "0123456789abcdef";
	char buf[20];
	int i = (int)sizeof(buf);
	buf[--i] = '\0';
	if (v == 0) buf[--i] = '0';
	while (v && i > 0) { buf[--i] = digits[v & 0xf]; v >>= 4; }
	wstr(fd, "0x");
	wstr(fd, buf + i);
}

static void w2(int fd, unsigned v)   // zero-padded two digits
{
	char b[2] = { (char)('0' + (v / 10) % 10), (char)('0' + v % 10) };
	(void)!write(fd, b, 2);
}

// Human-readable UTC, so a crash is identifiable without converting epoch seconds.
//
// UTC deliberately, not local time: localtime_r cannot be called from a signal handler (it may
// call tzset, allocate, and read /etc/localtime), so local time would mean capturing an offset at
// startup and being an hour wrong across a DST change. UTC needs none of that and cannot drift.
//
// Date split uses Howard Hinnant's civil_from_days - all integer, no library calls, safe here.
static void WriteUtcTimestamp(int fd, long long epoch)
{
	long long days = epoch / 86400;
	long long secs = epoch % 86400;
	if (secs < 0) { secs += 86400; days -= 1; }

	long long z = days + 719468;
	long long era = (z >= 0 ? z : z - 146096) / 146097;
	unsigned long long doe = (unsigned long long)(z - era * 146097);
	unsigned long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	long long y = (long long)yoe + era * 400;
	unsigned long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	unsigned long long mp = (5 * doy + 2) / 153;
	unsigned d = (unsigned)(doy - (153 * mp + 2) / 5 + 1);
	unsigned m = (unsigned)(mp + (mp < 10 ? 3 : -9));
	y += (m <= 2);

	wdec(fd, (unsigned long long)y); wstr(fd, "-");
	w2(fd, m); wstr(fd, "-");
	w2(fd, d); wstr(fd, " ");
	w2(fd, (unsigned)(secs / 3600)); wstr(fd, ":");
	w2(fd, (unsigned)((secs / 60) % 60)); wstr(fd, ":");
	w2(fd, (unsigned)(secs % 60));
	wstr(fd, " UTC");
}

struct BacktraceState
{
	int fd;
	int frame;
};

static _Unwind_Reason_Code TraceFrame(struct _Unwind_Context *ctx, void *arg)
{
	BacktraceState *state = (BacktraceState *)arg;
	uintptr_t pc = _Unwind_GetIP(ctx);
	if (pc == 0)
		return _URC_END_OF_STACK;

	// Cap it: a runaway or recursive stack must not produce an unbounded file.
	if (state->frame >= 48)
		return _URC_END_OF_STACK;

	wstr(state->fd, "  #");
	wdec(state->fd, (unsigned)state->frame);
	wstr(state->fd, " pc ");
	whex(state->fd, (unsigned long long)pc);

	// Module plus OFFSET is what matters: it is what llvm-symbolizer needs to turn this into a
	// file and line, using the unstripped .so from android/deps/prefix or the build tree.
	Dl_info info;
	if (dladdr((void *)pc, &info) && info.dli_fname)
	{
		wstr(state->fd, "  ");
		wstr(state->fd, info.dli_fname);
		if (info.dli_fbase)
		{
			wstr(state->fd, " + ");
			whex(state->fd, (unsigned long long)(pc - (uintptr_t)info.dli_fbase));
		}
		if (info.dli_sname)
		{
			wstr(state->fd, "  ");
			wstr(state->fd, info.dli_sname);
		}
	}
	wstr(state->fd, "\n");

	state->frame++;
	return _URC_NO_REASON;
}

static const char *SignalName(int sig)
{
	switch (sig)
	{
	case SIGSEGV: return "SIGSEGV (invalid memory access)";
	case SIGABRT: return "SIGABRT (abort - usually a failed assert or a heap error)";
	case SIGBUS:  return "SIGBUS (misaligned or unmapped access)";
	case SIGFPE:  return "SIGFPE (arithmetic error)";
	case SIGILL:  return "SIGILL (illegal instruction)";
	default:      return "unknown signal";
	}
}

static void CrashHandler(int sig, siginfo_t *info, void *ucontext)
{
	if (crashLogReady)
	{
		// Append: keeping the history matters more than a tidy file, because an intermittent fault
		// is the hard kind and the pattern across runs is the evidence.
		int fd = open(crashLogPath, O_WRONLY | O_CREAT | O_APPEND, 0644);
		if (fd >= 0)
		{
			wstr(fd, "\n==== " GAMENAME " crash ====\n");
			wstr(fd, "version: " VERSIONSTR "\n");

			struct timespec ts;
			if (clock_gettime(CLOCK_REALTIME, &ts) == 0)
			{
				wstr(fd, "time: ");
				WriteUtcTimestamp(fd, (long long)ts.tv_sec);
				wstr(fd, "\nunix time: ");
				wdec(fd, (unsigned long long)ts.tv_sec);
				wstr(fd, "\n");
			}

			wstr(fd, "signal: ");
			wdec(fd, (unsigned)sig);
			wstr(fd, " ");
			wstr(fd, SignalName(sig));
			wstr(fd, "\n");

			if (info)
			{
				wstr(fd, "code: ");
				wdec(fd, (unsigned)info->si_code);
				wstr(fd, "\nfault addr: ");
				whex(fd, (unsigned long long)(uintptr_t)info->si_addr);
				wstr(fd, "\n");
			}

			wstr(fd, "tid: ");
			wdec(fd, (unsigned long long)gettid());
			wstr(fd, "\n");

			if (crashContext[0])
			{
				wstr(fd, "context: ");
				wstr(fd, crashContext);
				wstr(fd, "\n");
			}

			wstr(fd, "backtrace:\n");
			BacktraceState state = { fd, 0 };
			_Unwind_Backtrace(TraceFrame, &state);
			if (state.frame == 0)
				wstr(fd, "  (no frames - built without unwind tables?)\n");

			wstr(fd, "==== end ====\n");
			fsync(fd);
			close(fd);
		}

		__android_log_write(ANDROID_LOG_ERROR, "selaco-ea", "crash log written to $PROGDIR");
	}

	// Hand back to whoever had it - bionic's debuggerd - so the tombstone and the logcat
	// backtrace are still produced. Without this, adding our log would COST the system record.
	for (int i = 0; i < kNumHandled; i++)
	{
		if (kHandledSignals[i] == sig)
		{
			sigaction(sig, &oldActions[i], nullptr);
			break;
		}
	}
	raise(sig);
}

void I_SetCrashContext(const char *text)
{
	if (text == nullptr)
	{
		crashContext[0] = '\0';
		return;
	}
	size_t n = strlen(text);
	if (n >= sizeof(crashContext)) n = sizeof(crashContext) - 1;
	memcpy(crashContext, text, n);
	crashContext[n] = '\0';
}

// Can we create/append this path? Tested by actually opening it - permissions on Android are
// granted at runtime, so the only reliable check is to try.
static bool TryCrashLogPath(const char *dir)
{
	const char *name = GAMENAMELOWERCASE "-crash.log";
	size_t n = strlen(dir);
	if (n == 0 || n + strlen(name) + 2 >= sizeof(crashLogPath))
		return false;

	char candidate[sizeof(crashLogPath)];
	memcpy(candidate, dir, n);
	if (candidate[n - 1] != '/')
		candidate[n++] = '/';
	strcpy(candidate + n, name);

	int fd = open(candidate, O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (fd < 0)
		return false;
	close(fd);

	strcpy(crashLogPath, candidate);
	return true;
}

void I_InstallCrashLog()
{
	// PUBLIC folder first, and this matters. progdir is the app's own external dir,
	// /sdcard/Android/data/<pkg>/files - which the Files app and MTP have both refused to enter
	// since Android 11, so it is an adb-only path AND it is deleted on uninstall. A beta tester
	// cannot retrieve a crash log from there. /sdcard/Selaco is where they already put the game
	// data by hand, so it is reachable by every means they have.
	//
	// In practice a public folder is always writable for anyone who can PLAY: the ipk3 cannot live
	// in the app-private dir without adb, so a player has necessarily granted
	// MANAGE_EXTERNAL_STORAGE - requestAllFilesAccess() is gated on !hasGameData() and fires until
	// they do. "Declined the permission" is not a state a playable install can be in.
	//
	// The progdir fallback therefore exists for the DEVELOPER case, not the player one: with the
	// ipk3 adb-pushed into the app-private dir, hasGameData() is already true, the permission is
	// never requested, and /sdcard/Selaco may not be writable. That path is adb-only, which is
	// fine, because so is that workflow.
	static const char *const candidates[] = {
		"/sdcard/Selaco",
		"/storage/emulated/0/Selaco",
		"/sdcard/Download",
		nullptr
	};

	crashLogReady = false;
	for (int i = 0; candidates[i] != nullptr && !crashLogReady; i++)
		crashLogReady = TryCrashLogPath(candidates[i]);
	if (!crashLogReady)
		crashLogReady = TryCrashLogPath(progdir.GetChars());
	if (!crashLogReady)
		return;

	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = CrashHandler;
	sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
	sigemptyset(&sa.sa_mask);

	for (int i = 0; i < kNumHandled; i++)
		sigaction(kHandledSignals[i], &sa, &oldActions[i]);

	// __android_log_write, not Printf: this runs from main() before the console exists, so a
	// Printf here goes nowhere and the install is unverifiable.
	__android_log_write(ANDROID_LOG_INFO, "selaco-ea", "crash handler installed, log path follows");
	__android_log_write(ANDROID_LOG_INFO, "selaco-ea", crashLogPath);
}
