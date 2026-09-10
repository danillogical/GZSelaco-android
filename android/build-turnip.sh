#!/usr/bin/env bash
#
# Build Mesa/Turnip for Android arm64 on a macOS host.
#
#   ./android/build-turnip.sh            # release - what you play and measure on
#   ./android/build-turnip.sh debug      # asserts + DWARF, for diagnosing a crash
#
# Adapted from ~/src/Banners-Turnip/build_turnip.sh, which is Linux-only.
#
# We build our own rather than download one because every prebuilt Turnip segfaults on a
# level load, and the fix is a source patch (see the kgsl_syncobj_merge step below).
# Prebuilts are also undebuggable: they ship
#
#     -fno-unwind-tables -fno-asynchronous-unwind-tables -Dbuildtype=release -Dstrip=true
#
# which is why five separate tombstones all stopped at frame #00 with no symbols and the
# crash went unexplained for so long. The debug mode here turns that same crash into a
# named assertion plus a full backtrace; that is how it was finally found. Both modes
# keep symbols and unwind tables, since neither costs anything at runtime.
#
# Differences from the original, all forced by the macOS host:
#   - uses the NDK already installed here rather than downloading the Linux one
#   - native file says darwin/aarch64, not linux/x86_64 (Apple Silicon)
#   - BSD sed needs an explicit empty suffix for -i
#   - brew bison/flex must precede Xcode's; see the PATH note below
#   - no ccache wrapper, to keep the toolchain lines simple
set -euo pipefail

# release (default) = what you play and measure on. debug = what you diagnose with.
MODE="${1:-release}"
case "$MODE" in
release | debug) ;;
*)
	echo "usage: $(basename "$0") [release|debug]"
	exit 1
	;;
esac

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKDIR="${TURNIP_WORKDIR:-/tmp/turnip-build}"
MESA_SRC="https://gitlab.freedesktop.org/mesa/mesa"
: "${NDK:=/opt/homebrew/share/android-commandlinetools/ndk/28.2.13676358}"
NDKBIN="$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin"
# Highest API the installed NDK has a wrapper for. Mesa wants >= 26 for Vulkan.
SDKVER="${SDKVER:-35}"
# Per-mode build dir and prefix, so both drivers can coexist and be A/B'd.
BUILDDIR="build-android-aarch64-$MODE"
PREFIX="$WORKDIR/out-$MODE"

say() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }

# Homebrew tools must come first, and the order matters:
#   - bison: macOS ships Xcode's 2.3, which lacks -Wcounterexamples that Mesa passes.
#     meson bakes the absolute bison path into the ninja rules at configure time, so
#     getting this wrong means reconfiguring, not just re-running ninja.
#   - flex: same reasoning, Xcode's is old.
#   - python3: Mesa needs >= 3.10 with mako, yaml AND packaging. `packaging` is not
#     optional on 3.12+, because Mesa falls back to distutils otherwise and that was
#     removed in 3.12 - the check exits before it ever looks at mako.
export PATH="/opt/homebrew/opt/bison/bin:/opt/homebrew/opt/flex/bin:/opt/homebrew/bin:$PATH"

[ -d "$NDKBIN" ] || { echo "NDK not found at $NDKBIN (set NDK)"; exit 1; }
[ -x "$NDKBIN/aarch64-linux-android$SDKVER-clang" ] || {
	echo "No aarch64-linux-android$SDKVER-clang in the NDK (set SDKVER)"; exit 1; }

mkdir -p "$WORKDIR"
cd "$WORKDIR"

if [ ! -d mesa/.git ]; then
	say "Cloning Mesa main (shallow)"
	git clone --depth=1 -b main "$MESA_SRC" mesa
else
	echo "  mesa already cloned"
fi

cd mesa
say "Mesa $(git rev-parse --short HEAD)"

# The upstream script's fixes for building Android headers with a recent NDK. BSD sed
# needs the empty suffix; GNU sed does not, which is why the original is Linux-only.
say "Applying Android-header fixes"
sed -i '' 's/typedef const native_handle_t\* buffer_handle_t;/typedef void* buffer_handle_t;/g' \
	include/android_stub/cutils/native_handle.h || true
