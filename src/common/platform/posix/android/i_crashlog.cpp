/*
** i_crashlog.cpp
**---------------------------------------------------------------------------
** Copyright 2026 danillogical
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
** Persistent crash logging for Android.
**
** Why this exists. Android's own crash record is a tombstone under /data/tombstones, which is
** root-only, plus a logcat dump that lives in a ring buffer. Both are effectively unreachable for
** a beta tester, and the logcat copy is easy to lose - during this project's own development two
** crashes were rendered undiagnosable because the buffer had been cleared or had rotated. A
** tester cannot be asked to run adb.
**
** So this writes a durable log to a PUBLIC folder - /sdcard/Selaco, where the tester already put
** the game data by hand. See I_InstallCrashLog for why progdir is only a developer fallback.
**
** It CHAINS rather than replaces. Installing a handler on Android normally costs you bionic's
** debuggerd handler, and with it the tombstone and the logcat backtrace - which is exactly why
** i_main.cpp does not install the generic crashcatcher here. This handler writes its own record
** and then restores the previous handler and re-raises, so debuggerd still runs and produces the
** tombstone as well. You get both, not one.
**
** Note the two records are complementary, not redundant. Because we re-raise via raise(), the
** siginfo debuggerd sees is the tgkill one, so the tombstone reads `code -6 (SI_TKILL), fault addr
** --------`: the real fault address is only in OUR log. Conversely debuggerd's unwinder is far
** better than ours. Keep both.
**
** ORDER IS THE SAFETY MECHANISM in the handler. write(2) only, no malloc, no stdio, integers
** formatted by hand - but two things here can fail or hang outright, so each is attempted only
** after everything more valuable is already fsync'd:
**
**   - _Unwind_Backtrace needs CFI on the signal trampoline to walk out of a signal frame at all.
**     When it cannot, there are no frames, which is why the faulting PC is taken from ucontext
**     FIRST and written before any unwinding is attempted.
**   - dladdr takes bionic's loader mutex (g_dl_mutex). This port dlopens a replacement Vulkan
**     driver and libopenal, so a crash inside dlopen would DEADLOCK the handler - hung process,
**     no log, no tombstone. Symbolisation therefore runs last, after the raw addresses are on
**     disk; a log that stops after the addresses means exactly that happened.
*/

#include <signal.h>
#include <unwind.h>
#include <dlfcn.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <ucontext.h>
#include <sys/stat.h>
#include <link.h>
#include <elf.h>
#include <android/log.h>

#ifndef NT_GNU_BUILD_ID
#define NT_GNU_BUILD_ID 3
#endif

// Build identity for the log.
//
// NOT __DATE__/__TIME__: those bake in when THIS translation unit was compiled, and the build system
// skips unchanged TUs - three successive APKs all reported the same timestamp, so the stamp could not
// identify a build at all, which was the whole point of adding it.
//
// The ELF build-id is regenerated on every link, and it is the SAME identifier debuggerd prints in
// the tombstone, so a tester's log correlates directly with a tombstone and with the shipped .so.
// Resolved once at install time; the handler only reads the formatted string.
static char buildIdStr[48] = "unknown";

static int FindBuildId(struct dl_phdr_info *info, size_t, void *data)
{
	if (info->dlpi_addr != (ElfW(Addr))(uintptr_t)data)
		return 0;   // keep looking - not our library

	for (int i = 0; i < info->dlpi_phnum; i++)
	{
		const ElfW(Phdr) &ph = info->dlpi_phdr[i];
		if (ph.p_type != PT_NOTE)
			continue;

		const char *p = (const char *)(info->dlpi_addr + ph.p_vaddr);
		const char *end = p + ph.p_memsz;
		while (p + sizeof(ElfW(Nhdr)) <= end)
		{
			const ElfW(Nhdr) *n = (const ElfW(Nhdr) *)p;
			const char *name = p + sizeof(ElfW(Nhdr));
			const char *desc = name + ((n->n_namesz + 3) & ~3u);
			if (n->n_type == NT_GNU_BUILD_ID && n->n_namesz == 4 && memcmp(name, "GNU", 4) == 0)
			{
				static const char hex[] = "0123456789abcdef";
				size_t out = 0;
				for (size_t b = 0; b < n->n_descsz && out + 2 < sizeof(buildIdStr); b++)
				{
					unsigned char v = (unsigned char)desc[b];
					buildIdStr[out++] = hex[v >> 4];
					buildIdStr[out++] = hex[v & 0xf];
				}
				buildIdStr[out] = '\0';
				return 1;
			}
			p = desc + ((n->n_descsz + 3) & ~3u);
		}
	}
	return 1;
}

