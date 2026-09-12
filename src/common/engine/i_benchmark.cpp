/*
** i_benchmark.cpp
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

// The engine's geometry counters, defined in hw_clock.cpp:56. Declared here rather than
// pulling a renderer header into common/engine - and note hw_clock.h's extern list omits
// rendered_commandbuffers, so it would not be sufficient anyway.
extern int rendered_lines, rendered_flats, rendered_sprites, rendered_decals;
extern int render_vertexsplit, render_texsplit;
extern int rendered_portals, rendered_commandbuffers;

CVAR(Int, i_benchmark, 0, CVAR_ARCHIVE)
// Frame rate below which a frame counts as a "dip". 26 rather than 30 so a frame that merely
// grazes the 30 fps cap is not counted - vid_maxfps 30 means a perfectly capped frame is 33.3 ms,
// and normal jitter around that would otherwise register as thousands of false dips.
CVAR(Float, i_dipfps, 26.0f, CVAR_ARCHIVE)

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

// The Vulkan backend clears keepGpuStatActive every frame in
// VkCommandBufferManager::UpdateGpuStats, so it has to be re-armed each frame or GPU timings are
// never collected. NOTE that at two frames in flight UpdateGpuStats early-returns before reading
// any timestamps, so on Vulkan the gpu line is currently always empty - the timestamp query pools
// are per-device, not per-frame-slot, and reading them across two in-flight frames is what the
// early return avoids. Restoring the gpu stat means giving each frame slot its own pool.
extern bool keepGpuStatActive;
extern FString gpuStatOutput;

namespace
{
	// Engine stats worth attributing a spike to. VM and think are the ZScript and
	// actor-update costs, which is where explosions land; gpu and rendertimes are
	// the graphics side. Naming them explicitly keeps the log narrow enough to read.
	// "rendertimes" carries the whole CPU/GPU split - BSP, per-type wall/flat/sprite
	// setup vs render, 2D, Drawcalls, All, Finish, GPU Wait and the frame limiter - and
	// "lightstats" carries the dynamic-light processing counts, which matter because
	// gl_lights 0 measured 11.2 ms on a scene where the cost was NOT pixel bound.
	//
	// Caveat on rendertimes: ADD_STAT(rendertimes) caches its string for 1 second
	// (hw_clock.cpp:133), so unlike VM and think it does not describe one frame. Read it as
	// "roughly now", not as the worst frame's own numbers.
	const char *const kStats[] = { "VM", "think", "gpu", "rendertimes", "lightstats" };

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

	// Geometry submitted on that same frame, from the engine's own counters
	// (hw_clock.cpp:56). These exist to separate PIXEL-bound from GEOMETRY-bound cost,
	// which GPU timestamps alone cannot do: a tiler's binning pass scales with vertices
	// and draw calls, not with pixels. Halving resolution scaled `ssao` by ~5x and
	// `plainwalls` by ~1.1x in an explosion frame, and without these counters there is no
	// way to tell whether that means "walls are geometry bound" or "the two scenes simply
	// had different amounts of wall in them".
	FString worstGeometry;

	// The gpu stat as it stood on that same worst frame. It used to be printed straight
	// from gpuStatOutput at report time, which is the LAST frame of the window, not the
	// worst one - so the GPU timings and the geometry counts described different frames and
	// could not legitimately be correlated. Capture it with everything else.
	FString worstGpu;

	// CPU time between present and the next BeginFrame - the window in which the GPU has
	// nothing queued (see VkCommandBufferManager::AdvanceFrameSlot). Accumulated as window means
	// rather than captured on the worst frame, for two reasons: the gap is a steady-state property,
	// and I_BenchmarkCpuGap necessarily runs AFTER I_BenchmarkFrame has already captured that
	// frame's stats, so a worst-frame value would be attributed one frame late.
	double gapSum = 0.0, tickSum = 0.0, gapMax = 0.0;
	int gapSamples = 0;

	// Dip counters. Maintained on EVERY frame regardless of i_benchmark, because the point is to
	// have a number a beta tester can report after simply playing, without knowing the benchmark
	// exists. Cost is two comparisons per frame.
	uint64_t dipFrames = 0, dipTotalFrames = 0, dipStalls = 0;
	double dipWorstMs = 0.0;
	uint64_t dipSinceMs = 0;

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
		if (worstGeometry.Len() > 0)
			Printf(kBenchPrint, "BENCH   %s\n", worstGeometry.GetChars());
		if (worstGpu.Len() > 0)
			Printf(kBenchPrint, "BENCH   gpu: %s\n", worstGpu.GetChars());
	}
}

// Called once per rendered frame with that frame's wall time in milliseconds.
void I_BenchmarkFrame(double frameMs)
{
	// Always-on dip accounting, ahead of the i_benchmark gate.
	{
		if (dipSinceMs == 0) dipSinceMs = (uint64_t)I_msTime();

		// A map load is a 1.3 s frame and is not a "dip" in any sense a player means, so it is
		// counted separately using the same threshold that excludes it from the percentiles.
		if (i_benchmark_ignore > 0 && frameMs >= i_benchmark_ignore)
		{
			dipStalls++;
		}
		else
		{
			dipTotalFrames++;
			const double dipMs = (i_dipfps > 0.0f) ? 1000.0 / i_dipfps : 0.0;
			if (dipMs > 0.0 && frameMs >= dipMs)
			{
				dipFrames++;
				if (frameMs > dipWorstMs) dipWorstMs = frameMs;
			}
		}
	}

	if (i_benchmark <= 0)
	{
		// Reset the WHOLE window, not just the frame list. CCMD(benchmark) is the documented way to
		// switch this on, so leaving windowStartMs, worstInWindow and the worst-frame stats stale
		// meant the first window after every enable mixed in a stale worst frame and reported an
		// interval measured from whenever the benchmark was last running.
		frames.clear();
		windowStartMs = 0;
		worstInWindow = 0.0;
		spikeCount = 0;
		spikesLogged = 0;
		for (int i = 0; i < kNumStats; i++) worstStats[i] = FString();
		worstGeometry = FString();
		worstGpu = FString();
		gapSum = tickSum = gapMax = 0.0;
		gapSamples = 0;
		return;
	}

	uint64_t now = (uint64_t)(I_msTime());
	if (windowStartMs == 0) windowStartMs = now;

	// Re-arm GPU timing collection for the next frame.
	keepGpuStatActive = true;

	if (i_benchmark_ignore > 0 && frameMs >= i_benchmark_ignore)
	{
		Printf(kBenchPrint, "BENCH load/stall %.0f ms - excluded from percentiles\n", frameMs);
		// Sample anyway before returning. GetStats() rolls each stat's own ring buffer as a side
		// effect, so skipping it on a load frame leaves that frame's ~500 ms of Think sitting in
		// slot 0, and the NEXT frame - which IS in the percentiles - inherits it and is reported as
		// the worst frame of the window. This is the exact failure the comment on SampleEngineStats
		// describes; the early return reintroduced it.
		SampleEngineStats();
		return;
	}

	frames.push_back(frameMs);

	// Sample before the comparison below so worstStats describes this frame.
	SampleEngineStats();
	if (frameMs > worstInWindow)
	{
		worstInWindow = frameMs;
		for (int i = 0; i < kNumStats; i++) worstStats[i] = currentStats[i];
		worstGpu = gpuStatOutput;
		worstGeometry.Format("geom: lines=%d flats=%d sprites=%d decals=%d portals=%d cmdbufs=%d vsplit=%d tsplit=%d",
			rendered_lines, rendered_flats, rendered_sprites, rendered_decals,
			rendered_portals, rendered_commandbuffers, render_vertexsplit, render_texsplit);
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

	// How much idle GPU time there is to reclaim, and how much of it is game simulation.
	// "other" is loop overhead, event processing, sound and the front of D_Display - work that
	// pipelining also covers but that no amount of tick optimisation would remove.
	if (gapSamples > 0)
	{
		double gapMean = gapSum / gapSamples, tickMean = tickSum / gapSamples;
		Printf(kBenchPrint, "BENCH   cpu gap: mean %.1f ms (%.0f%% of frame) | tick %.1f | other %.1f | max %.1f\n",
			gapMean, mean > 0 ? 100.0 * gapMean / mean : 0.0,
			tickMean, gapMean - tickMean, gapMax);
	}

	if (dipTotalFrames > 0)
	{
		Printf(kBenchPrint, "BENCH   dips <%.0f fps: %llu of %llu frames (%.2f%%) | worst %.1f ms | stalls %llu\n",
			(double)i_dipfps, (unsigned long long)dipFrames, (unsigned long long)dipTotalFrames,
			100.0 * (double)dipFrames / (double)dipTotalFrames, dipWorstMs,
			(unsigned long long)dipStalls);
	}

	Printf(kBenchPrint, "BENCH   -- stats as of the worst frame (%.1f ms) --\n", worstInWindow);
	LogEngineStats();

	// No second gpu line here. LogEngineStats already printed worstGpu, which is the gpu stat as it
	// stood on the WORST frame; printing gpuStatOutput as well printed the LAST frame of the window -
	// precisely the defect worstGpu was introduced to fix, left behind when it was added. Two lines
	// labelled the same way, describing different frames, one of which could not be correlated with
	// the geometry counters beside it.

	frames.clear();
	windowStartMs = now;
	worstInWindow = 0.0;
	spikeCount = 0;
	spikesLogged = 0;
	for (int i = 0; i < kNumStats; i++) worstStats[i] = FString();
	worstGeometry = FString();
	worstGpu = FString();
	gapSum = tickSum = gapMax = 0.0;
	gapSamples = 0;
}

// Called at the top of each frame with the CPU-only window that just closed: gapMs from
// present to BeginFrame, of which tickMs was the game tick.
void I_BenchmarkCpuGap(double tickMs, double gapMs)
{
	if (i_benchmark <= 0)
		return;

	// A map load or a stall is not a steady-state gap; use the same threshold that excludes
	// those frames from the percentiles so one 1.3 s load cannot dominate the mean.
	if (i_benchmark_ignore > 0 && gapMs >= i_benchmark_ignore)
		return;

	// Both must be validated, not just gapMs. These are deltas across the game tick, and the tick
	// is where I_FreezeTime lands (PerformWipe on every level transition, cl_waitforsave inside
	// TryRunTics), so a negative value here was reaching the mean and dragging "other" positive by
	// the same amount. Cheap belt-and-braces now that both use the monotonic I_msTimeF clock.
	if (tickMs < 0.0 || gapMs < 0.0)
		return;

	gapSum += gapMs;
	tickSum += tickMs;
	if (gapMs > gapMax) gapMax = gapMs;
	gapSamples++;
}

// Deliberately not gated on i_benchmark: "play for a while, then type fpsdips" is a request a
// beta tester can actually follow.
CCMD(fpsdips)
{
	if (argv.argc() > 1 && stricmp(argv[1], "reset") == 0)
	{
		dipFrames = dipTotalFrames = dipStalls = 0;
		dipWorstMs = 0.0;
		dipSinceMs = (uint64_t)I_msTime();
		Printf("fps dip counters reset\n");
		return;
	}

	if (dipTotalFrames == 0)
	{
		Printf("No frames counted yet.\n");
		return;
	}

	double mins = (double)((uint64_t)I_msTime() - dipSinceMs) / 60000.0;
	Printf("Dips below %.0f fps (>= %.1f ms): %llu of %llu frames (%.2f%%)\n",
		(double)i_dipfps, (i_dipfps > 0.0f) ? 1000.0 / (double)i_dipfps : 0.0,
		(unsigned long long)dipFrames, (unsigned long long)dipTotalFrames,
		100.0 * (double)dipFrames / (double)dipTotalFrames);
	Printf("Worst frame: %.1f ms (%.1f fps)   Load stalls excluded: %llu\n",
		dipWorstMs, dipWorstMs > 0 ? 1000.0 / dipWorstMs : 0.0,
		(unsigned long long)dipStalls);
	Printf("Measured over %.1f minutes.  'fpsdips reset' to start over.\n", mins);
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