sed -i '' 's/, hnd->handle/, (void *)hnd->handle/g' \
	src/util/u_gralloc/u_gralloc_fallback.c || true
sed -i '' -E 's/([a-z_]+)->handle->/((const native_handle_t *)\1->handle)->/g' \
	src/vulkan/runtime/vk_android.c || true

# The actual crash fix. kgsl_syncobj_merge() folds every wait in a submit into one
# syncobj, and both of its timestamp/fd transitions are wrong in the same way: neither
# converts the accumulated timestamp in `ret` into an fd.
#
#   - TS accumulator meets an fd wait: it calls kgsl_syncobj_ts_to_fd(sync), but the
#     enclosing case guarantees sync is ALREADY an fd. That trips the function's state
#     assert, and with asserts compiled out it reads sync->queue - never set for an
#     fd-backed syncobj - so timestamp_to_fd() loads queue->device off a null pointer.
#     tu_queue::device sits at offset 0x1b0, which is exactly the fault address every
#     prebuilt Turnip segfaulted at on a level load.
#   - TS accumulator meets a TS wait on a different queue: it merges against ret.fd,
#     still -1 because ret is in TS state, so sync_merge() fails its own assert.
#
# Both are fixed by converting &ret first and merging two valid fds. Reported upstream;
# remove this once it lands. Idempotent, so a re-run of the script is safe.
say "Patching kgsl_syncobj_merge (the 0x1b0 level-load crash)"
python3 - <<'PYEOF'
import sys
p = 'src/freedreno/vulkan/tu_knl_kgsl.cc'
t = open(p).read()

if 'ret is the TS one' in t:
    print("  already patched"); sys.exit(0)

subs = [
("""            } else {
               ret.state = KGSL_SYNCOBJ_STATE_FD;
               int sync_fd = kgsl_syncobj_ts_to_fd(sync);
               ret.fd = sync_merge_close("tu_sync", ret.fd, sync_fd, true);
               assert(ret.fd >= 0);
            }""",
"""            } else {
               /* Both sides are timestamps on different queues, so both have to
                * become fds. ret is still TS here, which means ret.fd is -1 -
                * convert ret's own timestamp as well, or sync_merge() is handed
                * an invalid fd1 and fails. */
               int ret_fd = kgsl_syncobj_ts_to_fd(&ret);
               int sync_fd = kgsl_syncobj_ts_to_fd(sync);
               ret.state = KGSL_SYNCOBJ_STATE_FD;
               ret.fd = sync_merge_close("tu_sync", ret_fd, sync_fd, true);
               assert(ret.fd >= 0);
            }""",
 "TS/TS cross-queue"),

("""         } else if (ret.state == KGSL_SYNCOBJ_STATE_TS) {
            ret.state = KGSL_SYNCOBJ_STATE_FD;
            int sync_fd = kgsl_syncobj_ts_to_fd(sync);
            ret.fd = sync_merge_close("tu_sync", ret.fd, sync_fd, true);
            assert(ret.fd >= 0);
         } else {""",
"""         } else if (ret.state == KGSL_SYNCOBJ_STATE_TS) {
            /* ret is the TS one - convert IT, not sync, which is already an fd.
             * Passing sync tripped kgsl_syncobj_ts_to_fd's state assert, and with
             * asserts compiled out it read sync->queue, which is never set for an
             * fd-backed syncobj: timestamp_to_fd then loads queue->device at
             * offset 0x1b0 off a null pointer. */
            int ret_fd = kgsl_syncobj_ts_to_fd(&ret);
            ret.state = KGSL_SYNCOBJ_STATE_FD;
            ret.fd = sync_merge_close("tu_sync", ret_fd, sync->fd, false);
            assert(ret.fd >= 0);
         } else {""",
 "fd wait with TS accumulator"),
]

for old, new, label in subs:
    if old not in t:
        # Upstream moved: fail loudly rather than silently building a crashing driver.
        print("  FAILED to match " + label + " - has upstream changed?"); sys.exit(1)
    t = t.replace(old, new, 1)

open(p, 'w').write(t)
print("  patched both transitions")
PYEOF

