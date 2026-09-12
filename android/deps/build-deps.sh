#!/usr/bin/env bash
#
# Fetch and cross-compile the external libraries GZSelaco links against, for
# Android arm64-v8a. Everything lands in android/deps/prefix/<abi>/.
#
#   ./android/deps/build-deps.sh              # all of them, arm64-v8a
#   ./android/deps/build-deps.sh sdl2 zmusic  # just these
#
# Deps and how the engine consumes them:
#   SDL2        link-time; also supplies the Java Activity the APK is built around
#   ZMusic      link-time (src/CMakeLists.txt:335 - find_package(ZMusic REQUIRED))
#   libvpx      link-time, HARD requirement (src/CMakeLists.txt:378 - SEND_ERROR)
#   openal-soft runtime dlopen of "libopenal.so" (oalsound.cpp:68), so it only
#               has to be packaged into the APK, not linked
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GZ_ROOT="$(cd "$HERE/../.." && pwd)"

: "${ANDROID_NDK:=/opt/homebrew/share/android-commandlinetools/ndk/28.2.13676358}"
: "${ANDROID_ABI:=arm64-v8a}"
# minSdk 26. Android 13 is API 33; 26 keeps the door open for older handhelds and
# is the floor for the NDK APIs used here.
: "${ANDROID_API:=26}"
: "${CMAKE_BIN:=/opt/homebrew/share/android-commandlinetools/cmake/3.22.1/bin/cmake}"
: "${NINJA_BIN:=/opt/homebrew/share/android-commandlinetools/cmake/3.22.1/bin/ninja}"

SDL2_TAG="release-2.32.10"
ZMUSIC_TAG="1.3.0"
VPX_TAG="v1.15.2"
OPENAL_TAG="1.24.3"
OGG_TAG="v1.3.6"
VORBIS_TAG="v1.3.7"
FLAC_TAG="1.4.3"
OPUS_TAG="v1.5.2"
SNDFILE_TAG="1.2.2"
# Pinned to the commit Eden uses; its CMake hard-requires arm64-v8a.
ADRENOTOOLS_TAG="8ba23b42d742545b709064d6e2523cdb86de68f5"

SRC="$HERE/src"
BUILD="$HERE/build/$ANDROID_ABI"
PREFIX="$HERE/prefix/$ANDROID_ABI"
TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake"

[ -d "$ANDROID_NDK" ] || { echo "NDK not found at $ANDROID_NDK (set ANDROID_NDK)"; exit 1; }
[ -x "$CMAKE_BIN" ]   || { echo "cmake not found at $CMAKE_BIN (set CMAKE_BIN)"; exit 1; }

mkdir -p "$SRC" "$BUILD" "$PREFIX"

say() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }

# Shallow-clone a tag once; subsequent runs reuse the checkout.
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
	say "Building $name for $ANDROID_ABI (API $ANDROID_API)"
	"$CMAKE_BIN" -S "$SRC/$name" -B "$BUILD/$name" -G Ninja \
		-DCMAKE_MAKE_PROGRAM="$NINJA_BIN" \
		-DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
		-DANDROID_ABI="$ANDROID_ABI" \
		-DANDROID_PLATFORM="android-$ANDROID_API" \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX="$PREFIX" \
		-DCMAKE_FIND_ROOT_PATH="$PREFIX" \
		-DCMAKE_PREFIX_PATH="$PREFIX" \
		"$@"
	"$CMAKE_BIN" --build "$BUILD/$name" --parallel
	"$CMAKE_BIN" --install "$BUILD/$name"
}

build_sdl2() {
	fetch SDL2 https://github.com/libsdl-org/SDL.git "$SDL2_TAG"
	# Shared is mandatory: SDLActivity dlopen()s libSDL2.so from Java.
	cmake_build SDL2 \
		-DSDL_SHARED=ON \
		-DSDL_STATIC=OFF \
		-DSDL_TEST=OFF \
		-DSDL_VULKAN=ON \
		-DSDL_OPENGLES=ON
}

