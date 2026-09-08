# CLAUDE.md

Technical notes for working in this repo. See [README.md](README.md) for install
instructions.

This is a **personal fork** of GZSelaco adding Android (arm64) and macOS (arm64)
targets. Not upstreamed, no contributions expected. Keep changes minimal and
commented, because every one of them is a permanent delta to carry.

---

## Building

### Android

```bash
./android/deps/build-deps.sh          # once: SDL2, ZMusic, OpenAL, libvpx, codecs
./android/build-android.sh            # host code-gen tools, then the engine
./android/package-apk.sh              # stage libs + assets (stops before gradle)
cd android && java -Xmx4g \
  -classpath /tmp/gradle-8.13/lib/gradle-launcher-8.13.jar \
  org.gradle.launcher.GradleMain --no-daemon assembleDebug
adb install -r android/app/build/outputs/apk/debug/app-debug.apk
```

**Gradle must be invoked as `java` directly.** Running it through a shell wrapper
fails with `java.net.SocketException: Operation not permitted` in this sandbox. Use
Gradle **8.x** — AGP 8.x rejects Gradle 9.

The build is two-stage: `re2c`, `lemon` and `zipdir` are compiled for the host first
(into `/tmp/gzs-build-host`) because the cross-compiled ones cannot run.

`-Wl,-z,nostart-stop-gc` is **load-bearing**. Without it lld garbage-collects the
`areg`/`creg`/`freg`/`greg`/`yreg`/`vreg` autoseg sections and the engine starts with
no cvars, no CCMDs and no script exports.

The engine builds as `libSelaco.so` (SHARED) and is `dlopen`ed by `SDLActivity`.

### macOS

```bash
./macos/build-deps.sh
./macos/build-macos.sh
./macos/package-macos.sh              # signs the bundle
./macos/run-macos.sh                  # picks gamedata/full over gamedata/demo
```

`package-macos.sh` re-signs but does **not** always recompile — if an engine change
seems absent from `Selaco.app`, run `build-macos.sh` explicitly.

Bundle layout matters: `progdir` is redirected to `Contents/Resources/` because
codesign refuses to seal `.pk3` files placed in `Contents/MacOS/`.

---

## Android storage and permissions

The app's own dir (`/sdcard/Android/data/com.selaco.game/files`, mode 770, group
`ext_data_rw`) is **unreachable by the user** — the Files app and MTP have both refused
to enter `Android/data` since Android 11. It is an adb-only path, and it is deleted on
uninstall along with any 1.2 GB game file in it.

So the game also searches public folders, which needs `MANAGE_EXTERNAL_STORAGE`:

- Manifest declares the permission
- `SelacoActivity.requestAllFilesAccess()` sends the user to
  `Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION` and finishes the activity —
  it is a Settings toggle, not a runtime dialog, so there is no callback to await
- Gated on `!hasGameData()`, so an adb workflow is never prompted
- `gameconfigfile.cpp` adds `/sdcard/Selaco`, `/sdcard/Download` and the
  `/storage/emulated/0` equivalents to IWADSearch and FileSearch
- `READ_MEDIA_*` cannot substitute: `.ipk3` is not a media type

---

## Config model — read this before adding a cvar

`android/configs/thor.cfg` is the shipped autoexec and is **five lines**. Do not add
graphics settings to it. Three separate times a graphics block here silently overrode
the player's in-menu choices, and the demo-era values rotted invisibly when the full
game renamed cvars (`r_effectLOD` deleted, `reflections`→`r_reflections`,
`r_gamedetail`→`g_gamedetail`, `r_permanentblood` split, explosion density formula
changed from `0.2*v` to `0.2*(v-1)+1`).

A cvar belongs in `thor.cfg` only if Selaco's own menus and defaults cannot express
it. Current contents, each justified:

| cvar | why |
|---|---|
| `vid_vsync 1` | not in `CVARINFO.defaults`; engine default is false |
| `vid_maxfps 35` | Selaco defaults 200; 35 is the lowest its menu offers |
| `i_benchmark 0` | ours, `CVAR_ARCHIVE`, stays on unless stated |
| `vid_fps 0` | `CVAR_ARCHIVE`, left on by earlier debugging |
| `con_notifylines 0` | ditto — draws console text over the game |

Switch profiles with `./android/set-config.sh <thor|thor-bench|thor-demo>`.

### Two facts that resolve most config confusion

**`setdefault` works.** Selaco ships a `CVARINFO.defaults` lump (~65 entries) that
changes the defaults of stock GZDoom cvars, and this engine implements it
(`d_main.cpp:1738`). `cL_run = true` and `movebob = 0.0126` are in there — never set
those by hand. The engine's raw `movebob` default of 0.25 is ~20x Selaco's own maximum.

