#!/usr/bin/env bash
#
# Make Selaco.app self-contained: copy the dylibs it needs into
# Contents/Frameworks and rewrite install names so it runs without the build
# tree present.
#
#   ./macos/package-macos.sh
#
# Run ./macos/build-macos.sh first.
#
# Two different mechanisms are at work here:
#
#   linked  libSDL2, libzmusic - recorded as @rpath/... in the executable, so
#           they are resolved by adding an LC_RPATH of @executable_path/../Frameworks
#
#   dlopen  libMoltenVK (volk.c:84), libopenal (oalsound.cpp:80) - dlopen does
#           NOT search LC_RPATH, so the engine asks for these by an explicit
#           @executable_path-relative path. Nothing to fix up; they just have to
#           be in the right place.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GZ_ROOT="$(cd "$HERE/.." && pwd)"

: "${MACOS_ARCH:=$(uname -m)}"
: "${CODESIGN_IDENTITY:=-}"

PREFIX="$HERE/deps/prefix/$MACOS_ARCH"
BUILD="$GZ_ROOT/build-macos-$MACOS_ARCH"
APP="$BUILD/Selaco.app"
FRAMEWORKS="$APP/Contents/Frameworks"
EXE="$APP/Contents/MacOS/Selaco"

say() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }

[ -x "$EXE" ] || { echo "Missing $EXE - run ./macos/build-macos.sh first"; exit 1; }

mkdir -p "$FRAMEWORKS"

say "Copying dylibs into $FRAMEWORKS"
# Follow symlinks: the versioned file is the real one, but the executable
# records the major-version name (libzmusic.1.dylib), so copy under that name.
copy_lib() {
	local src="$1" dst="$2"
	[ -f "$src" ] || { echo "  missing $src"; return 1; }
	cp -L "$src" "$FRAMEWORKS/$dst"
	chmod 644 "$FRAMEWORKS/$dst"
	install_name_tool -id "@rpath/$dst" "$FRAMEWORKS/$dst" 2>/dev/null || true
	echo "  $dst"
}

copy_lib "$PREFIX/lib/libSDL2-2.0.0.dylib" "libSDL2-2.0.0.dylib"
copy_lib "$PREFIX/lib/libzmusic.1.dylib"   "libzmusic.1.dylib"
copy_lib "$PREFIX/lib/libopenal.1.dylib"   "libopenal.1.dylib"
copy_lib "$PREFIX/lib/libMoltenVK.dylib"   "libMoltenVK.dylib"

# libzmusic has a DT_NEEDED on this; without it there is no decoder for any
# compressed audio and the game is completely silent. ogg/vorbis/FLAC/opus are
# static inside it, so this one dylib covers all of them.
copy_lib "$PREFIX/lib/libsndfile.1.dylib"  "libsndfile.1.dylib"

# ZMusic links against libzmusiclite in some configurations; carry it if present.
[ -f "$PREFIX/lib/libzmusiclite.1.dylib" ] && copy_lib "$PREFIX/lib/libzmusiclite.1.dylib" "libzmusiclite.1.dylib"

say "Fixing up the executable's rpath"
# Drop any absolute build-tree rpaths, then add the bundle-relative one.
while read -r old; do
	[ -n "$old" ] && install_name_tool -delete_rpath "$old" "$EXE" 2>/dev/null || true
done < <(otool -l "$EXE" | awk '/LC_RPATH/{f=1} f&&/path /{print $2; f=0}')

install_name_tool -add_rpath "@executable_path/../Frameworks" "$EXE"

say "Moving game resources into Contents/Resources"
# codesign treats everything in Contents/MacOS as nested code and refuses to seal
# plain data there. The build drops the pk3s and the fm_banks/soundfont data next
# to the binary because add_pk3() and friends use $<TARGET_FILE_DIR:zdoom>, which
# for a bundle target is Contents/MacOS. Move all of it to Resources, which is
# where macOS expects non-code content; i_main.cpp points progdir there when it
# detects it is running from a bundle.
mkdir -p "$APP/Contents/Resources"
for pk3 in "$BUILD"/*.pk3; do
	[ -e "$pk3" ] || continue
	cp "$pk3" "$APP/Contents/Resources/"
done
for entry in "$APP/Contents/MacOS/"*; do
	[ -e "$entry" ] || continue
	[ "$entry" = "$EXE" ] && continue
	# mv refuses to merge into an existing non-empty directory (fm_banks,
	# soundfont), so clear the destination first. This runs on every repackage.
	rm -rf "$APP/Contents/Resources/$(basename "$entry")"
	mv -f "$entry" "$APP/Contents/Resources/"
	echo "  $(basename "$entry")"
done

say "Signing (identity: $CODESIGN_IDENTITY)"
# Ad-hoc by default, which is enough to run locally. Pass a Developer ID via
# CODESIGN_IDENTITY to produce something distributable (notarization is separate).
# Nested code first, then the bundle - signing the main executable on its own
# would give it a signature that disagrees with the bundle's resource seal.
find "$FRAMEWORKS" -name '*.dylib' -exec codesign --force --sign "$CODESIGN_IDENTITY" {} \;
codesign --force --sign "$CODESIGN_IDENTITY" "$APP"

say "Verifying"
otool -L "$EXE" | grep -vE "/usr/lib|/System" | sed 's/^/  /'
echo
echo "  Bundle: $APP"
echo "  Put Selaco.ipk3 in ~/Library/Application Support/Selaco-EA/ before running."