# ZMusic vendors FluidSynth, whose CMake hard-requires glib-2.0 on any non-Windows
# platform (thirdparty/fluidsynth/src/CMakeLists.txt - pkg_search_module(GLIB REQUIRED)).
# glib is not available for Android without a large build of its own, and FluidSynth's
# glib shim (utils/win32_glibstubs.c) is Win32-only.
#
# So FluidSynth is dropped and CreateFluidSynthMIDIDevice replaced with a stub that
# reports it as unavailable. ZMusic still ships adlmidi, opnmidi, timidity, timidity++,
# wildmidi and oplsynth, so MIDI - including soundfont playback via timidity - keeps
# working. To restore FluidSynth properly, port its ~150 lines of glib stubs to pthreads
# and __atomic_* builtins rather than cross-compiling glib. See TECHNICAL.md.
patch_zmusic_drop_fluidsynth() {
	local root="$SRC/ZMusic"
	[ -f "$root/.android-fluidsynth-dropped" ] && return

	say "Patching ZMusic to build without FluidSynth/glib"

	sed -i.bak 's|^add_subdirectory(fluidsynth/src)|# dropped for Android (needs glib): add_subdirectory(fluidsynth/src)|' \
		"$root/thirdparty/CMakeLists.txt"

	sed -i.bak \
		-e 's|^\([[:space:]]*\)mididevices/music_fluidsynth_mididevice\.cpp|\1mididevices/music_fluidsynth_stub.cpp|' \
		-e 's|\([[:space:]]\)fluidsynth)|)|' \
		"$root/source/CMakeLists.txt"

	cat > "$root/source/mididevices/music_fluidsynth_stub.cpp" <<'STUB'
// Android build: FluidSynth is not compiled in (its CMake requires glib-2.0, which is
// not available here). Selecting the FluidSynth MIDI device reports an error and lets
// ZMusic fall back to one of the other bundled synths.
#include <stdexcept>
#include "mididevice.h"
#include "zmusic/midiconfig.h"

// configuration.cpp reads and writes this unconditionally, so it still has to exist.
FluidConfig fluidConfig;

MIDIDevice *CreateFluidSynthMIDIDevice(int samplerate, const char *Args)
{
	(void)samplerate;
	(void)Args;
	throw std::runtime_error("FluidSynth is not available in this build");
}
STUB

	touch "$root/.android-fluidsynth-dropped"
}

# --- Audio codecs -------------------------------------------------------------
#
# Without these, ZMusic has NO decoder for any compressed audio format, and the
# game is completely silent - not just missing music. Sound effects go through
# the same ZMusic CreateDecoder entry point as music
# (oalsound.cpp:1224 -> sounddecoder.cpp:40), and its only backends are
# libsndfile and mpg123.
#
# ogg/vorbis/FLAC are built STATIC (with PIC) and baked into a SHARED libsndfile,
# so only one extra runtime library ships. libsndfile is then linked directly
# rather than dlopen()ed: ZMusic's dynamic path looks for the versioned soname
# "libsndfile.so.1" (sndfile_decoder.cpp:50), and Android cannot package a
# versioned soname - the APK only carries lib*.so.
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
	# Shared, so it carries the static ogg/vorbis/FLAC inside it and ZMusic only
	# has to link one thing. MPEG is off: it would pull in mpg123 and lame, and
	# Selaco's audio is Ogg Vorbis.
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
	patch_zmusic_drop_fluidsynth

	# DYN_SNDFILE=OFF matters: with it ON (the default) ZMusic defines
	# HAVE_SNDFILE and dlopen()s "libsndfile.so.1" at runtime, which Android
	# cannot package. OFF makes it find_package(SndFile) and link directly, so
	# the paths below have to be supplied explicitly.
	local sndfile_args=()
	if [ -f "$PREFIX/lib/libsndfile.so" ]; then
		sndfile_args=(
			-DDYN_SNDFILE=OFF
			-DSNDFILE_INCLUDE_DIR="$PREFIX/include"
			-DSNDFILE_LIBRARY="$PREFIX/lib/libsndfile.so"
		)
	else
		echo "  WARNING: no libsndfile in $PREFIX - building ZMusic with no audio decoder."
		echo "           Run '$0 ogg vorbis flac sndfile' first, or the game will be silent."
		sndfile_args=( -DDYN_SNDFILE=OFF )
	fi

	cmake_build ZMusic \
		-DBUILD_SHARED_LIBS=ON \
		-DDYN_MPG123=OFF \
		"${sndfile_args[@]}"
}