say "Writing meson machine files"
cat > android-aarch64.txt <<EOF
[binaries]
ar = '$NDKBIN/llvm-ar'
c = ['$NDKBIN/aarch64-linux-android$SDKVER-clang']
# Unwind tables deliberately LEFT ON - this is the whole point of the build.
cpp = ['$NDKBIN/aarch64-linux-android$SDKVER-clang++', '-fno-exceptions', '--start-no-unused-arguments', '-static-libstdc++', '--end-no-unused-arguments']
c_ld = '$NDKBIN/ld.lld'
cpp_ld = '$NDKBIN/ld.lld'
strip = '$NDKBIN/llvm-strip'
pkg-config = ['env', 'PKG_CONFIG_LIBDIR=$NDKBIN/pkg-config', '$(command -v pkg-config)']

[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'armv8'
endian = 'little'
EOF

# Host is Apple Silicon macOS, not the linux/x86_64 the original assumes.
cat > native.txt <<EOF
[binaries]
c = ['clang']
cpp = ['clang++']

[build_machine]
system = 'darwin'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'
EOF

WARNFLAGS="-D__ANDROID__ -Wno-error -Wno-deprecated-declarations -Wno-incompatible-pointer-types-discards-qualifiers -Wno-incompatible-pointer-types"

# Unwind tables and frame pointers stay on in BOTH modes. They cost nothing at runtime
# and they are the difference between a tombstone that names the bug and five tombstones
# that stop at frame #00 - which is exactly how the 0x1b0 crash went unsolved for so
# long. Only -g and the assertions differ between the modes.
if [ "$MODE" = debug ]; then
	OPTFLAGS="-g -fno-omit-frame-pointer -funwind-tables"
	MESON_MODE=(-Dbuildtype=debugoptimized -Db_ndebug=false)
	DESC="debugoptimized, DWARF line info, asserts ON"
else
	OPTFLAGS="-fno-omit-frame-pointer -funwind-tables"
	MESON_MODE=(-Dbuildtype=release -Db_ndebug=true)
	DESC="release, asserts OFF, symbols+unwind kept"
fi

export CFLAGS="$WARNFLAGS $OPTFLAGS"
export CXXFLAGS="$CFLAGS"
export LDFLAGS="-fuse-ld=lld"

say "Configuring ($DESC)"
rm -rf "$BUILDDIR"
meson setup "$BUILDDIR" \
	--cross-file android-aarch64.txt \
	--native-file native.txt \
	--prefix "$PREFIX" \
	"${MESON_MODE[@]}" \
	-Dstrip=false \
	-Dplatforms=android \
	-Dvideo-codecs= \
	-Dandroid-stub=true \
	-Dgallium-drivers= \
	-Dvulkan-drivers=freedreno \
	-Dvulkan-beta=true \
	-Dfreedreno-kmds=kgsl \
	-Degl=disabled \
	-Dplatform-sdk-version="$SDKVER" \
	-Dandroid-libbacktrace=disabled

say "Building ($MODE)"
ninja -C "$BUILDDIR" install

LIB="$PREFIX/lib/libvulkan_freedreno.so"
[ -f "$LIB" ] || { echo "Build produced no $LIB"; exit 1; }

say "Done ($MODE)"
ls -l "$LIB" | awk '{print "  " $5 " bytes  " $9}'
"$NDKBIN/llvm-readelf" --dynamic "$LIB" | grep -i soname | sed 's/^/  /'
printf '  has symbols: '
"$NDKBIN/llvm-nm" --defined-only "$LIB" >/dev/null 2>&1 && echo yes || echo no
printf '  has unwind info: '
# No grep -q here: it exits on the first match, readelf takes SIGPIPE, and pipefail
# turns that into a failure - a false "no" that only shows up when the output is large
# enough not to fit the pipe buffer.
if "$NDKBIN/llvm-readelf" -S "$LIB" | grep -E "\.eh_frame|ARM\.exidx" >/dev/null; then
	echo yes
else
	echo no
fi
printf '  asserts: '
if [ "$MODE" = debug ]; then echo "on"; else echo "off"; fi
printf '  syncobj fix present: '
grep -q "ret is the TS one" src/freedreno/vulkan/tu_knl_kgsl.cc && echo yes || echo "NO - BUILD IS BROKEN"
