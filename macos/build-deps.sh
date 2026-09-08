#!/usr/bin/env bash
#
# Fetch and build the external libraries GZSelaco links against, for macOS.
# Everything lands in macos/deps/prefix/<arch>/.
#
#   ./macos/build-deps.sh                 # all of them, host arch
#   ./macos/build-deps.sh zmusic vpx      # just these
#
# Deps and how the engine consumes them:
#   ZMusic      link-time (src/CMakeLists.txt:335 - find_package(ZMusic REQUIRED))
#   libvpx      link-time, HARD requirement (src/CMakeLists.txt:378 - SEND_ERROR)
#   openal-soft runtime dlopen of "libopenal.1.dylib" (oalsound.cpp:76)
#   MoltenVK    runtime dlopen of "libMoltenVK.dylib" by volk (volk.c:84), so
#               there is no Vulkan SDK or loader to install - just this dylib
#   SDL2        only needed with -DOSX_COCOA_BACKEND=OFF; the default Cocoa
#               backend has its own Vulkan path (cocoa/i_video.mm:228)
#
# Unlike Android, macOS has Homebrew, so glib is available and ZMusic's vendored
# FluidSynth builds normally here.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GZ_ROOT="$(cd "$HERE/.." && pwd)"

: "${MACOS_ARCH:=$(uname -m)}"
: "${MACOS_DEPLOYMENT_TARGET:=11.0}"
: "${CMAKE_BIN:=/opt/homebrew/share/android-commandlinetools/cmake/3.22.1/bin/cmake}"
: "${NINJA_BIN:=/opt/homebrew/share/android-commandlinetools/cmake/3.22.1/bin/ninja}"
command -v cmake >/dev/null 2>&1 && CMAKE_BIN="$(command -v cmake)"
command -v ninja >/dev/null 2>&1 && NINJA_BIN="$(command -v ninja)"

ZMUSIC_TAG="1.3.0"
VPX_TAG="v1.15.2"
OPENAL_TAG="1.24.3"
OGG_TAG="v1.3.6"
VORBIS_TAG="v1.3.7"
FLAC_TAG="1.4.3"
OPUS_TAG="v1.5.2"
SNDFILE_TAG="1.2.2"
SDL2_TAG="release-2.32.10"
MOLTENVK_TAG="v1.4.2"

SRC="$HERE/deps/src"
BUILD="$HERE/deps/build/$MACOS_ARCH"
PREFIX="$HERE/deps/prefix/$MACOS_ARCH"

mkdir -p "$SRC" "$BUILD" "$PREFIX/lib" "$PREFIX/include"

say() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }

fetch() {
	local name="$1" url="$2" tag="$3"
	if [ -d "$SRC/$name/.git" ]; then
		echo "  $name already fetched ($tag)"
		return
	fi
	say "Fetching $name $tag"
	git clone --depth 1 --branch "$tag" --recurse-submodules --shallow-submodules "$url" "$SRC/$name"
}

cmake_build() {
	local name="$1"; shift
	say "Building $name for $MACOS_ARCH"
	"$CMAKE_BIN" -S "$SRC/$name" -B "$BUILD/$name" -G Ninja \
		-DCMAKE_MAKE_PROGRAM="$NINJA_BIN" \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_OSX_ARCHITECTURES="$MACOS_ARCH" \
		-DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOS_DEPLOYMENT_TARGET" \
		-DCMAKE_INSTALL_PREFIX="$PREFIX" \
		-DCMAKE_PREFIX_PATH="$PREFIX" \
		"$@"
	"$CMAKE_BIN" --build "$BUILD/$name" --parallel
	"$CMAKE_BIN" --install "$BUILD/$name"
}

# --- Audio codecs -------------------------------------------------------------
#
# Without these, ZMusic has NO decoder for any compressed audio format and the
# game is completely silent - not just missing music. Sound effects go through
# the same ZMusic CreateDecoder entry point as music
# (oalsound.cpp:1224 -> sounddecoder.cpp:40), whose only backends are libsndfile
# and mpg123.
#
# ogg/vorbis/FLAC are static (with PIC) and baked into a shared libsndfile, so
# only one extra dylib ships. libsndfile is linked directly rather than
# dlopen()ed, because ZMusic's dynamic path asks for the bare leafname
# "libsndfile.1.dylib" (sndfile_decoder.cpp:48) and dlopen does not search
# LC_RPATH, so a copy inside the .app bundle would never be found.
CODEC_STATIC_ARGS=(
	-DBUILD_SHARED_LIBS=OFF
	-DCMAKE_POSITION_INDEPENDENT_CODE=ON
	-DBUILD_TESTING=OFF
	# libvorbis 1.3.7 still declares cmake_minimum_required(VERSION 2.8), and
	# CMake 4 removed compatibility with anything below 3.5. Ignored by CMake 3.x.
	-DCMAKE_POLICY_VERSION_MINIMUM=3.5
)