static void ResolveBuildId()
{
	Dl_info di;
	if (dladdr((void *)&ResolveBuildId, &di) && di.dli_fbase)
		dl_iterate_phdr(FindBuildId, di.dli_fbase);
}

#include "cmdlib.h"
#include "version.h"
#include "printf.h"

// Snapshotted at install time so the handler never touches an FString.
static char crashLogPath[512];
static bool crashLogReady = false;

// Free-form context the engine can update as it runs, e.g. the current map. Plain storage so it
// is safe to read from a signal handler.
static char crashContext[256];

// The handler runs on its own stack (see I_InstallCrashLog), which is the only way a
// stack-exhaustion SIGSEGV can be reported at all - on the faulting stack there is by definition
// no room left to deliver the signal. Must be static: a thread stack would defeat the point.
enum { kAltStackSize = 64 * 1024 };
static char altStack[kAltStackSize];

// A crash file left on a tester's device forever is a liability, so the accumulated history is
// dropped once it passes this. Checked at install time, never in the handler.
enum { kMaxLogBytes = 256 * 1024 };

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

// si_code is SIGNED and the interesting values are negative - SI_USER 0, SI_QUEUE -1, SI_TKILL -6.
// Printing it through an unsigned cast turned the -1 of a real SIGABRT into "code: 4294967295".
static void wsdec(int fd, long long v)
{
	if (v < 0)
	{
		wstr(fd, "-");
		wdec(fd, (unsigned long long)(-(v + 1)) + 1ull);   // avoids overflow at LLONG_MIN
		return;
	}
	wdec(fd, (unsigned long long)v);
}

