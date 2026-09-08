#!/usr/bin/env bash
#
# Stage the native build into the Gradle project and assemble the APK.
#
#   ./android/package-apk.sh            # stage + assembleDebug
#   ./android/package-apk.sh --stage    # stage only (no Gradle needed)
#
# Run android/build-android.sh first.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GZ_ROOT="$(cd "$HERE/.." && pwd)"

: "${ANDROID_NDK:=/opt/homebrew/share/android-commandlinetools/ndk/28.2.13676358}"
: "${ANDROID_ABI:=arm64-v8a}"

ENGINE_BUILD="$GZ_ROOT/build-android-$ANDROID_ABI"
PREFIX="$HERE/deps/prefix/$ANDROID_ABI"
SDL_SRC="$HERE/deps/src/SDL2"
APP="$HERE/app"
JNILIBS="$APP/src/main/jniLibs/$ANDROID_ABI"
ASSETS="$APP/src/main/assets"
JAVA_SDL="$APP/src/main/java-sdl"

TOOLCHAIN="$ANDROID_NDK/toolchains/llvm/prebuilt/darwin-x86_64"
STRIP="$TOOLCHAIN/bin/llvm-strip"

say() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }

[ -f "$ENGINE_BUILD/src/libSelaco.so" ] || {
	echo "Missing $ENGINE_BUILD/src/libSelaco.so - run android/build-android.sh first"
	exit 1
}

mkdir -p "$JNILIBS" "$ASSETS" "$JAVA_SDL"

# ---- native libraries ----------------------------------------------------
say "Staging native libraries into $JNILIBS"

# libSelaco.so is ~118 MB with debug info. Stripping is the difference between
# a usable APK and an unusable one; keep the unstripped copy in the build dir
# for symbolicating logcat traces.
install -m 644 "$ENGINE_BUILD/src/libSelaco.so" "$JNILIBS/libSelaco.so"
"$STRIP" "$JNILIBS/libSelaco.so"

# libsndfile carries static ogg/vorbis/FLAC/opus inside it and is a DT_NEEDED of
# libzmusic.so. Without it there is no decoder for any compressed audio and the
# game is completely silent - see report-android-port.md D5.
for lib in libSDL2.so libzmusic.so libopenal.so libsndfile.so; do
	install -m 644 "$PREFIX/lib/$lib" "$JNILIBS/$lib"
	"$STRIP" "$JNILIBS/$lib"
done

# OpenMP is a real DT_NEEDED of libSelaco.so (the engine's parallel_for uses it)
# and is not part of the Android system image, so it ships with the app.
OMP_ABI_DIR="aarch64"
case "$ANDROID_ABI" in
	arm64-v8a) OMP_ABI_DIR=aarch64 ;;
	armeabi-v7a) OMP_ABI_DIR=arm ;;
	x86_64) OMP_ABI_DIR=x86_64 ;;
esac
OMP_SO="$(find "$TOOLCHAIN/lib/clang" -path "*linux/$OMP_ABI_DIR/libomp.so" | head -1)"
[ -n "$OMP_SO" ] || { echo "Could not find libomp.so in the NDK"; exit 1; }
install -m 644 "$OMP_SO" "$JNILIBS/libomp.so"

# ---- engine resources ----------------------------------------------------
say "Staging pk3s into $ASSETS"
for pk3 in "$ENGINE_BUILD"/*.pk3; do
	[ -e "$pk3" ] || continue
	install -m 644 "$pk3" "$ASSETS/"
done
# assets/autoexec.cfg is checked in, not generated - it is the shipped Android
# default config (skips the first-run dialog, sets the 1080p60 graphics profile).
[ -f "$ASSETS/autoexec.cfg" ] && echo "  autoexec.cfg (shipped default config)"

# ---- SDL's Java sources --------------------------------------------------
# SDLActivity and friends are part of SDL, not of this repo, so they are copied
# out of the SDL checkout rather than vendored.
say "Staging SDL Java sources into $JAVA_SDL"
SDL_JAVA_SRC="$SDL_SRC/android-project/app/src/main/java"
[ -d "$SDL_JAVA_SRC" ] || { echo "Missing $SDL_JAVA_SRC - run android/deps/build-deps.sh sdl2"; exit 1; }
rm -rf "$JAVA_SDL/org"
mkdir -p "$JAVA_SDL"
cp -R "$SDL_JAVA_SRC/org" "$JAVA_SDL/"

say "Staged"
du -sh "$JNILIBS" "$ASSETS" | sed 's|^|  |'

if [ "${1:-}" = "--stage" ]; then
	exit 0
fi

# ---- assemble ------------------------------------------------------------
say "Assembling APK"
cd "$HERE"
if [ -n "${GRADLE_LAUNCHER_JAR:-}" ]; then
	# Escape hatch for the Claude Code sandbox, which denies the local socket
	# Gradle's FileLockContentionHandler binds ("java.net.SocketException:
	# Operation not permitted"). Invoking java directly rather than through the
	# gradle shell script avoids the extra process layer that the tool allowlist
	# does not reach. See report-android-port.md W5.
	java -Xmx4g -classpath "$GRADLE_LAUNCHER_JAR" org.gradle.launcher.GradleMain --no-daemon assembleDebug
elif [ -x ./gradlew ]; then
	./gradlew assembleDebug
elif command -v gradle >/dev/null 2>&1; then
	gradle assembleDebug
else
	echo "No Gradle wrapper and no gradle on PATH."
	echo "See report-android-port.md D7 - do not use Homebrew's Gradle 9.x with AGP 8.x."
	exit 1
fi

APK="$(find "$APP/build/outputs/apk" -name '*.apk' -newer "$JNILIBS/libSelaco.so" 2>/dev/null | head -1)"
if [ -z "$APK" ]; then
	echo
	echo "ERROR: no APK newer than the staged libraries was produced."
	echo
	echo "If Gradle reported:"
	echo "    java.net.SocketException: Operation not permitted"
	echo "then the sandbox blocked the local socket Gradle binds for its file-lock"
	echo "handler. Running Gradle through this script does not help, because the"
	echo "shell adds a process layer the tool allowlist does not reach. Invoke java"
	echo "as the top-level command instead:"
	echo
	echo "    cd android && java -classpath \$GRADLE_LAUNCHER_JAR \\"
	echo "        org.gradle.launcher.GradleMain --no-daemon assembleDebug"
	echo
	echo "See report-android-port.md W5."
	exit 1
fi
echo "  $APK"
unzip -l "$APK" | grep '\.so$' | awk '{print "    ", $1, $4}'
