#!/usr/bin/env bash
#
# Two-stage Android build for GZSelaco.
#
#   ./android/build-android.sh              # host tools if stale, then the engine
#   ./android/build-android.sh --clean      # wipe both build dirs first
#
# Stage 1 builds re2c, lemon and zipdir for the BUILD machine. The engine build
# shells out to those at compile time, and tools/*/CMakeLists.txt skips them
# entirely when CMAKE_CROSSCOMPILING is set, so a cross build has to import them
# from a native build (root CMakeLists.txt:60-63).
#
# Stage 2 cross-compiles the engine to libSelaco.so.
#
# External libraries come from android/deps/build-deps.sh - run that first.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GZ_ROOT="$(cd "$HERE/.." && pwd)"

: "${ANDROID_NDK:=/opt/homebrew/share/android-commandlinetools/ndk/28.2.13676358}"
: "${ANDROID_ABI:=arm64-v8a}"
: "${ANDROID_API:=26}"
: "${CMAKE_BIN:=/opt/homebrew/share/android-commandlinetools/cmake/3.22.1/bin/cmake}"
: "${NINJA_BIN:=/opt/homebrew/share/android-commandlinetools/cmake/3.22.1/bin/ninja}"
: "${BUILD_TYPE:=Release}"

HOST_BUILD="$GZ_ROOT/build-host"
ANDROID_BUILD="$GZ_ROOT/build-android-$ANDROID_ABI"
PREFIX="$HERE/deps/prefix/$ANDROID_ABI"

say() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }

if [ "${1:-}" = "--clean" ]; then
	say "Removing $HOST_BUILD and $ANDROID_BUILD"
	rm -rf "$HOST_BUILD" "$ANDROID_BUILD"
	shift                       # anything left in "$@" is forwarded to cmake below
fi

[ -f "$PREFIX/lib/libSDL2.so" ] || {
	echo "Missing $PREFIX/lib/libSDL2.so - run android/deps/build-deps.sh first"
	exit 1
}

# ---- Stage 1: host code-generation tools ---------------------------------
if [ ! -f "$HOST_BUILD/ImportExecutables.cmake" ]; then
	say "Stage 1: building host tools (re2c, lemon, zipdir)"
	"$CMAKE_BIN" -S "$HERE/host-tools" -B "$HOST_BUILD" -G Ninja \
		-DCMAKE_MAKE_PROGRAM="$NINJA_BIN" \
		-DCMAKE_BUILD_TYPE=Release
	"$CMAKE_BIN" --build "$HOST_BUILD"
else
	say "Stage 1: host tools already built"
fi

# ---- Stage 2: the engine -------------------------------------------------
say "Stage 2: cross-compiling the engine for $ANDROID_ABI (API $ANDROID_API)"

# FindSDL2.cmake / FindZMusic.cmake only look in desktop locations and hint off
# absolute paths, which the NDK toolchain re-roots into the sysroot. Point them
# straight at the deps prefix instead of fighting the search logic.
"$CMAKE_BIN" -S "$GZ_ROOT" -B "$ANDROID_BUILD" -G Ninja \
	-DCMAKE_MAKE_PROGRAM="$NINJA_BIN" \
	-DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
	-DANDROID_ABI="$ANDROID_ABI" \
	-DANDROID_PLATFORM="android-$ANDROID_API" \
	-DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
	-DIMPORT_EXECUTABLES="$HOST_BUILD/ImportExecutables.cmake" \
	-DCMAKE_FIND_ROOT_PATH="$PREFIX" \
	-DSDL2_INCLUDE_DIR="$PREFIX/include/SDL2" \
	-DSDL2_LIBRARY="$PREFIX/lib/libSDL2.so" \
	-DADRENOTOOLS_PREFIX="$PREFIX" \
	-DZMUSIC_INCLUDE_DIR="$PREFIX/include" \
	-DZMUSIC_LIBRARIES="$PREFIX/lib/libzmusic.so" \
	-DVPX_INCLUDE_DIR="$PREFIX/include" \
	-DVPX_LIBRARIES="$PREFIX/lib/libvpx.a" \
	-DHAVE_VULKAN=ON \
	-DNO_GTK=ON \
	"$@"

"$CMAKE_BIN" --build "$ANDROID_BUILD" --parallel

say "Done"
find "$ANDROID_BUILD" -name 'libSelaco.so' -o -name '*.pk3' | sed 's|^|  |'