static void whex(int fd, unsigned long long v){
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

// Frames are COLLECTED first and symbolised afterwards, so that dladdr - which can deadlock, see
// the file header - is never called until the raw addresses are already on disk.
enum { kMaxFrames = 48 };

struct BacktraceState
{
	uintptr_t pc[kMaxFrames];
	int frame;
};

static _Unwind_Reason_Code CollectFrame(struct _Unwind_Context *ctx, void *arg)
{
	BacktraceState *state = (BacktraceState *)arg;
	uintptr_t pc = _Unwind_GetIP(ctx);
	if (pc == 0)
		return _URC_END_OF_STACK;

	// Cap it: a runaway or recursive stack must not produce an unbounded file.
	if (state->frame >= kMaxFrames)
		return _URC_END_OF_STACK;

	state->pc[state->frame++] = pc;
	return _URC_NO_REASON;
}

// The registers the kernel saved at the moment of the fault. This is the one piece of information
// that is always available and never depends on unwinding succeeding.
static void WriteFaultRegisters(int fd, void *ucontext)
{
	if (ucontext == nullptr)
		return;
	const ucontext_t *uc = (const ucontext_t *)ucontext;
#if defined(__aarch64__)
	wstr(fd, "pc: ");  whex(fd, (unsigned long long)uc->uc_mcontext.pc);
	// LR matters when PC is garbage: a call through a bad function pointer leaves PC unmapped but
	// LR still pointing at the caller, which is the only way to locate the culprit.
	wstr(fd, "\nlr: ");  whex(fd, (unsigned long long)uc->uc_mcontext.regs[30]);
	wstr(fd, "\nsp: ");  whex(fd, (unsigned long long)uc->uc_mcontext.sp);
	wstr(fd, "\n");
#elif defined(__arm__)
	wstr(fd, "pc: ");  whex(fd, (unsigned long long)uc->uc_mcontext.arm_pc);
	wstr(fd, "\nlr: ");  whex(fd, (unsigned long long)uc->uc_mcontext.arm_lr);
	wstr(fd, "\nsp: ");  whex(fd, (unsigned long long)uc->uc_mcontext.arm_sp);
	wstr(fd, "\n");
#else
	(void)uc;
#endif
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
	// A fault raised INSIDE this handler must not re-enter it. Chaining straight out leaves a
	// partial record plus a tombstone, which beats a hang or an unbounded file.
	static volatile sig_atomic_t inHandler = 0;

	if (crashLogReady && !inHandler)
	{
		inHandler = 1;

		// Append: keeping the history matters more than a tidy file, because an intermittent fault
		// is the hard kind and the pattern across runs is the evidence.
		int fd = open(crashLogPath, O_WRONLY | O_CREAT | O_APPEND, 0644);
		if (fd >= 0)
		{
			wstr(fd, "\n==== " GAMENAME " crash ====\n");
			// build-id, not a compile timestamp - see ResolveBuildId. Matches the BuildId debuggerd
			// prints for libSelaco.so, so this log and the tombstone can be tied to the same binary.
			wstr(fd, "version: " VERSIONSTR "\nbuild-id: ");
			wstr(fd, buildIdStr);
			wstr(fd, "\n");

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
				wsdec(fd, (long long)info->si_code);
				// si_addr is only a fault address when si_code > 0. For SI_USER (0), SI_QUEUE (-1) and
				// SI_TKILL (-6) that union member holds the SENDING pid/uid instead, so printing it as
				// an address is actively misleading - and those are exactly the codes you get when the
				// signal was raised rather than trapped, which includes every abort() and our own
				// re-raise. Two real SIGABRT logs showed a plausible-looking "fault addr" that was a
				// pid/uid pair.
				if (info->si_code > 0)
				{
					wstr(fd, "\nfault addr: ");
					whex(fd, (unsigned long long)(uintptr_t)info->si_addr);
				}
				else
				{
					wstr(fd, "\nfault addr: n/a - signal was sent, not trapped");
				}
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

			// Registers first and flushed immediately. Everything below this point either can fail
			// to produce anything (the unwinder) or can hang (dladdr), so the fault location is
			// committed to disk before either is attempted.
			WriteFaultRegisters(fd, ucontext);
			fsync(fd);

			BacktraceState state;
			state.frame = 0;
			_Unwind_Backtrace(CollectFrame, &state);

			wstr(fd, "backtrace:\n");
			if (state.frame == 0)
			{
				// Not a build problem: unwinding OUT of a signal frame needs CFI on the sigreturn
				// trampoline, which is why debuggerd uses libunwindstack instead of _Unwind_*. Use
				// the pc/lr above, and the tombstone, which does have a real unwinder.
				wstr(fd, "  (none - could not unwind out of the signal frame; use pc/lr above)\n");
			}
			for (int i = 0; i < state.frame; i++)
			{
				wstr(fd, "  #");
				wdec(fd, (unsigned)i);
				wstr(fd, " pc ");
				whex(fd, (unsigned long long)state.pc[i]);
				wstr(fd, "\n");
			}
			fsync(fd);

			// LAST, because dladdr can deadlock on the loader mutex - see the file header. If the
			// log ends here with no modules section, the crash was inside dlopen.
			//
			// Module plus OFFSET is what matters: it is what llvm-symbolizer needs to turn this into
			// a file and line, using the unstripped .so from android/deps/prefix or the build tree.
			if (state.frame > 0)
				wstr(fd, "modules:\n");
			for (int i = 0; i < state.frame; i++)
			{
				Dl_info di;
				if (!dladdr((void *)state.pc[i], &di) || di.dli_fname == nullptr)
					continue;
				wstr(fd, "  #");
				wdec(fd, (unsigned)i);
				wstr(fd, " ");
				wstr(fd, di.dli_fname);
				if (di.dli_fbase)
				{
					wstr(fd, " + ");
					whex(fd, (unsigned long long)(state.pc[i] - (uintptr_t)di.dli_fbase));
				}
				if (di.dli_sname)
				{
					wstr(fd, "  ");
					wstr(fd, di.dli_sname);
				}
				wstr(fd, "\n");
			}

			wstr(fd, "==== end ====\n");
			fsync(fd);
			close(fd);

			// Only claim success if the file was actually opened, and name the real path - a literal
			// "$PROGDIR" here told the reader nothing, and progdir is not even where it goes.
			__android_log_write(ANDROID_LOG_ERROR, "selaco-ea", "crash log written to:");
			__android_log_write(ANDROID_LOG_ERROR, "selaco-ea", crashLogPath);
		}
	}

	// Hand back to whoever had it - bionic's debuggerd - so the tombstone and the logcat
	// backtrace are still produced. Without this, adding our log would COST the system record.
	//
	// raise() only marks sig PENDING, because sig is blocked for the duration of the handler
	// (sa_mask plus the implicit block). Delivery happens inside rt_sigreturn, once the kernel has
	// restored the original faulting registers, so debuggerd sees the true PC. Do NOT add
	// SA_NODEFER: that would deliver immediately and re-enter, breaking the chain.
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
//
// If the probe is what created the file, it is removed again. Otherwise every launch leaves an
// empty crash log sitting next to the game data, and "send me your crash log" stops meaning
// anything when the folder always contains one.
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

	bool existed = (access(candidate, F_OK) == 0);

	int fd = open(candidate, O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (fd < 0)
		return false;
	close(fd);

	if (!existed)
	{
		(void)unlink(candidate);
	}
	else
	{
		// Cap the accumulated history. A tester who plays through a run of crashes should not end up
		// with an ever-growing file, and 256 KB is many crashes' worth of context.
		struct stat st;
		if (stat(candidate, &st) == 0 && st.st_size > kMaxLogBytes)
			(void)unlink(candidate);
	}

	strcpy(crashLogPath, candidate);
	return true;
}

void I_InstallCrashLog()
{
	// SDL_main can re-enter the same process, so guard against installing twice. A second install
	// would capture our own CrashHandler into oldActions, and the chain at the end of the handler
	// would then restore US and re-raise - appending to the log forever until storage filled.
	static bool installed = false;
	if (installed)
		return;

	// Resolved here, not in the handler: dl_iterate_phdr takes the loader lock, which is exactly what
	// the handler must never do.
	ResolveBuildId();

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

	// SA_ONSTACK is meaningless without this: the kernel silently ignores the flag when no alternate
	// stack is registered, and the handler then runs on the faulting thread's stack. That loses the
	// one case the 48-frame cap exists for - a stack-exhaustion SIGSEGV, where there is no room to
	// deliver the signal at all, so neither our log nor a tombstone would be produced.
	stack_t ss;
	memset(&ss, 0, sizeof(ss));
	ss.ss_sp = altStack;
	ss.ss_size = sizeof(altStack);
	ss.ss_flags = 0;
	if (sigaltstack(&ss, nullptr) != 0)
		__android_log_write(ANDROID_LOG_WARN, "selaco-ea", "sigaltstack failed; stack-overflow crashes will not be logged");

	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = CrashHandler;
	sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
	sigemptyset(&sa.sa_mask);

	for (int i = 0; i < kNumHandled; i++)
		sigaction(kHandledSignals[i], &sa, &oldActions[i]);

	installed = true;

	// __android_log_write, not Printf: this runs from main() before the console exists, so a
	// Printf here goes nowhere and the install is unverifiable.
	__android_log_write(ANDROID_LOG_INFO, "selaco-ea", "crash handler installed, log path follows");
	__android_log_write(ANDROID_LOG_INFO, "selaco-ea", crashLogPath);
}
