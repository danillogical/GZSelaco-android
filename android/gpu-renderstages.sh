#!/usr/bin/env bash
#
# Capture GPU render stages directly with Perfetto. No AGI, no APA, no Mono, no GTK.
#
# Why this exists: every AGI route failed on orchestration, not on capability.
#   - the GUI System Profile sets up gpu.renderstages and never STARTS it, so the session
#     never reaches "ready" and the client times out after ~85 s (with or without counters
#     selected). gapis.log only carries the GPU producer's output, so whatever other data
#     source it cannot satisfy is invisible.
#   - gapit's CLI cannot be driven by the coding agent at all: it needs to bind or connect a
#     local gRPC port and the sandbox denies both, and the bypass does not reach gapis
#     because gapit spawns it as a child.
#   - a full -api vulkan trace is ~1 MB per frame; 18 s of it was 468 MB and wedged the app.
#
# What actually works: Qualcomm's driver ships /vendor/lib64/libgpudataproducer.so, and the
# platform property debug.graphics.gpu.profiler.perfetto makes the APP ITSELF register the
# data source. Verified on the Thor:
#     gpu.renderstages    com.selaco.game (148)
#     gpu.renderstages    com.selaco.game (149)
# AGI's whole orchestration layer existed only to do this step.
#
# IMPORTANT: the property is read at process start, so the game must be RELAUNCHED after
# setting it. This script does that, which means you have to reload your savegame.
#
# ONLY THE FIRST CAPTURE PER PROCESS IS VALID. A second `capture` against the same game
# process silently returns a trace that looks plausible but contains almost none of the game's
# GPU work - measured, twice: 24091 events / 297 submissions / 71.6% GPU busy on the first
# capture, then 3503 events / 1 submission / 2.3% busy on the second, with 0.07 ms bursts once
# per frame (i.e. some other process's compositing, not Selaco). It is NOT a real change in GPU
# behaviour, and it reproduced with the game setting unchanged, so it is a producer session
# limit rather than anything about what was being measured.
#
# So an A/B needs a RELAUNCH between the two captures, and therefore a savegame reload each
# time. Sanity-check every trace before trusting it: a valid one has hundreds of submissions
# and GPU busy well over 50%.
#
# Usage:
#   ./android/gpu-renderstages.sh setup     # set the property and relaunch the game
#   ./android/gpu-renderstages.sh query     # confirm the data source is registered
#   ./android/gpu-renderstages.sh capture   # capture (default 3s) -> /tmp/selaco-stages.perfetto
#   ./android/gpu-renderstages.sh off       # unset the property and relaunch
#
# Then open the .perfetto file in https://ui.perfetto.dev
set -u

PKG=com.selaco.game
ACT="$PKG/.SelacoActivity"
PROP=debug.graphics.gpu.profiler.perfetto
DUR="${DUR:-3000}"                       # ms
OUT="${OUT:-/tmp/selaco-stages.perfetto}"
DEVOUT=/data/misc/perfetto-traces/selaco-stages.perfetto

case "${1:-}" in
setup)
	adb shell setprop "$PROP" 1
	echo "$PROP = $(adb shell getprop $PROP | tr -d '\r')"
	adb shell am force-stop "$PKG"
	sleep 1
	adb shell am start -n "$ACT" >/dev/null
	echo "Relaunched. Load your save and stand at the junction, then run: $0 capture"
	;;
query)
	adb shell perfetto --query 2>/dev/null | grep -i "gpu\." | tr -d '\r' \
		|| echo "No GPU data sources. Run '$0 setup' first - the property is read at process start."
	;;
capture)
	# The config is inline so there is one less file to keep in sync. Render stages only:
	# gpu.counters needs device-specific counter IDs and AGI logged "Unrecognized GPU: A740",
	# so it has no counter profile for this chip. Stages alone answer the open question,
	# because they separate BINNING from RENDERING - the split Vulkan timestamp queries
	# structurally cannot see, and the one that decides whether plainflats' 15.7 ms is
	# geometry-stage or fragment-stage cost.
	CFG=$(mktemp /tmp/rsXXXX.cfg)
	cat > "$CFG" <<EOF
buffers: { size_kb: 131072 fill_policy: RING_BUFFER }
data_sources: { config { name: "gpu.renderstages" } }
duration_ms: $DUR
EOF
	echo "Capturing ${DUR}ms of GPU render stages..."
	adb shell "mkdir -p /data/misc/perfetto-traces" >/dev/null 2>&1
	adb shell "rm -f $DEVOUT" >/dev/null 2>&1
	# --txt so perfetto parses the human-readable config rather than a binary proto.
	if ! adb shell "cat > /data/local/tmp/rs.cfg" < "$CFG"; then
		echo "failed to push config"; rm -f "$CFG"; exit 1
	fi
	rm -f "$CFG"
	adb shell "perfetto --txt -c /data/local/tmp/rs.cfg -o $DEVOUT" 2>&1 | tr -d '\r' | tail -5
	adb pull "$DEVOUT" "$OUT" 2>&1 | tail -1
	[ -f "$OUT" ] && echo "Open $OUT in https://ui.perfetto.dev"
	;;
off)
	adb shell setprop "$PROP" \"\"
	adb shell am force-stop "$PKG"
	echo "Property cleared. Relaunch the game normally when you want to play."
	;;
*)
	echo "usage: $0 {setup|query|capture|off}"
	echo "  DUR=<ms>   capture length (default $DUR)"
	echo "  OUT=<path> output file (default $OUT)"
	exit 1
	;;
esac