**`nosave` does not mean "not saved".** In CVARINFO it maps to `CVAR_CONFIG_ONLY`,
which per `c_cvars.h:75` means *not in savegames and not over the network*. Those cvars
**do** persist in the ini. So the first-run dialog only needs to run once — do not
replicate it in an autoexec.

### Do not bypass the first-run dialog

`g_tos 1` skips it, and that skips `SetSteamdeckPresets()` plus ~45 preset cvars.
Doing so caused three separate bugs: walking instead of running, 20x view bob, and a
stale gamepad layout with no weapon-wheel binding. The full game's dialog is
gamepad-navigable. Let it run.

`g_steamdeck` is defaulted **on for Android** in `d_main.cpp` right after
`ParseCVarInfo()` and before `ExecCommands()`, so an autoexec can still override it.
Selaco's own detection can never fire here — it requires a 1280x800 screen
(`helper.zs:675`). It unlocks `SetSteamdeckPresets()` (9 cvars: UI scale 1.2, HUD scale
1.2, subtitle size 4, HUD opacity 0, and the aim-assist trio), Deck-only menu items,
and the engine's own texture-quality and transfer-thread tweaks.

---

## Patches carried against upstream

**openal-soft `LoadBufferStatic` overrun** — the important one. In the looping branch,
`remaining` was computed from the unwrapped `dataPosInt`, so when `dataPosInt >=
loopEnd` (which the `intPos` line above it exists to handle) `loopEnd-dataPosInt`
underflows as `size_t` and the mixer reads far past the buffer into a guard page.
Segfaulted the audio thread within a minute of full-game play; the demo never triggered
it. Fixed by bounding with `loopEnd-intPos`. Applied in **both**
`android/deps/build-deps.sh` and `macos/build-deps.sh` — they clone separate trees.
Still present in upstream 1.24.3; worth reporting.

Note the backend is irrelevant to that bug. OpenSL and SDL2 produced identical
signatures; switching backends only changed how long it took to hit.

**`vid_maxfps` floor relaxed** (`v_video.cpp`) — stock clamps up to `GameTicRate` (35).
30 is the only cap that divides a 60 Hz panel evenly (35 gives 1.71 vblanks and
judders). The shipped config uses 35 anyway, to match Selaco's menu.

**`FPSLimit()` runs with vsync on** (`vk_commandbuffer.cpp`) — Vulkan was the only
backend that skipped it when vsync was enabled, making `vid_maxfps` inert in the one
configuration a handheld ships with. GL and GLES already called it unconditionally.
Do **not** re-add a margin to compensate for vsync: `vid_vsync` selects
`VK_PRESENT_MODE_FIFO_RELAXED_KHR`, which presents a late frame immediately, so there
is nothing to round up and shaving the target just caps slightly fast.

**ZVulkan surface recreation** — Android destroys the `ANativeWindow` on pause, which
invalidates `VkSurfaceKHR`. Surface *and* swapchain both need recreating;
capability queries return empty caps instead of throwing.

**`i_benchmark`** (`common/engine/i_benchmark.cpp`) — frame-time percentiles to logcat.
Uses `PRINT_HIGH | PRINT_NONOTIFY` so it does not draw its own output and skew what it
measures. Samples engine stats **every frame**, because `GetStats()` rolls the stat's
ring buffer as a side effect (`vmframe.cpp:767`) and calling it once per report made
"VM time in last 10 tics" accumulate a whole window.

---

## Debugging on device

```bash
adb logcat -s selaco-ea:V | grep BENCH                       # frame times
adb shell run-as com.selaco.game cat files/selaco-ea.ini      # live config
```

- `run-as` needs a **debug** build.
- The ini flushes on clean exit and on some menu actions. `force-stop` is SIGKILL and
  writes nothing — quit from the in-game menu before diffing config state.
- adb's daemon dies intermittently here (`cannot connect to daemon`). `adb kill-server`
  and retry; it is environmental, not a device fault.
- Enabling `i_benchmark` sticks (`CVAR_ARCHIVE`). Switch back to `thor` afterwards, or
  a later "clean" run is silently instrumented. This already caused one run to be
  measured under a 30 fps lock nobody intended.

### Measuring performance honestly

Compare **peak explosion windows only**. deck-low reproduced within ~1 ms across three
runs, but windows within a single run span 36-43 ms depending on where the 5-second
boundary falls, and whole-log means are dominated by route coverage. Several "this is
free" conclusions in this repo's history came from comparing the wrong window.

A map load shows up as a single ~1.3 s frame (~2996 thinkers, ~500 ms of Think, GPU
idle). That is not a stall — `i_benchmark_ignore` excludes frames above 500 ms and
reports them separately.