build_ogg() {
	fetch ogg https://github.com/xiph/ogg.git "$OGG_TAG"
	cmake_build ogg "${CODEC_STATIC_ARGS[@]}" -DINSTALL_DOCS=OFF
}

build_vorbis() {
	fetch vorbis https://github.com/xiph/vorbis.git "$VORBIS_TAG"
	cmake_build vorbis "${CODEC_STATIC_ARGS[@]}"
}

build_flac() {
	fetch flac https://github.com/xiph/flac.git "$FLAC_TAG"
	cmake_build flac "${CODEC_STATIC_ARGS[@]}" \
		-DBUILD_PROGRAMS=OFF \
		-DBUILD_EXAMPLES=OFF \
		-DBUILD_DOCS=OFF \
		-DBUILD_CXXLIBS=OFF \
		-DINSTALL_MANPAGES=OFF \
		-DWITH_OGG=ON
}

build_opus() {
	fetch opus https://github.com/xiph/opus.git "$OPUS_TAG"
	cmake_build opus "${CODEC_STATIC_ARGS[@]}" -DOPUS_BUILD_PROGRAMS=OFF
}

build_sndfile() {
	fetch libsndfile https://github.com/libsndfile/libsndfile.git "$SNDFILE_TAG"
	cmake_build libsndfile \
		-DBUILD_SHARED_LIBS=ON \
		-DBUILD_PROGRAMS=OFF \
		-DBUILD_EXAMPLES=OFF \
		-DBUILD_TESTING=OFF \
		-DENABLE_EXTERNAL_LIBS=ON \
		-DENABLE_MPEG=OFF \
		-DCMAKE_POLICY_VERSION_MINIMUM=3.5
}

build_zmusic() {
	fetch ZMusic https://github.com/ZDoom/ZMusic.git "$ZMUSIC_TAG"

	# See the codec comment above for why this is DYN_SNDFILE=OFF.
	local sndfile_args=()
	if [ -f "$PREFIX/lib/libsndfile.dylib" ]; then
		sndfile_args=(
			-DDYN_SNDFILE=OFF
			-DSNDFILE_INCLUDE_DIR="$PREFIX/include"
			-DSNDFILE_LIBRARY="$PREFIX/lib/libsndfile.dylib"
		)
	else
		echo "  WARNING: no libsndfile in $PREFIX - ZMusic will have no audio decoder."
		echo "           Run '$0 ogg vorbis flac sndfile' first, or the game will be silent."
	fi

	cmake_build ZMusic -DBUILD_SHARED_LIBS=ON "${sndfile_args[@]}"
}

patch_openal_loop_overrun() {
	local root="$SRC/openal-soft"
	[ -f "$root/.macos-loop-overrun-patched" ] && return

	say "Patching openal-soft: LoadBufferStatic loop-buffer read overrun"

	# Same fix as android/deps/build-deps.sh - see the long explanation there. The two
	# platforms clone openal-soft into separate source trees, so the patch has to be
	# applied in both or macOS keeps the bug.
	#
	# core/voice.cpp, looping branch of LoadBufferStatic: `remaining` is computed from
	# the unwrapped dataPosInt, so when dataPosInt >= loopEnd (which the intPos line
	# immediately above exists to handle) `loopEnd-dataPosInt` underflows as size_t and
	# LoadSamples reads far past the end of the buffer. Bound it by loopEnd-intPos
	# instead; intPos is provably < loopEnd in both branches.
	#
	# Diagnosed on Android from three identical tombstones (SIGSEGV/SEGV_ACCERR on the
	# audio thread, page-aligned fault address, all frames inside libopenal). Triggered
	# by looping sounds, so the full game hits it within a minute and the demo does not.
	sed -i.bak \
		's|std::min(voiceSamples\.size(), loopEnd-dataPosInt)|std::min(voiceSamples.size(), loopEnd-intPos)|' \
		"$root/core/voice.cpp"

	grep -q 'loopEnd-intPos' "$root/core/voice.cpp" || {
		echo "ERROR: openal-soft loop-overrun patch did not apply" >&2
		exit 1
	}

	touch "$root/.macos-loop-overrun-patched"
}