patch_openal_loop_overrun() {
	local root="$SRC/openal-soft"
	[ -f "$root/.android-loop-overrun-patched" ] && return

	say "Patching openal-soft: LoadBufferStatic loop-buffer read overrun"

	# Fixes a size_t underflow that segfaults the mixer on looping sounds.
	#
	# core/voice.cpp, the looping branch of LoadBufferStatic:
	#
	#   const size_t intPos{(dataPosInt < loopEnd) ? dataPosInt
	#       : (((dataPosInt-loopStart)%(loopEnd-loopStart)) + loopStart)};
	#   const size_t remaining{std::min(voiceSamples.size(), loopEnd-dataPosInt)};
	#
	# The intPos line exists precisely because dataPosInt can be >= loopEnd, and it
	# wraps it back into [loopStart, loopEnd). But `remaining` is then computed from
	# the UNWRAPPED dataPosInt, so in that same case `loopEnd-dataPosInt` underflows
	# (size_t) to a huge value, min() picks the full block, and LoadSamples reads far
	# past the end of buffer->mSamples starting from intPos - off the end of the
	# allocation and into a scudo guard page.
	#
	# Diagnosed on an Ayn Thor from three identical tombstones:
	#   signal 11 (SIGSEGV), code 2 (SEGV_ACCERR), page-aligned fault addr
	#   #00 LoadSamples lambda   core/voice.cpp:291
	#   #01 LoadBufferStatic     core/voice.cpp:547
	#   #02 alu.cpp:1993  #03 renderSamples alu.cpp:2276  #04 sdl2.cpp audioCallback
	#
	# LoadSamples reads FROM intPos, so the count must be bounded by what remains from
	# intPos. intPos is provably < loopEnd in both branches, so loopEnd-intPos cannot
	# underflow. Note the sibling loaders both guard their subtraction already
	# (LoadBufferCallback checks numCallbackSamples > dataPosInt; LoadBufferQueue
	# decrements dataPosInt across buffers first) - this branch is the only one that
	# does not, which is what makes it an oversight rather than an invariant.
	#
	# Worth reporting upstream; still present in 1.24.3.
	sed -i.bak \
		's|std::min(voiceSamples\.size(), loopEnd-dataPosInt)|std::min(voiceSamples.size(), loopEnd-intPos)|' \
		"$root/core/voice.cpp"

	grep -q 'loopEnd-intPos' "$root/core/voice.cpp" || {
		echo "ERROR: openal-soft loop-overrun patch did not apply" >&2
		exit 1
	}

	touch "$root/.android-loop-overrun-patched"
}

