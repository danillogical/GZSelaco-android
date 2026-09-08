/*
** i_benchmark.cpp
**
** Frame-time benchmarking that logs to the console (logcat on Android) instead of
** drawing on screen, so captures can be read and analysed offline rather than
** squinting at small text on a handheld.
**
** Why this exists: reading a single instantaneous "43 ms (23 fps)" off the screen
** is not enough to tell a GPU bottleneck from a CPU one, and averages hide exactly
** the spikes that matter - an explosion that drops one frame to 60 ms is felt, but
** barely moves a mean. So this reports percentiles, and pairs them with the
** engine's own VM/think/GPU stats to attribute the cost.
**
**   i_benchmark 1        summary every i_benchmark_interval seconds
**   i_benchmark 2        also log every frame slower than i_benchmark_spike ms
**
** Read it back with:  adb logcat -s selaco-ea:V | grep BENCH
*/

#include <algorithm>
#include <vector>

#include "c_cvars.h"
#include "c_dispatch.h"
#include "stats.h"
#include "printf.h"
#include "i_time.h"

CVAR(Int, i_benchmark, 0, CVAR_ARCHIVE)
CVAR(Float, i_benchmark_interval, 5.0f, CVAR_ARCHIVE)
CVAR(Float, i_benchmark_spike, 33.4f, CVAR_ARCHIVE)
// Cap on spike lines per window. Without this, once most frames exceed the spike
// threshold the logger writes several logcat lines PER FRAME, which slows the frame
// further and pushes more frames over the threshold - a feedback loop that makes
// the instrument produce the very slowdown it is measuring.
CVAR(Int, i_benchmark_maxspikes, 3, CVAR_ARCHIVE)
// Frames longer than this are treated as loads, not gameplay: reported on their own
// line and left out of the percentiles. A map load is a single ~1300 ms frame - it
// spawns thousands of actors and builds the BSP with the GPU idle - and leaving it in
// dragged a window mean from 33.3 ms to 42.7 ms, which reads as a performance problem
// that is not there. 0 disables the exclusion.
CVAR(Float, i_benchmark_ignore, 500.0f, CVAR_ARCHIVE)

// The Vulkan backend clears keepGpuStatActive every frame (vk_commandbuffer.cpp:290),
// so it has to be re-armed each frame or GPU timings are never collected.
extern bool keepGpuStatActive;
extern FString gpuStatOutput;

namespace
{
	// Engine stats worth attributing a spike to. VM and think are the ZScript and
	// actor-update costs, which is where explosions land; gpu and rendertimes are
	// the graphics side. Naming them explicitly keeps the log narrow enough to read.
	const char *const kStats[] = { "VM", "think", "gpu", "rendertimes" };

	// PRINT_NONOTIFY still reaches I_PrintStr (so logcat gets everything) but keeps
	// the text out of the on-screen notify buffer. Without it the benchmark draws
	// its own output every frame for several seconds, which measurably slows the
	// frames it is reporting on.
	const int kBenchPrint = PRINT_HIGH | PRINT_NONOTIFY;

	constexpr int kNumStats = (int)(sizeof(kStats) / sizeof(kStats[0]));

	// Stats have to be sampled every frame, not once per report. FStat::GetStats()
	// rolls the stat's own ring buffer as a side effect - ADD_STAT(VM) shifts a
	// 10-slot history and zeroes slot 0 on every call (vmframe.cpp:767) - so it is
	// built to be called once per frame by the stats overlay. Calling it once every
	// i_benchmark_interval instead left slot 0 accumulating a whole window, which is
	// why "VM time in last 10 tics" reported hundreds of ms and looked like a broken
	// timer. Sampling per frame makes the window mean what it says.
	FString currentStats[kNumStats];

	// Stats as they stood on the slowest frame of the window. A window mean cannot
	// say what made one frame cost 56 ms; this can.
	FString worstStats[kNumStats];

	std::vector<double> frames;
	uint64_t windowStartMs = 0;
	double worstInWindow = 0.0;
	int spikeCount = 0;
	int spikesLogged = 0;

	double Percentile(std::vector<double> &sorted, double frac)
	{
		if (sorted.empty()) return 0.0;
		size_t idx = (size_t)(frac * (sorted.size() - 1));
		return sorted[idx];
	}