Full-game baseline on a Thor at 1080p with `GFXPresetDeckLow` + `SpectacleDeck`:
33.3 ms locked, 39.2 ms peak explosion window, 52 ms worst frame. Of a 47 ms explosion
frame, roughly 36 ms is the main scene pass — translucent overdraw. The instrumented
postprocess chain is only 1.5-4.5 ms, so cutting SSAO or bloom buys almost nothing.

**An arm64 ZScript JIT is not worth building.** Measured, the VM is ~1 ms of a 33 ms
frame and ~3.8 ms of a 47 ms explosion frame. `HAVE_VM_JIT` is x86_64-only
(`CMakeLists.txt:223`), the vendored asmjit has no ARM backend at all (`arm.h` includes
a directory that does not exist), its API is a generation out of date (`CCFunc`,
`FuncSignature1..7`), and the 5819-line emitter has 312 `x86::` references. Upstream
GZDoom has not done it either, so there is nothing to cherry-pick.

---

## Replacement Vulkan drivers (Mesa/Turnip)

`vk_driver` loads a Mesa/Turnip build in place of Qualcomm's driver, through
libadrenotools. Verified on an Ayn Thor: `PurpleVK-public Adreno (TM) 740`,
Vulkan 1.4.359, Mesa 26.2.99, versus the stock 1.3.128 / 512.676.53.

Switch with `./android/set-config.sh thor-turnip` after putting a driver at
`/sdcard/Selaco/vulkan.purple.so`.

### Why adrenotools and not a direct load

Android ships Vulkan drivers as HAL modules. Loading one directly *works* - dlopen,
read `HMI`, validate its `'HWMT'` tag, call `open()`, validate the device's `'HWDT'`
tag, take `GetInstanceProcAddr` - and that was built and tested here. But the driver
then advertises only **8** instance extensions, and the only surface type among them
is `VK_EXT_headless_surface`. `VK_KHR_surface`, `VK_KHR_android_surface` and
`VK_KHR_swapchain` are implemented by Android's Vulkan **loader** on top of the
driver's `VK_ANDROID_native_buffer`, so `CreateInstance` fails with "extension not
present" and there is nothing to present to.

adrenotools keeps `libvulkan.so` and hooks only the loader's driver lookup, so WSI
still comes from the loader. Through it the same driver advertises **14** extensions
including `VK_KHR_surface` and `VK_KHR_android_surface`.

Static linking would not have helped: the driver genuinely has no surface support, so
linking it in yields the same 8 extensions. The alternative would be implementing the
loader's swapchain ourselves.

### Four things that each silently broke it

- **Linker namespace.** An app may only dlopen from its APK `lib/` or internal data
  dir; `/sdcard` is outside the namespace *and* noexec. `I_StageVulkanDriver`
  (sdlglvideo.cpp) copies the driver into internal storage first.
- **SONAME.** The staged copy must keep the source basename. Android keys libraries by
  `DT_SONAME`, and a Turnip build's soname is its filename, so staging it as
  `vkdriver.so` fails.
- **Trailing slash.** `adrenotools_open_libvulkan` concatenates `customDriverDir` and
  `customDriverName` with no separator (its `src/driver.cpp`), so the directory must
  end in `/`. Without it the `stat` check fails and it returns null *before* logging
  anything, which is a confusing way to lose an hour.
- **`useLegacyPackaging = true`.** Required in `app/build.gradle` or Android reads
  `.so` straight from the APK and `nativeLibraryDir` - which is where the hooks must
  live - is never populated. Already set.

`hookLibDir` is derived with `dladdr` on one of our own functions: `dli_fname` gives
the full path of `libSelaco.so`, whose directory *is* `nativeLibraryDir`. No JNI needed.

### Whether it is worth using

Unmeasured for performance. The bottleneck is translucent overdraw - ~36 ms of a 47 ms
explosion frame - which is fill rate and bandwidth, so a driver swap should not move it
much. The plausible win is different tiler load/store decisions. Measure with
`thor-turnip` against `thor-bench` before believing either way.

`libhardware-stub/` is left in the tree from the direct-HAL attempt. adrenotools does
not need it, since its namespace can resolve private platform libraries properly.

---

## Sandbox notes specific to this repo

- Swift-style nested `sandbox-exec` is not involved here, but Gradle is — see above.
- `unzip` on `Selaco.ipk3`: entries use **backslash** path separators, and unzip treats
  `\` as an escape. Quote patterns with doubled backslashes:
  `unzip -o -q Selaco.ipk3 'ACTORS\\Effects\\SMOKE.zsc' -d out`
- Symbolizing an Android crash: the unstripped `.so` in
  `android/deps/prefix/arm64-v8a/lib/` matches the tombstone's BuildID. Use the NDK's
  `llvm-symbolizer --obj=<lib> 0x<pc offset>`. This is what turned "OpenAL crashes
  somewhere" into an exact line.