build_openal() {
	fetch openal-soft https://github.com/kcat/openal-soft.git "$OPENAL_TAG"
	patch_openal_loop_overrun
	# Output through SDL2, NOT OpenSL ES.
	#
	# openal-soft 1.24.3 offers only two Android backends: OpenSL and Oboe. Oboe
	# needs an external library we do not ship, so OpenSL was the only one active -
	# and it segfaults. Three reproducible crashes on the Thor, all identical:
	#   Fatal signal 11 (SIGSEGV), code 2 (SEGV_ACCERR)
	#   tid: alsoft-mixer   #00-#05 all inside libopenal.so
	# SEGV_ACCERR at page-aligned addresses in the scudo heap region is a heap
	# overrun into a guard page. (Not 16 KB page misalignment - the segments are
	# correctly aligned to 0x4000.)
	#
	# SDL2 is still the right backend choice - it adds no new dependency, since SDL2
	# is already a hard requirement, already built above, and already owns the Android
	# audio path. OpenSL is disabled outright so it cannot be selected at runtime, and
	# SDL2 is REQUIREd so a missing SDL2 fails the build loudly instead of silently
	# leaving us with no backend.
	#
	# BUT: an earlier version of this comment claimed the SDL2 backend "sidesteps the
	# crash entirely". That was wrong, and it was concluded from demo-only play. The
	# same crash reappeared on the full game after ~25 s:
	#   signal 11 (SIGSEGV), code 2 (SEGV_ACCERR), fault addr 0xb400006c507b0000
	#   tid: SDLAudioP2    #00-#07 inside libopenal.so then libSDL2.so
	# Identical signal, identical code, same all-frames-in-libopenal shape - only the
	# thread name differs. So the overrun is inside openal-soft's mixer and is
	# BACKEND-INDEPENDENT; changing backends only changed how long it took to hit.
	#
	# It is load-dependent: the demo never triggered it, the full game does quickly.
	# The engine requests snd_channels-1 mono sources (oalsound.cpp:656), which is 127
	# by default. Leading suspects, in order: voice count, then the NEON mixer or
	# resampler path (test with -DALSOFT_CPUEXT_NEON=OFF).
	cmake_build openal-soft \
		-DLIBTYPE=SHARED \
		-DALSOFT_UTILS=OFF \
		-DALSOFT_EXAMPLES=OFF \
		-DALSOFT_TESTS=OFF \
		-DALSOFT_INSTALL_EXAMPLES=OFF \
		-DALSOFT_INSTALL_UTILS=OFF \
		-DALSOFT_BACKEND_SDL2=ON \
		-DALSOFT_REQUIRE_SDL2=ON \
		-DALSOFT_BACKEND_OPENSL=OFF \
		-DALSOFT_BACKEND_WAVE=OFF
}

# libadrenotools: lets the Android Vulkan loader load a replacement GPU driver
# (Mesa/Turnip) while keeping libvulkan.so in place, so window-system integration still
# comes from the loader. Loading a driver HAL directly does NOT work - it advertises no
# VK_KHR_surface, only VK_EXT_headless_surface - see TECHNICAL.md, "Why adrenotools and
# not a direct load".
#
# It has no install target unless GEN_INSTALL_TARGET is on, and its hooks must end up in
# the app's nativeLibraryDir, so copy them out by hand rather than via --install.
build_adrenotools() {
	fetch libadrenotools https://github.com/eden-emulator/libadrenotools.git "$ADRENOTOOLS_TAG"

	# Its CMake hard-requires arm64-v8a and errors out on anything else.
	cmake_build_noinstall libadrenotools -DBUILD_SHARED_LIBS=OFF

	install -d "$PREFIX/lib" "$PREFIX/include"
	local h
	for h in libhook_impl.so libmain_hook.so libfile_redirect_hook.so libgsl_alloc_hook.so; do
		install -m 644 "$BUILD/libadrenotools/src/hook/$h" "$PREFIX/lib/$h"
	done
	install -m 644 "$BUILD/libadrenotools/libadrenotools.a" "$PREFIX/lib/libadrenotools.a"
	install -m 644 "$BUILD/libadrenotools/lib/linkernsbypass/liblinkernsbypass.a" \
		"$PREFIX/lib/liblinkernsbypass.a"
	cp -R "$SRC/libadrenotools/include/adrenotools" "$PREFIX/include/"
}

