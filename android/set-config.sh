#!/usr/bin/env bash
#
# Swap the graphics config on a connected device and restart the game.
#
#   ./android/set-config.sh thor               the shipped default; play on this
#   ./android/set-config.sh thor-fixedview-ab  single-variable A/B harness
#   ./android/set-config.sh thor-clean-binds   scrub debug keybinds from a dev device
#
# Run with no argument to list what is available. See configs/README.md.
#
# This pushes over the autoexec the app extracted on first run. The app only
# extracts that file when it is absent (SelacoActivity.extractAssets), so a push
# sticks across relaunches and reinstalls.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
dest=/sdcard/Android/data/com.selaco.game/files/autoexec.cfg

name="${1:-}"
src="$here/configs/$name.cfg"
if [ -z "$name" ] || [ ! -f "$src" ]; then
	echo "usage: $(basename "$0") <config>" >&2
	echo "available:" >&2
	for f in "$here"/configs/*.cfg; do
		echo "  $(basename "$f" .cfg)" >&2
	done
	exit 2
fi

echo "==> $name -> $dest"
adb push "$src" "$dest" >/dev/null
adb shell svc power stayon true          # a sleeping device looks exactly like a hung one
adb logcat -c
adb shell am force-stop com.selaco.game
adb shell am start -n com.selaco.game/.SelacoActivity >/dev/null

echo "==> running. Watch frame times with:"
echo "    adb logcat -s selaco-ea:V | grep BENCH"
