#!/usr/bin/env bash
#
# Build GZSelaco for macOS.
#
#   ./macos/build-macos.sh              # host arch, SDL2 backend, Vulkan via MoltenVK
#   ./macos/build-macos.sh --clean
#   OSX_COCOA_BACKEND=ON ./macos/build-macos.sh     # native Cocoa backend (see below)
#
# Run ./macos/build-deps.sh first.
#
# Backend: SDL2, not the native Cocoa backend, even though Cocoa is upstream's
# default and does have a Vulkan path (cocoa/i_video.mm:228, VulkanCocoaView on a
# CAMetalLayer).
#
# The reason is Selaco's auxiliary-GL-context API for threaded texture upload
# (createAuxContext/setAuxContext/setMainContext/setNULLContext). It was added to
# the Win32 and SDL backends only - cocoa/gl_sysfb.h has none of those methods -
# so gl_framebuffer.cpp does not compile against the Cocoa backend. Setting
# OSX_COCOA_BACKEND=ON will fail until someone implements those on
# NSOpenGLContext, which is low value given Apple deprecated OpenGL at 4.1 and
# the target here is Vulkan through MoltenVK anyway.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GZ_ROOT="$(cd "$HERE/.." && pwd)"

: "${MACOS_ARCH:=$(uname -m)}"
: "${MACOS_DEPLOYMENT_TARGET:=11.0}"
: "${OSX_COCOA_BACKEND:=OFF}"
: "${BUILD_TYPE:=Release}"
: "${CMAKE_BIN:=/opt/homebrew/share/android-commandlinetools/cmake/3.22.1/bin/cmake}"
: "${NINJA_BIN:=/opt/homebrew/share/android-commandlinetools/cmake/3.22.1/bin/ninja}"
command -v cmake >/dev/null 2>&1 && CMAKE_BIN="$(command -v cmake)"
command -v ninja >/dev/null 2>&1 && NINJA_BIN="$(command -v ninja)"

PREFIX="$HERE/deps/prefix/$MACOS_ARCH"
BUILD="$GZ_ROOT/build-macos-$MACOS_ARCH"

say() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }

if [ "${1:-}" = "--clean" ]; then
	say "Removing $BUILD"
	rm -rf "$BUILD"
	shift
fi

[ -f "$PREFIX/lib/libzmusic.dylib" ] || {
	echo "Missing $PREFIX/lib/libzmusic.dylib - run ./macos/build-deps.sh first"
	exit 1
}

say "Configuring for macOS $MACOS_ARCH (Cocoa backend: $OSX_COCOA_BACKEND)"

# FindZMusic.cmake and FindVPX.cmake only search a couple of hardcoded
# locations, so point them at the deps prefix explicitly.
EXTRA=()
if [ "$OSX_COCOA_BACKEND" = "OFF" ]; then
	EXTRA+=( -DSDL2_INCLUDE_DIR="$PREFIX/include/SDL2" -DSDL2_LIBRARY="$PREFIX/lib/libSDL2.dylib" )
fi
# set -u trips on ${EXTRA[@]} when the array is empty on bash 3.2 (the macOS
# system bash), so give it something to expand.
EXTRA+=( -DHAVE_VULKAN=ON )

"$CMAKE_BIN" -S "$GZ_ROOT" -B "$BUILD" -G Ninja \
	-DCMAKE_MAKE_PROGRAM="$NINJA_BIN" \
	-DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
	-DCMAKE_OSX_ARCHITECTURES="$MACOS_ARCH" \
	-DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOS_DEPLOYMENT_TARGET" \
	-DOSX_COCOA_BACKEND="$OSX_COCOA_BACKEND" \
	-DZMUSIC_INCLUDE_DIR="$PREFIX/include" \
	-DZMUSIC_LIBRARIES="$PREFIX/lib/libzmusic.dylib" \
	-DVPX_INCLUDE_DIR="$PREFIX/include" \
	-DVPX_LIBRARIES="$PREFIX/lib/libvpx.a" \
	"${EXTRA[@]}" \
	"$@"

"$CMAKE_BIN" --build "$BUILD" --parallel

say "Done"
find "$BUILD" -maxdepth 2 -name '*.app' -o -maxdepth 2 -name '*.pk3' | sed 's|^|  |'