build_openal() {
	fetch openal-soft https://github.com/kcat/openal-soft.git "$OPENAL_TAG"
	patch_openal_loop_overrun
	cmake_build openal-soft \
		-DLIBTYPE=SHARED \
		-DALSOFT_UTILS=OFF -DALSOFT_EXAMPLES=OFF -DALSOFT_TESTS=OFF \
		-DALSOFT_INSTALL_EXAMPLES=OFF -DALSOFT_INSTALL_UTILS=OFF
}

build_sdl2() {
	fetch SDL2 https://github.com/libsdl-org/SDL.git "$SDL2_TAG"
	cmake_build SDL2 -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF
}

# libvpx has a hand-rolled configure, not CMake.
build_vpx() {
	fetch libvpx https://github.com/webmproject/libvpx.git "$VPX_TAG"
	say "Building libvpx for $MACOS_ARCH"

	local vpx_target
	case "$MACOS_ARCH" in
		arm64)  vpx_target=arm64-darwin20-gcc ;;
		x86_64) vpx_target=x86_64-darwin20-gcc ;;
		*) echo "unsupported arch for libvpx: $MACOS_ARCH"; exit 1 ;;
	esac

	mkdir -p "$BUILD/libvpx"
	(
		cd "$BUILD/libvpx"
		# Decoders only - the engine plays cutscenes, it does not encode them.
		"$SRC/libvpx/configure" \
			--target="$vpx_target" \
			--prefix="$PREFIX" \
			--disable-examples --disable-tools --disable-docs --disable-unit-tests \
			--disable-vp8-encoder --disable-vp9-encoder \
			--enable-vp8-decoder --enable-vp9-decoder \
			--enable-static --disable-shared --enable-pic
		make -j"$(sysctl -n hw.ncpu)"
		make install
	)
}

# MoltenVK ships prebuilt release tarballs; building it from source pulls in the
# whole Vulkan-Tools/SPIRV chain for no benefit here.
build_moltenvk() {
	say "Fetching MoltenVK $MOLTENVK_TAG"
	local tar="$SRC/MoltenVK-macos.tar"
	if [ ! -f "$tar" ]; then
		curl -sSL --max-time 600 -o "$tar" \
			"https://github.com/KhronosGroup/MoltenVK/releases/download/$MOLTENVK_TAG/MoltenVK-macos.tar"
	fi
	rm -rf "$SRC/MoltenVK-extract"
	mkdir -p "$SRC/MoltenVK-extract"
	tar -xf "$tar" -C "$SRC/MoltenVK-extract"

	local dylib
	dylib="$(find "$SRC/MoltenVK-extract" -name 'libMoltenVK.dylib' | head -1)"
	[ -n "$dylib" ] || { echo "libMoltenVK.dylib not found in the release tarball"; exit 1; }
	install -m 644 "$dylib" "$PREFIX/lib/libMoltenVK.dylib"
	echo "  installed $PREFIX/lib/libMoltenVK.dylib"
}

TARGETS=("$@")
if [ ${#TARGETS[@]} -eq 0 ]; then
	# Order matters: vorbis and flac need ogg, sndfile needs all three, and
	# ZMusic links against sndfile.
	TARGETS=(ogg vorbis flac opus sndfile zmusic vpx openal moltenvk)
fi

for t in "${TARGETS[@]}"; do
	case "$t" in
		ogg)      build_ogg ;;
		vorbis)   build_vorbis ;;
		flac)     build_flac ;;
		opus)     build_opus ;;
		sndfile)  build_sndfile ;;
		zmusic)   build_zmusic ;;
		vpx)      build_vpx ;;
		openal)   build_openal ;;
		sdl2)     build_sdl2 ;;
		moltenvk) build_moltenvk ;;
		*) echo "unknown target: $t"; exit 1 ;;
	esac
done

say "Done. Prefix: $PREFIX"
find "$PREFIX/lib" -maxdepth 1 \( -name '*.dylib' -o -name '*.a' \) | sed "s|$PREFIX/|  |"
