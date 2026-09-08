#!/usr/bin/env bash
#
# Launch the macOS build against game data staged in gamedata/.
#
#   ./macos/run-macos.sh              # prefers gamedata/full, falls back to demo
#   ./macos/run-macos.sh demo         # force the demo
#   ./macos/run-macos.sh full
#   ./macos/run-macos.sh demo -- +map MAP01
#
# Anything after -- is passed straight through to the engine.
#
# The engine reports fatal errors in a modal dialog and never writes startup
# detail to stdout, so this always passes +logfile and tails it on exit.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GZ_ROOT="$(cd "$HERE/.." && pwd)"

: "${MACOS_ARCH:=$(uname -m)}"
: "${LOGFILE:=/tmp/selaco-run.log}"

APP="$GZ_ROOT/build-macos-$MACOS_ARCH/Selaco.app"
EXE="$APP/Contents/MacOS/Selaco"

[ -x "$EXE" ] || { echo "Missing $EXE - run ./macos/build-macos.sh first"; exit 1; }

WHICH="${1:-}"
if [ -n "$WHICH" ] && [ "$WHICH" != "--" ]; then
	shift
else
	WHICH=""
fi
[ "${1:-}" = "--" ] && shift

pick_dir() {
	for d in "$@"; do
		[ -f "$GZ_ROOT/gamedata/$d/Selaco.ipk3" ] && { echo "$d"; return; }
	done
}

if [ -z "$WHICH" ]; then
	WHICH="$(pick_dir full demo)"
fi

if [ -z "$WHICH" ]; then
	echo "No Selaco.ipk3 found in gamedata/full or gamedata/demo."
	echo "See gamedata/README.txt for the expected files."
	exit 1
fi

DATA="$GZ_ROOT/gamedata/$WHICH"
[ -f "$DATA/Selaco.ipk3" ] || { echo "No Selaco.ipk3 in $DATA - see gamedata/README.txt"; exit 1; }

# Only the game's own support archives. gzdoom.pk3 / lights.pk3 / brightmaps.pk3
# deliberately come from this build, not from the distribution - they are locked
# to the engine version. See gamedata/README.txt.
FILES=()
for f in game_support.pk3 game_widescreen_gfx.pk3; do
	[ -f "$DATA/$f" ] && FILES+=( "$DATA/$f" )
done

printf '\n\033[1m==> Launching %s data from gamedata/%s\033[0m\n' "$WHICH" "$WHICH"
echo "  iwad:  $DATA/Selaco.ipk3"
[ ${#FILES[@]} -gt 0 ] && printf '  file:  %s\n' "${FILES[@]}"
echo "  log:   $LOGFILE"
echo

rm -f "$LOGFILE"
set +e
if [ ${#FILES[@]} -gt 0 ]; then
	"$EXE" -iwad "$DATA/Selaco.ipk3" -file "${FILES[@]}" +logfile "$LOGFILE" "$@"
else
	"$EXE" -iwad "$DATA/Selaco.ipk3" +logfile "$LOGFILE" "$@"
fi
rc=$?
set -e

printf '\n\033[1m==> Exited (%s). Errors and warnings from the log:\033[0m\n' "$rc"
if [ -f "$LOGFILE" ]; then
	grep -inE "error|fail|cannot|unable|unsupported|not found" "$LOGFILE" | head -20 | sed 's/^/  /'
	echo
	echo "  Full log: $LOGFILE"
else
	echo "  No log written - the engine died before opening it."
fi