	void SampleEngineStats()
	{
		for (int i = 0; i < kNumStats; i++)
		{
			FStat *stat = FStat::FindStat(kStats[i]);
			currentStats[i] = stat != nullptr ? stat->GetStats() : FString();
		}
	}

	void LogEngineStats()
	{
		for (int i = 0; i < kNumStats; i++)
		{
			if (worstStats[i].Len() > 0)
				Printf(kBenchPrint, "BENCH   %s\n", worstStats[i].GetChars());
		}
	}
}

// Called once per rendered frame with that frame's wall time in milliseconds.
void I_BenchmarkFrame(double frameMs)
{
	if (i_benchmark <= 0)
	{
		if (!frames.empty()) frames.clear();
		return;
	}

	uint64_t now = (uint64_t)(I_msTime());
	if (windowStartMs == 0) windowStartMs = now;

	// Re-arm GPU timing collection for the next frame.
	keepGpuStatActive = true;

	if (i_benchmark_ignore > 0 && frameMs >= i_benchmark_ignore)
	{
		Printf(kBenchPrint, "BENCH load/stall %.0f ms - excluded from percentiles\n", frameMs);
		return;
	}

	frames.push_back(frameMs);

	// Sample before the comparison below so worstStats describes this frame.
	SampleEngineStats();
	if (frameMs > worstInWindow)
	{
		worstInWindow = frameMs;
		for (int i = 0; i < kNumStats; i++) worstStats[i] = currentStats[i];
	}

	if (frameMs >= i_benchmark_spike)
	{
		spikeCount++;
		// Log at most a few per window, and without the stat block, so the cost of
		// measuring stays bounded no matter how bad the frame rate gets.
		if (i_benchmark >= 2 && spikesLogged < i_benchmark_maxspikes)
		{
			spikesLogged++;
			Printf(kBenchPrint, "BENCH spike %.1f ms (%.1f fps)\n", frameMs, frameMs > 0 ? 1000.0 / frameMs : 0.0);
		}
	}

	double windowMs = i_benchmark_interval * 1000.0;
	if (windowMs < 500.0) windowMs = 500.0;
	if ((double)(now - windowStartMs) < windowMs) return;

	std::vector<double> sorted = frames;
	std::sort(sorted.begin(), sorted.end());

	double total = 0.0;
	for (double f : sorted) total += f;
	double mean = total / sorted.size();

	// Percentiles rather than just a mean: p99 is what stutter actually feels like.
	double p50 = Percentile(sorted, 0.50);
	double p95 = Percentile(sorted, 0.95);
	double p99 = Percentile(sorted, 0.99);

	Printf(kBenchPrint, "BENCH %zu frames over %.1fs | mean %.1f ms (%.1f fps) | p50 %.1f | p95 %.1f | p99 %.1f | worst %.1f ms (%.1f fps) | spikes>%.0fms: %d\n",
		sorted.size(), (double)(now - windowStartMs) / 1000.0,
		mean, mean > 0 ? 1000.0 / mean : 0.0,
		p50, p95, p99,
		worstInWindow, worstInWindow > 0 ? 1000.0 / worstInWindow : 0.0,
		(double)i_benchmark_spike, spikeCount);

	Printf(kBenchPrint, "BENCH   -- stats as of the worst frame (%.1f ms) --\n", worstInWindow);
	LogEngineStats();

	if (gpuStatOutput.Len() > 0)
		Printf(kBenchPrint, "BENCH   gpu: %s\n", gpuStatOutput.GetChars());

	frames.clear();
	windowStartMs = now;
	worstInWindow = 0.0;
	spikeCount = 0;
	spikesLogged = 0;
	for (int i = 0; i < kNumStats; i++) worstStats[i] = FString();
}

CCMD(benchmark)
{
	if (argv.argc() > 1)
	{
		i_benchmark = atoi(argv[1]);
	}
	else
	{
		i_benchmark = i_benchmark > 0 ? 0 : 1;
	}
	Printf("benchmark logging %s (interval %.1fs, spike threshold %.1f ms)\n",
		i_benchmark > 0 ? "ON" : "off", (double)i_benchmark_interval, (double)i_benchmark_spike);
}