# Like cmake_build but skips --install, for projects with no install target.
cmake_build_noinstall() {
	local name="$1"; shift
	say "Building $name for $ANDROID_ABI (API $ANDROID_API)"
	"$CMAKE_BIN" -S "$SRC/$name" -B "$BUILD/$name" -G Ninja \
		-DCMAKE_MAKE_PROGRAM="$NINJA_BIN" \
		-DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
		-DANDROID_ABI="$ANDROID_ABI" \
		-DANDROID_PLATFORM="android-$ANDROID_API" \
		-DCMAKE_BUILD_TYPE=Release \
		"$@"
	"$CMAKE_BIN" --build "$BUILD/$name" --parallel
}

# libvpx has a hand-rolled configure, not CMake.
build_vpx() {
	fetch libvpx https://github.com/webmproject/libvpx.git "$VPX_TAG"
	say "Building libvpx for $ANDROID_ABI (API $ANDROID_API)"

	local host_tag
	case "$(uname -s)" in
		Darwin) host_tag=darwin-x86_64 ;;   # the NDK ships a single fat prebuilt dir on macOS
		Linux)  host_tag=linux-x86_64 ;;
		*) echo "unsupported host for libvpx build"; exit 1 ;;
	esac
	local tc="$ANDROID_NDK/toolchains/llvm/prebuilt/$host_tag"
	[ -d "$tc" ] || { echo "NDK toolchain dir missing: $tc"; exit 1; }

	local vpx_target
	case "$ANDROID_ABI" in
		arm64-v8a)   vpx_target=arm64-android-gcc; local triple=aarch64-linux-android ;;
		armeabi-v7a) vpx_target=armv7-android-gcc; local triple=armv7a-linux-androideabi ;;
		x86_64)      vpx_target=x86_64-android-gcc; local triple=x86_64-linux-android ;;
		*) echo "unsupported ABI for libvpx: $ANDROID_ABI"; exit 1 ;;
	esac

	mkdir -p "$BUILD/libvpx"
	(
		cd "$BUILD/libvpx"
		export CC="$tc/bin/${triple}${ANDROID_API}-clang"
		export CXX="$tc/bin/${triple}${ANDROID_API}-clang++"
		export LD="$tc/bin/ld"
		export AR="$tc/bin/llvm-ar"
		export NM="$tc/bin/llvm-nm"
		export RANLIB="$tc/bin/llvm-ranlib"
		export STRIP="$tc/bin/llvm-strip"
		# The engine only decodes cutscenes, so every encoder is dropped.
		"$SRC/libvpx/configure" \
			--target="$vpx_target" \
			--prefix="$PREFIX" \
			--disable-examples --disable-tools --disable-docs --disable-unit-tests \
			--disable-vp8-encoder --disable-vp9-encoder \
			--enable-vp8-decoder --enable-vp9-decoder \
			--enable-static --disable-shared \
			--enable-pic \
			--disable-runtime-cpu-detect
		make -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)"
		make install
	)
}

TARGETS=("$@")
if [ ${#TARGETS[@]} -eq 0 ]; then
	# Order matters: vorbis and flac need ogg, sndfile needs all three, and
	# ZMusic links against sndfile.
	TARGETS=(sdl2 ogg vorbis flac opus sndfile zmusic vpx openal adrenotools)
fi

for t in "${TARGETS[@]}"; do
	case "$t" in
		sdl2)     build_sdl2 ;;
		ogg)      build_ogg ;;
		vorbis)   build_vorbis ;;
		flac)     build_flac ;;
		opus)     build_opus ;;
		sndfile)  build_sndfile ;;
		zmusic)   build_zmusic ;;
		vpx)      build_vpx ;;
		openal)   build_openal ;;
		adrenotools) build_adrenotools ;;
		*) echo "unknown target: $t"; exit 1 ;;
	esac
done

say "Done. Prefix: $PREFIX"
find "$PREFIX" -name '*.so' -o -name '*.a' | sed "s|$PREFIX/|  |"
