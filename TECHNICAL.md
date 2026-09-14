# TECHNICAL.md

How the Android and macOS ports of GZSelaco work, and why they are built this way.

This is a **personal fork** adding Android (arm64) and macOS (arm64) targets. It merges
from upstream GZDoom, so every change here is a permanent delta to carry — prefer a
small targeted guard over a forked file, and comment why.

For install instructions see [README.md](README.md). For the rules and traps that bite
when *working* in this repo, see [CLAUDE.md](CLAUDE.md).

---

## Architecture

### Android

The engine builds as **`libSelaco.so` (SHARED)** and is `dlopen`ed by `SDLActivity` —
there is no executable, which is why the generic crash catcher cannot work here (it
re-execs `argv[0]`).

**The build is two-stage.** `re2c`, `lemon` and `zipdir` are compiled for the *host*
first (into `/tmp/gzs-build-host`), because the cross-compiled ones cannot run. Then the
engine is cross-compiled against them.

**`-Wl,-z,nostart-stop-gc` is load-bearing.** Without it lld garbage-collects the
`areg`/`creg`/`freg`/`greg`/`yreg`/`vreg` autoseg sections and the engine starts with no
cvars, no CCMDs and no script exports — a silent, baffling failure.

Platform code lives in `common/platform/posix` + `posix/sdl` + `posix/android`. The
convention is **targeted `#ifdef __ANDROID__` guards in shared files**, and a genuinely
new file only where the whole file is wrong (`i_specialpaths.cpp`, `i_crashlog.cpp`).
Four small guards in a 600-line file survive a merge from upstream; a 600-line duplicate
does not.

### macOS

SDL2 rather than Cocoa, and Vulkan via **MoltenVK**. Bundle layout matters: `progdir` is
redirected to `Contents/Resources/` because codesign refuses to seal `.pk3` files placed
in `Contents/MacOS/`.

Not done: code signing/notarization for distribution, and a universal binary.

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

- **Gradle must be invoked as `java` directly.** Through a shell wrapper it fails with
  `java.net.SocketException: Operation not permitted` in this sandbox.
- **Use Gradle 8.x.** AGP 8.x rejects Gradle 9, so Homebrew's current Gradle will not work.
- `libvpx` is a hard requirement — the engine's cutscene playback will not link without it.

### macOS

```bash
./macos/build-deps.sh
./macos/build-macos.sh
./macos/package-macos.sh              # signs the bundle
./macos/run-macos.sh                  # picks gamedata/full over gamedata/demo
```

- `package-macos.sh` re-signs but does **not** always recompile. If an engine change seems
  absent from `Selaco.app`, run `build-macos.sh` explicitly.
- **SDL2 needs CMake 4.** Older CMake fails its configure step.

---

## Port decisions worth knowing

**Vulkan only.** There is no OpenGL ES fallback in these builds. Vulkan 1.1+ required.

**`multiDrawIndirect` requirement removed** (`vulkanbuilders.cpp:1835`) — and the original
justification for this was **wrong**, which is worth recording. The claim was that Adreno
reports it false and Vulkan would find zero devices on the Thor. Measured via
`adb shell cmd gpu vkjson`, the Adreno 740 supports `multiDrawIndirect`,
`textureCompressionBC`, `shaderClipDistance` and `shaderCullDistance` — so it would never
have blocked this device. The edit still stands on its own merits: `vkCmdDrawIndirect`
appears **nowhere in the codebase**, so requiring the feature is a real if latent bug that
rejects GPUs which genuinely lack it, some older Intel iGPUs among them. Worth upstreaming,
just not as an Android fix. Same correction applies to `shaderClipDistance`: the
`NO_CLIPDISTANCE_SUPPORT` fallback is never exercised here.

**FluidSynth dropped from ZMusic.** ZMusic 1.3.0 vendors FluidSynth, whose CMake
hard-requires `glib-2.0` on non-Windows platforms, and whose glib shim is Win32-only
(`CRITICAL_SECTION`, `Interlocked*`, `_beginthreadex`). `build-deps.sh` patches it out and
stubs `CreateFluidSynthMIDIDevice`/`fluidConfig`. ZMusic still ships timidity, timidity++,
wildmidi, adlmidi, opnmidi and oplsynth, so MIDI — including soundfont playback via
timidity, which is what `soundfont/` feeds — keeps working. To restore it properly, port the
~150 lines of glib stubs to pthreads and `__atomic_*` builtins rather than cross-compiling
glib.

**`libomp.so` is shipped, not disabled.** `libSelaco.so` has a real `DT_NEEDED` on it: the
engine's `parallel_for` uses OpenMP and NDK clang links `-fopenmp=libomp`, which is not on
the Android system image. `package-apk.sh` packages it from the NDK — about 1 MB, and it
keeps the parallel fast path rather than falling back to the serial one.

**The software renderer stays enabled**, and **BC7 texture compression needed no work** —
the Adreno 740 reports `textureCompressionBC` true.

**`useLegacyPackaging = true`** is required in `app/build.gradle`, or Android reads `.so`
straight from the APK and `nativeLibraryDir` is never populated — which breaks the
adrenotools hooks (see below).

---

## Android storage and permissions

The app's own dir (`/sdcard/Android/data/com.selaco.game/files`, mode 770, group
`ext_data_rw`) is **unreachable by the user** — the Files app and MTP have both refused to
enter `Android/data` since Android 11. It is an adb-only path, and it is deleted on
uninstall along with any 1.2 GB game file in it.

So the game also searches public folders, which needs `MANAGE_EXTERNAL_STORAGE`:

- Manifest declares the permission
- `SelacoActivity.requestAllFilesAccess()` sends the user to
  `Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION` and finishes the activity — it
  is a Settings toggle, not a runtime dialog, so there is no callback to await
- Gated on `!hasGameData()`, so an adb workflow is never prompted
- `gameconfigfile.cpp` adds `/sdcard/Selaco`, `/sdcard/Download` and the
  `/storage/emulated/0` equivalents to IWADSearch and FileSearch
- `READ_MEDIA_*` cannot substitute: `.ipk3` is not a media type

A useful consequence: **anyone who can play has granted the permission.** The ipk3 cannot
reach the app-private dir without adb, and the prompt fires until game data is found.
"Player declined the permission" is not a reachable state for a playable install — only the
adb developer workflow skips it.

### The autoexec must live in EXTERNAL storage

`M_GetAutoexecPath()` returns the external files dir, i.e. `$PROGDIR`. It used to return
internal storage, which meant a freshly generated config recorded

    [Selaco.AutoExec]
    Path=/data/data/<pkg>/files/autoexec.cfg

— a path nothing can write to without adb *and* a debug build. The autoexec therefore
silently never ran on a clean install, which would have hit every user of a release APK. It
only appeared to work during development because those inis predated the fix and had
recorded a different path.

If the `[Selaco.AutoExec]` section exists but is wrong, deleting the Path line is not
enough — `CreateStandardAutoExec` only populates the section when it is *absent*. Delete the
whole section and let the engine regenerate it.

**And the file itself is staged once, never refreshed.** `SelacoActivity` skips the copy when the
target exists (`isConfig && target.exists()`), deliberately, so a player's own edits survive a
reinstall — the pk3s beside it *are* refreshed on a size change, the config is not.

The consequence bites when shipping a config fix: **a changed shipped autoexec reaches new installs
only.** Existing users keep their staged copy. Changing an engine default does not reach them
either, because these cvars are `CVAR_ARCHIVE` and their ini already records the old value. The
escape hatch is deleting the staged file so the next launch re-stages it, which plain
`adb shell rm /sdcard/Android/data/com.selaco.game/files/autoexec.cfg` can do — app-external
storage is reachable by the shell user, so this works even on a non-debuggable release build. A
normal user without adb has to reinstall or use a file manager.

Worth knowing before treating an autoexec edit as a shipped fix: it is a fix for the next person to
install, not for the people who already have it.

### Savegames live in the public folder, and `selaco.globals` needed a migration

`M_GetSavegamesPath()` used to return the app-private *internal* dir. Android deletes that with
the app, so saves were destroyed by an uninstall, by "clear storage", and by the
`INSTALL_FAILED_UPDATE_INCOMPATIBLE` that any signing-key change forces you to resolve that
way — which is the one that matters, because it will happen to anyone installing successive APKs
from a release page.

It now writes to `/sdcard/Selaco/savegames/`, the folder the player already put the ipk3 in.
Writability is **probed, not assumed**: the adb workflow pushes the ipk3 into the private dir, so
`hasGameData()` is true, so `SelacoActivity` never asks for `MANAGE_EXTERNAL_STORAGE` and the
public folder may not be writable. Internal stays as the fallback. The result is cached — the save
menu calls this per redraw and the answer cannot change mid-session.

Savegames needed no migration, because `M_GetSavegamesPaths()` returns the write path plus both
app-scoped dirs and `G_BuildSaveNames` searches all of them.

**`selaco.globals` is the trap.** Unlike savegames it is only ever read from and written to the
*single* write path (`m_misc.cpp:612` load; `:414`, `:177`, `:191` save), so moving that path
silently started the player over from an empty set — the symptom was a fresh 4-byte globals file
next to the real 70-byte one still sitting in internal. `M_LoadDefaults` now folds in a copy left
in any other savegames directory, skipping the write path itself because `M_MigrateGlobalVars`
deletes its source once the destination is written — migrating a file onto itself would delete
what it had just written. **Android-only on purpose:** it deletes what it migrates, and the
desktop path lists are plural by design (Windows returns up to four), so unguarded it would
consume globals out of the player's Documents and Saved Games folders.

Two upstream bugs in that area, both left alone deliberately:

- `M_MigrateGlobalVars` reads the source file into `globalStorage` but then iterates a
  never-populated local `map`, so its documented "adopt the largest value" merge is dead code and
  migration is really a clobber. **Do not "fix" it.** The merge coerces through `ToLong()` and
  `"%d"`, and `_Globals.Set` (`m_misc.cpp:137`) stores raw *strings* — repairing the loop would
  turn every string-valued key into `0`. The accidental clobber is the safer semantic.
- `M_ReadGlobalVars` guards with `!key.Len() == 0`, which parses as `(!key.Len()) == 0`.

---

## The shipped config

There are **two** files and only one of them ships. Confusing them is easy and has already put a
beta build out with the wrong frame cap.

| file | who sees it |
|---|---|
| `android/app/src/main/assets/autoexec.cfg` | **the one that ships.** Packaged into the APK by `package-apk.sh`; the app extracts it on first run (`SelacoActivity.extractAssets`) if absent. This is the only config a player or tester ever gets. |
| `android/configs/thor.cfg` | a dev profile, pushed over the extracted copy by `set-config.sh`, which needs adb. Referenced by no build or packaging step. |

Both are deliberately tiny. **Do not add graphics settings to either** — see CLAUDE.md for why that
has burned this project three times.

| cvar | shipped | thor.cfg | why it is there |
|---|---|---|---|
| `vid_vsync 1` | ✓ | ✓ | not in `CVARINFO.defaults`; the engine default is false |
| `vid_maxfps 30` | ✓ | ✓ | Selaco defaults 200. 30 divides a 60 Hz panel exactly; 35 gives 1.71 vblanks and judders. Needs the relaxed floor in `v_video.cpp` — stock GZDoom clamps up to `GameTicRate` |
| `vid_fps 1` | ✓ | ✓ | on for the beta, so a tester reports a number rather than "choppy". Revisit for release |
| `con_scale 4` | ✓ | | the fps readout above scales by `GetConScale`, which is **1** at 1920x1080 with `con_scale`/`uiscale` unset — unreadable on a 7-inch panel. Not settable from Selaco's menus: `MENUDEF` exposes `ui_scaling`/`hud_scaling`, neither of which feeds this. Non-linear — `(con_scale+1)/2` clamped to 3 here, so 2 changes nothing, 4 gives 2x and 6 the 3x maximum. 6 was tried first and was overbearing; there is no step between them, since 5 also computes to 3x |
| `con_notifylines 0` | ✓ | ✓ | `CVAR_ARCHIVE` — otherwise console text draws over the game |
| `vid_scalefactor 1.0` | | ✓ | `CVAR_ARCHIVE` — the A/B profiles sweep it |
| `con_scale 0` | | ✓ | `CVAR_ARCHIVE` — benchmark profiles set 4, this puts it back to auto |
| `vk_driver ""` | | ✓ | `CVAR_ARCHIVE` — otherwise a dev device keeps loading whatever driver was last set |
| `vk_driver_env ""` | | ✓ | ditto |

The last four are absent from the shipped file on purpose: they exist only to undo what the
benchmark profiles set, and a tester never runs those.

Note the pattern: apart from the first two, **every entry exists only because the cvar is
`CVAR_ARCHIVE` and something else once set it.** An archived cvar left unset is not "default", it
is "whatever the last profile did" — which is how a measurement run came out silently locked at
30 fps, and how the shipped profile nearly went out loading an experimental driver.

`i_benchmark` used to be on this list for exactly that reason. It is no longer `CVAR_ARCHIVE`, so
it cannot persist and needs no defensive line — which is the cheaper fix wherever a cvar is a pure
diagnostic rather than a setting. The tuning cvars beside it (`i_benchmark_interval`,
`i_benchmark_spike`, `i_dipfps`, …) stay archived deliberately: they are inert while `i_benchmark`
is 0, so they cannot surprise anyone.

## Patches carried against upstream

**openal-soft `LoadBufferStatic` overrun** — the important one. In the looping branch,
`remaining` was computed from the unwrapped `dataPosInt`, so when `dataPosInt >= loopEnd`
(which the `intPos` line above it exists to handle) `loopEnd-dataPosInt` underflows as
`size_t` and the mixer reads far past the buffer into a guard page. Segfaulted the audio
thread within a minute of full-game play; the demo never triggered it. Fixed by bounding
with `loopEnd-intPos`. Applied in **both** `android/deps/build-deps.sh` and
`macos/build-deps.sh` — they clone separate trees. Still present in upstream 1.24.3.

The backend is irrelevant to that bug: OpenSL and SDL2 produced identical signatures, and
switching backends only changed how long it took to hit. OpenAL uses the **SDL2** backend
here.

**Accelerometer read as joystick input** (`hardware.cpp`) — menus scrolled down continuously
with nothing touched, because **SDL exposes the Android accelerometer as an extra virtual
joystick and that hint defaults to ON**. Gravity held one axis at roughly +6500 forever,
indistinguishable from a stick being held. Fixed with one line:

```c
SDL_SetHint(SDL_HINT_ACCELEROMETER_AS_JOYSTICK, "0");
```

Found by instrumenting, not guessing: `i_debuginput 1` logged 5019 `JOYAXISMOTION` events in
22 seconds from device `which=1`, while the real gamepad (`which=0`) read all zeros. Note a
gamepad-only axis snapshot **cannot see this** — the accelerometer is not an
`SDL_GameController`, so sampling `SDL_IsGameController` devices skips it entirely.

**Swapchain `preTransform`** (`vulkanswapchain.cpp`) — it passed `caps.currentTransform`
straight into `VkSwapchainCreateInfoKHR::preTransform`, which is a *contract that the
application pre-rotates its own rendering*. The Thor's panel is portrait-native and mounted
rotated, so `currentTransform` is `ROTATE_90` and the engine — which knows nothing about
pre-rotation — drew sideways and stretched inside a correctly-landscape window. Now requests
`VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR` when `supportedTransforms` allows, letting the
compositor rotate.

**ZVulkan surface recreation** — Android destroys the `ANativeWindow` on pause, which
invalidates `VkSurfaceKHR`. Surface *and* swapchain both need recreating; capability queries
return empty caps instead of throwing.

**`vid_maxfps` floor relaxed** (`v_video.cpp`) — stock clamps up to `GameTicRate` (35). 30 is
the only cap that divides a 60 Hz panel evenly; 35 gives 1.71 vblanks and judders.

**`FPSLimit()` runs with vsync on** (`vk_commandbuffer.cpp`) — Vulkan was the only backend
that skipped it when vsync was enabled, making `vid_maxfps` inert in the one configuration a
handheld ships with. GL and GLES already called it unconditionally. Do **not** re-add a
margin to compensate for vsync: `vid_vsync` selects `VK_PRESENT_MODE_FIFO_RELAXED_KHR`,
which presents a late frame immediately.

**Swapchain acquire wait ordered first** (`vk_commandbuffer.cpp`) — a deliberate
**workaround for a driver bug, not a correctness fix**. The old order was legal:
`pWaitSemaphores` is a set of conditions that must all be satisfied, the spec imposes no
ordering, and `AddWait` keeps each `pWaitDstStageMask[i]` paired with its semaphore.

But `FlushCommands` added the internal cross-submit wait before the swapchain acquire wait,
and Turnip's `kgsl_syncobj_merge()` walks the waits in order and steers that ordering into a
branch that dereferences a null queue pointer — a segfault at `tu_queue::device`, offset
**`0x1b0`**. Acquire-first makes Mesa/Turnip work, and is also the conventional order, which
is why emulators and Winlator never hit it.

**Keep this even after the Mesa fix merges.** Drivers reach an Android device via Mesa main →
release → a third-party packager → a file the user downloads, which lags by months.
Reverting would mean Turnip works only on drivers built after the merge and segfaults on
every prebuilt in circulation. The orders are not *exactly* equal — acquire-first costs one
extra `dup`/`close` per merge, ~2 syscalls against an 18–35 ms frame, four orders of
magnitude below anything measurable. Mesa-side analysis is in
`docs/upstream/mesa-kgsl-syncobj-merge.md`.

**Per-frame swapchain semaphores** (`vk_framebuffer.h`) — there was **one** binary
`RenderFinishedSemaphore` and one `SwapChainImageAvailableSemaphore`, correct only with one
frame in flight. With two, frame N's submit would signal `RenderFinished` while frame N-1's
present might not have waited on it — two pending signals on a binary semaphore with no wait
between, undefined per spec.

They are indexed **differently on purpose**, because what makes each safe to reuse differs:

- image-available, by **frame slot**: reuse is gated by `AdvanceFrameSlot` having waited on
  that slot's fences, so the submit that waited on it has definitely executed
- render-finished, by **swapchain image index**: reuse requires having re-acquired that
  image, which is itself proof its previous present completed. A frame index would not prove
  that, since the frame fence signals when rendering ends, not when presentation does

**Explicit fence-outstanding tracking** (`vk_commandbuffer.cpp`) replacing
`mNextSubmit >= maxConcurrentSubmitCount`. That test assumed a fixed submits-per-frame, which
is false — 4 at the default `vk_submit_size`, **79 at 50**, plus mid-frame
`WaitForStreamBuffers` flushes, postprocess and shadowmap passes. `mFenceOutstanding[]` is
cleared **as** each fence is collected, not in a second pass: a frame with more than 8 submits
repeats fence indices, and testing without clearing let every repeat append, so the wait array
could overflow the stack. That corruption surfaces anywhere later and is the likely cause of
two crashes originally attributed to other things.

**Stream/matrix writers rewind to their REGION, not to 0** (`vk_streambuffer.cpp`) —
`ApplyHWBufferSet` reads those offsets and can bind before a frame's first `Write()`, so a
frame in region 1 briefly pointed at region 0, i.e. at another in-flight frame's data.
Presents as "fine standing still, wrong once something changes".

**Writers no longer poison their offset on failure** (`vk_streambuffer.cpp`) — both assigned
the `0xffffffff` sentinel to their offset *before* returning failure. Anything reading it in
between hands a dynamic offset of 4,294,967,295 to `vkCmdBindDescriptorSets`, which faults
inside the driver. Latent at one frame in flight; regions make it reachable. Both writer fixes
are arguably upstreamable to GZDoom on their own.

**ZVulkan `queueFamilyIndex` of `(uint32_t)-1`** (`vulkandevice.cpp:33`) — not
Android-specific, and **Selaco's own bug, not upstream GZDoom's**: upstream (checked at
`c26ce2e6c`) has no `UploadFamily` at all. `UploadFamily` is an `int` defaulting to `-1`, and
the upload-queue sizing indexed with it unguarded:

```cpp
UploadQueuesSupported = selectedDevice.Device->QueueFamilies[UploadFamily].queueCount - rqt;
```

`QueueFamilies[-1]` is an out-of-bounds vector read. The garbage `queueCount` leaves
`UploadQueuesSupported` positive about half the time, and `vkCreateDevice` then receives
`queueFamilyIndex = 4294967295`. Qualcomm's driver silently ignores it; Mesa/Turnip indexes
with it and segfaults in `tu_CreateDevice`. Fixed by guarding on `UploadFamily >= 0`, which is
what `PresentFamily` already did three lines later. Reported in
`docs/upstream/selaco-gzdoom-vulkan-report.md`.

Two things this cost time on: the ~50% hit rate made a single run each way "prove" a false
cause, and the fault address was **identical across two different driver builds**, which looks
like a driver-side constant but was really `qfi` being the same constant every time.
`GraphicsFamily` has the identical latent shape; left alone, as no selected device lacks a
graphics family.

**`i_benchmark`** (`common/engine/i_benchmark.cpp`) — frame-time percentiles to logcat. Uses
`PRINT_HIGH | PRINT_NONOTIFY` so it does not draw its own output and skew what it measures.
Samples engine stats **every frame**, because `GetStats()` rolls the stat's ring buffer as a
side effect (`vmframe.cpp:767`) and calling it once per report made "VM time in last 10 tics"
accumulate a whole window.

---

## Two frames in flight

The largest engine change here, and the largest measured win: **35.7 ms → 27.2 ms (24%)** at a
fixed viewpoint, reproduced three times, with `GPU Wait` collapsing from ~24 ms to **0.08 ms**
and frames over 33.4 ms going from ~133 to **zero**.

`VkCommandBufferManager::framesInFlight` is a **compile-time constant 2**, deliberately not a
cvar.

### What the problem was

The engine was strictly serial: submit, present, then block on a fence before doing anything
else. Measured decomposition of a 34.4 ms frame:

| | ms |
|---|---|
| GPU work | ~24 |
| CPU command recording and setup (`rendertimes` `All` minus `Finish`) | ~7.5 |
| CPU game tick (`i_benchmark`'s `cpu gap`) | ~2.8 |

Those sum to 34.3 serially, exactly what the serial engine measured. Overlapping both CPU
parts approaches `max(10.3, 24)` ≈ 24 ms, and that is what happened.

A Perfetto render-stage trace corroborated it independently: GPU busy 71.6% → 77.4%, idle gap
p90 10.28 → 6.48 ms, and **mean burst unchanged at 1.45 → 1.44 ms** — the GPU does identical
work, only the timing changed, which is the correct signature for a pipelining change rather
than a workload change.

### How it works: one buffer per frame, rotated

Every buffer the CPU writes per frame gets **one buffer per frame in flight**, and rotation is
driven by the frame slot:

| buffer | mechanism |
|---|---|
| `HWViewpointBuffer`, `FLightBuffer`, `BoneBuffer` | `mPipelineNbr` copies, `SetPipelinePos()` |
| `FFlatVertexBuffer` | `mPipelineNbr` copies, `SetPipelinePos()` |
| `VkStreamBuffer` (stream + matrix UBOs) | N internal `UniformBuffer`s, `SetPipelinePos()` |

**Separate buffers rather than regions inside one, deliberately.** Regions were tried first and
worked, but the failure modes are asymmetric: a region overrun is a *legal* write into another
in-flight frame's data, so neither the driver nor the validation layers can see it and it surfaces
only as a one-frame flicker — the hardest symptom to attribute. An overrun of a separate buffer is
out of bounds and can be caught. Costs **98.7 MiB** more at depth 2, nearly all of it
`FFlatVertexBuffer` at 61.04 MiB per copy, which is a good trade for making a class of silent
corruption inexpressible.

The breakdown, derived rather than estimated — and it reproduces `vkbufmem`'s measured 197.3 MiB of
Persistent allocations exactly, which is what makes it trustworthy:

| buffer | per copy | added at depth 2 |
|---|---|---|
| `FFlatVertexBuffer` vertex (2,000,000 × 32 B) | 61.04 MiB | +61.04 |
| Stream UBO (300 × 65,280 B) | 18.68 MiB | +18.68 |
| Matrix UBO (50,000 × 192 B) | 9.16 MiB | +9.16 |
| `FLightBuffer` (80,000 × 64 B) | 4.88 MiB | +4.88 |
| `BoneBuffer` (80,000 × 64 B) | 4.88 MiB | +4.88 |
| `HWViewpointBuffer` (100 × 256 B) | 0.02 MiB | +0.02 |
| | **101.7 MiB** | **+98.66 MiB** |

Earlier figures of "~71 MB" and "~93 MB" were both wrong: 71 counted only the four engine buffers
and predates the two `VkStreamBuffer`s being pipelined.

**96% of that is one oversized constant.** `FFlatVertexBuffer::BUFFER_SIZE` is 2,000,000 vertices,
while `mIndex` on a real level is ~102,000 — the buffer is provisioned ~20× above the static
high-water mark, and depth 2 doubles the cost of that pessimism. Before shrinking it, measure: there
is no instrumentation for the actual per-frame peak, and `AllocVertices` turns an overrun into
`I_FatalError` rather than a wasted page. A max-tracker on `mCurIndex` would give the real headroom.

Three things that make this work, each of which broke it first:

- **The descriptor set must bind the ACTIVE buffer.** `VkBufferManager::CreateDataBuffer` assigns
  `ViewpointUBO`/`LightBufferSSO`/`BoneBufferSSO` on *every* call, so with N buffers those cached
  pointers hold whichever was constructed last. `UpdateHWBufferSet` therefore reads
  `screen->mViewpoints->GetBuffer()` and friends. Binding the cached pointer bound the wrong buffer
  on every frame the rotation did not land on the last one — severe flickering.
- **Rotation must be driven by the frame slot, not by `Clear()`.** The descriptor set is written
  once per frame in `BeginFrame`, while `Clear()` runs later and repeatedly during the frame, so
  rotating there leaves the descriptor pointing at a buffer other than the one being written.
  `VulkanRenderDevice::BeginFrame` calls `SetPipelinePos()` on all of them immediately before
  `mDescriptorSetManager->BeginFrame()`. `Clear()` skips its own rotation once `mExternalPipeline`
  is set, so the GL backend is unaffected.
- **`FFlatVertexBuffer` never actually rotated upstream.** `mPipelinePos` is set once in the
  constructor and only used by `Copy()` to seed the reserved quads into every copy. GL survives
  that because writing a buffer the GPU is reading makes the driver rename it implicitly; Vulkan
  has no such behaviour, so real rotation had to be added. It needs no descriptor work — it is
  bound with `vkCmdBindVertexBuffers` via `GetBufferObjects()`, and `VkRenderState` rebinds when
  the handle changes.

**`SetSubData` needed fixing too**, and this is easy to miss because it is not one of the obvious
per-frame buffers. It memcpy'd into a **shared** staging buffer and then recorded a `copyBuffer`,
so frame N+1 could overwrite the staging data before frame N's copy executed — the frame-end fence
used to prevent exactly that. It now allocates a transient staging buffer per call and hands it to
`TransferDeleteList`. The only caller is the shadowmap AABB tree, at most twice per frame and only
when dynamic geometry moved, so the allocation is cheap and the bug would have been a rare
one-frame shadow glitch.

Note what does **not** need duplicating: render targets, depth buffers and anything else only the
GPU writes. Submissions are ordered on one queue, so two frames never execute simultaneously — the
overlap is CPU recording against GPU execution. Only CPU-written data needs a second copy.

### Why it is not settable

It was a cvar so the change could be A/B'd. Changing it at runtime is a **use-after-free**:
`AdvanceFrameSlot` stops retiring slots at the same moment `FinishFrameWait` starts clearing
them, tearing down retained delete lists while a frame is still executing. That surfaced as
`SIGABRT` in `scudo::reportInvalidChunkState` from `tu_FreeDescriptorSets` — a double free
Qualcomm's driver silently tolerated and **Turnip, built against the scudo heap checker, aborts
on**. Toggling on Qualcomm "worked" for several rounds and hid it. A constant removes the whole
class of bug and deleted more code than it added.

### The GPU profiler cannot be used with it

`debug.graphics.gpu.profiler.perfetto` makes the Adreno driver take an instrumented path that
neither survives nor correctly observes two frames in flight. Three captures, all degenerate:
**1 submission, 5.3–5.4% GPU busy, 0.05 ms mean bursts**, uniform from the first 500 ms window,
so no capture length or timing helps. It also crashed the process 3/3 times inside
`vulkan.adreno.so`'s own `vkCmdBindDescriptorSets`.

The captures completed fine — 1.44 MB, full span — so this is wrong *data*, not lost data. The
tell is a trace reporting a few percent GPU busy for a scene known to be GPU bound. **To
capture a GPU trace you must build with `framesInFlight = 1`.**

Per-group GPU timings (the `gpu` stat) **do** work, but they were not free. The timestamp pool
carries one range per frame slot, and a slot's results are read at the top of a frame, after
`AdvanceFrameSlot` has waited that slot's fence — so the read never blocks. That was the whole
problem: reading at the end of a frame means reading a pool a frame still executing is writing,
and `VK_QUERY_RESULT_WAIT_BIT` there would stall on the GPU and give back exactly the time
pipelining bought. The cost of doing it safely is that the figures are **two frames old**, since a
slot comes round every other frame at `framesInFlight = 2`.

### The static flat vertices are per-slot, and `mIndex` is the range that matters

`FFlatVertexBuffer::Copy()` used to write into **all** `mPipelineNbr` buffers in a loop. That was
safe when `mPipelineNbr` was 1 on Vulkan and every frame ended with a full `vkWaitForFences`; with
two frames in flight it is neither. The buffers are `Persistent`, permanently mapped, and
`Upload()` is a no-op on Vulkan, so seeding a non-current slot is a host write into memory a
submitted command buffer is reading as vertex data. Reachable from `OutputResized()` →
`Copy(4, 4)` via `DFrameBuffer::Update` on any resize, `vid_scalefactor`/`vid_scalemode` change, or
Android resume.

It now writes only the current slot and marks the others, each refreshed at its next
`SetPipelinePos` — which on Vulkan is after `AdvanceFrameSlot` has waited on that slot's fence.

**The range to refresh is `[0, mIndex)`, not `NUM_RESERVED`.** This is the whole trap, and it cost
seven device builds. `mNumReserved` is 20 — the uniform quad, fullscreen quad, present quad and two
stencil caps — so "reserved" reads like the entire front-of-buffer region. It is not.
`CreateVBO` (`hw_vertexbuilder.cpp:490-496`) appends **all static sector geometry** to
`vbo_shadowdata`, sets `mIndex = vbo_shadowdata.Size()`, and calls `Copy(0, fvb->mIndex)` — around
**102,000 vertices** on a real level. That is the third caller of `Copy()`, it lives in a different
file from the other two, and it is the only one whose range is not tiny.

Refreshing only 20 vertices left the other slot holding none of the level's static flats. The two
slots alternated every frame, which presents as **lit surfaces flickering while standing
still** — it reads as a dynamic-light bug, and it is not.

Four other explanations were investigated and falsified before the real one was found; recorded so
nobody spends the builds again:

- slot divergence on `FULLSCREEN_INDEX` — no, the flicker persisted with both write paths
  producing byte-identical data
- `Map()`/`Unmap()` disturbing the mapping — no, both are no-ops for `Persistent` buffers
- `HWViewpointBuffer::mLastMappedIndex` — no, tested alone and clean
- the act of writing a non-current slot from `BeginFrame` — no, tested with contents held correct
  and a redundant write added, and that is clean

**Also fixed here:** `Copy()` copied from `&vbo_shadowdata[0]` regardless of `start`, so
`OutputResized`'s `Copy(4, 4)` wrote the `QUAD_INDEX` marker quad over the fullscreen quad it had
just computed. Verified on device as a single-line change with no regression — though that is all
it establishes, since the bug is latent. Upstream GZDoom carries the same line; worth upstreaming.

If you touch this again: `Copy()` has **three** callers and one of them is in
`hw_vertexbuilder.cpp`. Grep the whole tree, not just `flatvertices.*`.

### The wipe path, and why it is dead code in a shipped Selaco

`PerformWipe` (`common/2d/wipe.cpp`) loops `Begin/Run/End/screen->Update()` **without**
`BeginFrame`, so `Update()` would rotate nothing and reuse a slot still executing.
`VulkanRenderDevice` carries an `mFrameBegun` flag for it (`vk_renderdevice.cpp:1100`, set at
`:1559`): when `Update()` runs unpaired, it does a full wait instead of an advance.

**Selaco never executes it.** `wipetype` ships as `0` (`wipe_None`) via the game's own
`CVARINFO.defaults`, and `d_main.cpp:1225` skips straight to `End2DAndUpdate()` when
`wipe_type == wipe_None`. No level transition will reach `PerformWipe`, so playing the game is
not a test of this fix — a full playthrough proves nothing either way. The path is reachable only
from a cutscene that calls `System_SetTransition` (`d_main.cpp:3238`), which forces a type
independently of `wipetype`, or from a player who sets the cvar by hand.

To actually exercise it, edit `wipetype=1` into `selaco-ea.ini` **with the game stopped** (a clean
exit rewrites the ini from memory and would undo the edit; `am force-stop` skips that, which is
what makes the edit stick). Do not try to do it from the console — printable characters need SDL
text input and are unreachable over adb. Note `d_main.cpp:1014` wipes on *any* gamestate change,
so loading a save from the title screen is enough; a level exit is not required. Restore the cvar
afterwards, or a clean exit archives melt as the player's permanent setting.

Verified on device this way: the "Now Loading" melt rendered and a full level run followed with no
hang, no crash and no `DEVICE_LOST`.

---

## Performance

### Where the frame goes

DeckLow, fixed viewpoint, T30 driver, two frames in flight: **31.4 ms (31.8 fps)**. The frame is
**GPU bound** — `All` 29.7 ms with `GPU Wait` 0.045 ms, i.e. the CPU no longer waits at all.

Single-variable sweep from DeckLow, one menu step down each:

| change | saving |
|---|---|
| `gl_ssao` 2→0 | **3.3 ms** |
| `gl_light_shadowmap` off | **2.0 ms** |
| `gl_bloom` off | **1.0 ms** |
| `gl_ssao` 2→1 | 0.3 ms |
| `gl_shadowmap_quality` 512→256 | 0.3 ms |
| `r_fogeffects` off | 0.0 ms |

Those three account for **6.3 of the 6.4 ms** gap between DeckLow and Low, so the other ten
differing cvars are noise at this viewpoint.

**SSAO is effectively a binary cost on a tiler.** 2→1 saves almost nothing because any non-zero
`gl_ssao` puts the whole scene into a 3-draw-buffer G-buffer pass (`hw_entrypoint.cpp:146`) —
every fragment writes SceneColor RGBA16F + SceneFog RGBA8 + SceneNormal A2R10G10B10, and on a
tiler the store lands at tile resolve where no draw-list timestamp can see it. That invisibility
is why an earlier note wrongly recorded SSAO as nearly free.

A static viewpoint **cannot** measure `r_particleIntensity`, `r_smokequality`, `r_BloodQuality`,
`cl_maxdecals` or `r_rainquality`. Zero there means "not in this scene", not "free".

### Solid geometry is not fragment bound

> These per-draw-list figures were measured at **one frame in flight**, before pipelining landed.
> The shipped code can reproduce them again now that the `gpu` stat works at two frames in flight,
> but the numbers are not directly comparable: they are two frames old, and the pipelining they
> predate changed the frame's shape. Re-measure rather than diffing against this table. See
> [The GPU profiler cannot be used with it](#the-gpu-profiler-cannot-be-used-with-it).

`plainflats` (13.5 ms) and `plainwalls` (7.65 ms) dominate GPU time, and at the same viewpoint
1920x1080 → 960x540 moved `plainwalls` only **1.19×** and `plainflats` **1.12×**, while `ssao`
scaled 4.6× as a fullscreen pass must. So walls and floors are geometry, draw-submission or
per-draw-setup bound — which is why a 4× pixel cut only bought ~25%, and why settings sweeps find
so little. **The remaining headroom is structural, not a preset.**

Resolution is still the largest single quality-for-speed lever: `vid_scalefactor` 1.0 → 0.75
measured ~18%. Not shipped — the softness on a 7-inch panel was judged not worth it. Two traps:
never measure it under a frame cap (a cap hides the entire effect), and Selaco's Video menu lists
only `1, 1.25, 1.5, 1.75, 2`, so opening that page snaps 0.75 back to 1.0.

`vk_submit_size` is fully swept (50 to 4000): `GPU Wait` falls monotonically with submission count
but frame time is U-shaped, and the **1000 default is the minimum**. Nothing left there.

**An arm64 ZScript JIT is not worth building.** The VM is ~1 ms of a 33 ms frame and ~3.8 ms of a
47 ms explosion frame. `HAVE_VM_JIT` is x86_64-only (`CMakeLists.txt:223`), the vendored asmjit has
no ARM backend at all, its API is a generation out of date, and the 5819-line emitter has 312
`x86::` references. Upstream GZDoom has not done it either.

### Post-load spikes are ZScript, not autosave

A 130–265 ms frame appears shortly after each map load. Per-frame stat sampling attributed it:
**404k VM calls** against ~82k in a normal window, a 40 ms single-tic VM peak, and think time
essentially idle. A ZScript burst from level-init scripts — not a save, not rendering.

A map load itself shows up as a single ~1.3 s frame (~2996 thinkers, ~500 ms of Think, GPU idle).
`i_benchmark_ignore` excludes frames above 500 ms and reports them separately.

---

## Replacement Vulkan drivers (Mesa/Turnip)

Drop a Turnip build named **`vulkan.so`** next to the game data and it is picked up automatically;
`vk_driver` still overrides with an explicit path. Prebuilts work — MrPurple's T30 and stevenmx's
have both been run.

The engine reads the library's **`DT_SONAME`** and stages the copy under *that* name, which is what
makes renaming to `vulkan.so` safe: Android's linker keys libraries by soname, every Turnip build's
soname is its original filename, and a copy staged under any other name fails to load.

### Is it faster? Only once the CPU is off the critical path

Fixed viewpoint, GFXPresetLow, all four combinations:

| | 1 frame in flight | 2 frames in flight |
|---|---|---|
| Qualcomm | 35.7 ms (28.7 fps) | 27.2 ms (36.8 fps) |
| Turnip T30 | 36.2 ms (27.6 fps) | **25.0 ms (39.9 fps)** |

**At one frame in flight the two drivers are within noise of each other here.** An older note
claimed ~27% for Turnip in steady play; that was a different scene and a hand-played route, and it
does not reproduce at a heavy fixed viewpoint.

Pipelining helps Turnip more (31%) than Qualcomm (24%), and that is **not explained**. The naive
read of `GPU Wait` at depth 1 (Qualcomm 23.7, Turnip 24.3–25.6 ms) says Turnip's GPU work is
slower, which contradicts it being 2 ms faster once pipelined. Candidates: higher per-submit CPU
cost hidden at depth 2, or Turnip benefiting more from back-to-back submissions. Settling it needs
a render-stage trace, which requires a `framesInFlight = 1` build.

Treat it as experimental: an init failure falls back to the system driver, but a crash *after* init
cannot be recovered from.

### Why adrenotools and not a direct load

Android ships Vulkan drivers as HAL modules. Loading one directly *works* — dlopen, read `HMI`,
validate its `'HWMT'` tag, call `open()`, take `GetInstanceProcAddr` — and that was built and
tested here. But the driver then advertises only **8** instance extensions, and the only surface
type among them is `VK_EXT_headless_surface`. `VK_KHR_surface`, `VK_KHR_android_surface` and
`VK_KHR_swapchain` are implemented by Android's Vulkan **loader** on top of the driver's
`VK_ANDROID_native_buffer`, so `CreateInstance` fails with "extension not present" and there is
nothing to present to.

adrenotools keeps `libvulkan.so` and hooks only the loader's driver lookup, so WSI still comes from
the loader. Through it the same driver advertises **14** extensions including `VK_KHR_surface` and
`VK_KHR_android_surface` — that count is the quick check that the load worked. Static linking would
not have helped: the driver genuinely has no surface support. `libhardware-stub/` is left over from
that attempt and is unused.

### Four things that each silently broke it

- **Linker namespace.** An app may only dlopen from its APK `lib/` or internal data dir; `/sdcard`
  is outside the namespace *and* noexec. `I_StageVulkanDriver` (sdlglvideo.cpp) copies the driver
  into internal storage first.
- **SONAME.** The staged copy must be named after the library's `DT_SONAME`, not its filename.
- **Trailing slash.** `adrenotools_open_libvulkan` concatenates `customDriverDir` and
  `customDriverName` with no separator, so the directory must end in `/`. Without it the `stat`
  check fails and it returns null *before* logging anything.
- **`useLegacyPackaging = true`.** Required, or `nativeLibraryDir` — where the hooks must live — is
  never populated.

`hookLibDir` is derived with `dladdr` on one of our own functions: `dli_fname` gives the full path
of `libSelaco.so`, whose directory *is* `nativeLibraryDir`. No JNI needed.

### Building a driver — optional, for debugging only

`./android/build-turnip.sh [debug]`. A prebuilt works fine, so build one only to *debug* a driver:
prebuilts ship `-fno-unwind-tables -Dstrip=true`, which is why five tombstones in a row stopped at
frame `#00` and taught us nothing. A debug build turns a bare SIGSEGV into an abort naming the
violated invariant with a full backtrace.

Staging is by **name**, so delete the stale staged copy after a rebuild
(`adb shell run-as com.selaco.game rm -f files/<name>.so`) or the old driver keeps loading.

Four host-specific things break this build on macOS, all handled in the script: Xcode's **bison is
2.3** and rejects `-Wcounterexamples` (brew's 3.8.2 required, and meson bakes the absolute path at
configure time so it needs a re-`setup`, not a re-`ninja`); Mesa needs **Python ≥ 3.10** with
`mako`, `pyyaml` **and `packaging`** (without the last, Mesa falls back to the removed `distutils`
and dies before it looks at mako); **BSD `sed -i` needs an explicit empty suffix**; and the native
file must say **darwin/aarch64**.

`TU_DEBUG=noconform` does nothing on this device — `a7xx_base` sets `has_hw_multiview = True`, so
its only use in Mesa short-circuits on an Adreno 740. It has been wrongly recorded as significant
twice, both times from testing one run each way. Repeat counts, or nothing.

---

## Diagnostics

### Crash logs

`i_crashlog.cpp` installs handlers for SIGSEGV/SIGABRT/SIGBUS/SIGFPE/SIGILL and writes signal,
code, fault address, tid and a backtrace with **module + offset** — the form `llvm-symbolizer`
needs.

- It **chains**: after writing, it restores the previous handler and re-raises, so bionic's
  debuggerd still produces the tombstone and the logcat dump. Installing a handler naively *costs*
  you the system record, which is why `i_main.cpp` skips the generic crashcatcher on Android.
- Path is chosen by **trying to open** each candidate, since storage permission is granted at
  runtime: `/sdcard/Selaco` → `/storage/emulated/0/Selaco` → `/sdcard/Download` → `$PROGDIR`. A
  public folder matters because `Android/data` is adb-only and is wiped on uninstall. The `$PROGDIR`
  fallback exists for the **adb developer** case, where game data is already app-private so the
  permission is never requested.
- **Appends**, one block per crash: with an intermittent fault the pattern across runs is the
  evidence.
- Timestamps are **UTC**, and everything in the handler path is async-signal-safe — `write(2)` only,
  integers formatted by hand. Local time would need `localtime_r`, which is not safe in a handler.
- Test it with `crashout` (an `UNSAFE_CCMD` that exists for this) or
  `adb shell run-as com.selaco.game kill -11 <pid>`. Bindings do **not** fire at the title screen,
  so `crashout` needs an in-game session.

### FPS dips

`fpsdips` prints frames below `i_dipfps` (default 26) with the worst frame; `fpsdips reset`
restarts. Counted on **every** frame regardless of `i_benchmark`, so a tester needs no setup. 26
rather than 30 deliberately: at a 30 fps cap a perfect frame is 33.3 ms, and jitter around the cap
would log thousands of false dips. Map loads are counted separately as stalls, reusing
`i_benchmark_ignore`.

### On device

```bash
adb logcat -s selaco-ea:V | grep BENCH                       # frame times
adb shell run-as com.selaco.game cat files/selaco-ea.ini      # live config
adb shell cat /sdcard/Selaco/selaco-ea-crash.log             # crashes
```

`run-as` needs a **debug** build. The ini flushes on clean exit and on some menu actions —
`force-stop` is SIGKILL and writes nothing, so quit from the in-game menu before diffing config
state. adb's daemon dies intermittently here (`cannot connect to daemon`); `adb kill-server` and
retry, it is environmental rather than a device fault.

Symbolizing a crash: the unstripped `.so` in `android/deps/prefix/arm64-v8a/lib/` matches the
tombstone's BuildID. Use the NDK's `llvm-symbolizer --obj=<lib> 0x<pc offset>`. This is what turned
"OpenAL crashes somewhere" into an exact line.

### Running Vulkan validation

`vk_debug 1` alone is close to useless, for three reasons that each fail silently.

**The layer is not on the device and the NDK no longer ships one** (r21+ dropped it). Fetch the
Android build from the Vulkan-ValidationLayers releases and drop the arm64 `.so` into
`android/app/src/main/jniLibs/arm64-v8a/`, which the loader searches because the debug build is
debuggable. That directory is gitignored, so nothing is committed; it adds ~26 MB to the APK, so
take it back out when finished:

    curl -sSL -o /tmp/vvl.zip https://github.com/KhronosGroup/Vulkan-ValidationLayers/releases/download/vulkan-sdk-1.4.357.0/android-binaries-1.4.357.0.zip
    unzip -o -j /tmp/vvl.zip '*/arm64-v8a/libVkLayer_khronos_validation.so' -d android/app/src/main/jniLibs/arm64-v8a/

**Synchronization validation is a layer feature, not the layer.** Enabling the layer gives core
validation only. `vulkaninstance.cpp` now chains a `VkValidationFeaturesEXT` asking for it.

**`vk_debug` lives in the ini, and the ini has more than one plausible home for it.** Console text
input is unreachable over adb, so it must be set in `selaco-ea.ini` with the game **stopped** — a
clean exit rewrites the ini from memory and undoes the edit. Set the existing `vk_debug=` line, the
one next to `vk_debug_callstack` and `vk_driver`. Adding a second `vk_debug=` elsewhere in the file
does nothing and looks exactly like it worked. Editing on the device is its own trap: toybox `sed`
writes a literal `n` for `\n` and rejects the `a` command, so pull the file with
`adb exec-out run-as … cat`, edit on the host, and copy it back via `/sdcard`.

**Prove the layer loaded before believing any result.** Expect
`added global layer 'VK_LAYER_KHRONOS_validation'` from the `vulkan` tag, and no
`requested but not installed` line. Better, run a **positive control**: temporarily add
`VK_VALIDATION_FEATURE_ENABLE_BEST_PRACTICES_EXT` alongside sync validation. It fires on any real
engine, so its output proves layer → messenger → callback → logcat works end to end. A silent run
that has not been controlled this way is not evidence of anything — that mistake was made here
twice, once from an unset `vk_debug` and once from a mangled `logcat -t` marker that returned zero
for the hazards *and* the control.

Count hazards as a **delta across exactly one extra launch** (`Creating Vulkan device` occurrences),
not as an absolute: the logcat buffer rolls within minutes under validation, and `logcat -c` is
forbidden here.

What the first run found, on a title screen and one loaded level:

| Hazard | Count/launch | Status |
|---|---|---|
| `WRITE_AFTER_WRITE`, mipmap barrier access masks | 12 refs | fixed, now 0 |
| `READ_AFTER_WRITE`, transfer→draw on `VkHardwareBuffer.Stream` | 7 | fixed, now 0 |
| `WRITE_AFTER_WRITE`, PP renderpass `loadOp` vs layout transition | 4–5 | fixed, now 0 |
| `VUID-vkDestroyBuffer-buffer-00922`, staging buffer freed unwaited | 9 | fixed, now 0 |

All three were the same shape: a barrier or dependency that described a write as a read. **All
synchronization hazard classes now report zero**, on a title screen, a loaded level and play.

It also found a **use-after-free that was not a sync hazard**, and this one was in fork code:
`VUID-vkDestroyBuffer-buffer-00922`, nine per launch, freeing a `VkHardwareTexture.mStagingBuffer`
still referenced by `mTransferCommands`. A staging buffer joined `TransferDeleteList` at upload
time while the command buffer recording its copy only joined at the next flush — so a slot rotation
between the two split them across lists with independent fences, and the buffer's slot was stamped
`LastSerial = 0` (the 64 MB path's `DropRetainedFrames` zeroes it), which the fence loop treats as
"nothing of mine outstanding". Two slots freed 260 buffers each having waited on nothing. Fixed by
flushing pending transfers before retiring their resources.

Reaching it needs a load: `FlushBackground` drives `UploadLoadedTextures` outside the frame loop,
so no end-of-frame flush puts the command buffer in the same list. Steady-state play never does it.

**Two wrong turns worth remembering**, both caught only by instrumenting rather than reasoning:

- The `LastSerial == 0` diagnosis was declared *falsified* on a `tail`-limited view of the log —
  the load-time events had scrolled past, and a full grep found them. The theory was right; the
  sampling was wrong.
- After the fix, cumulative counts still showed 1 unwaited free and 9 destroy errors. Both were
  the *previous* launch still in the buffer. Cut the log at the last `Creating Vulkan device` and
  count per launch; `logcat -c` is forbidden here.

**Nothing was reported against the frames-in-flight machinery** — no hazard on fences, semaphores,
delete lists or slot rotation, during play or at teardown. Coverage gap worth closing: no
level-to-level map change has been run under validation, only title → `SE_01A`.

---

## The second screen, and reading Selaco's codex from C++

The AYN Thor is a 3DS-style clamshell: both panels face the player, so a codex on the lower one is
useful rather than decorative. Display 4 (`local:4630946482288158084`, 1240x1080, `FLAG_PRESENTATION`)
is the target.

### The panel does not involve the renderer at all

`AuxPanel.java` shows an `android.app.Presentation` and draws an ordinary `View`; native pushes a few
primitives to it over JNI. **Nothing Vulkan is created, submitted, presented or waited on**, so the
renderer is bit-identical to a build without the feature — which is the entire safety argument. An
optional screen must not be able to take the main one down.

Two designs that *did* add a second Vulkan surface were built out on paper and both failed adversarial
review: one on an illegal readback (the hardware canvas image is created `COLOR_ATTACHMENT | SAMPLED`
at `vk_hwtexture.cpp:254`, with no `TRANSFER_SRC`), one on recording an aux blit outside the submit
meant to synchronise it. Prior art confirms the hazard is real rather than theoretical — azahar's own
secondary present thread can starve its frame pool and freeze the *main* screen, survived only by
keeping a hidden `VirtualDisplay` permanently alive, which costs every single-screen user a full extra
render and present per frame. Do not reopen that approach without reading those findings.

**`FLAG_NOT_FOCUSABLE` is mandatory, for a reason specific to this engine.** If the panel takes focus,
`SDL_WINDOWEVENT_FOCUS_LOST` sets `AppActive = false` (`sdlglvideo.cpp:1129-1131`) and `D_Display`
then returns early (`d_main.cpp:945-948`) — freezing **both** screens while the process runs perfectly.
`FLAG_NOT_TOUCHABLE` is a *separate* bit and is not set, so touch still works. `FLAG_KEEP_SCREEN_ON`
must be on the Presentation's own window or the panel sleeps, and a sleeping panel screencaps pure
black, which reads as a broken renderer.

Verified on device: the Presentation composites **above** `rip.moth.cocoonshell`'s activity, which
already owned display 4. Neither prior-art codebase handles a foreign owner, so this was inference
until tested.

### Reading ZScript state from C++ without shipping any ZScript

Selaco's codex content is **not** code: `/MANUAL.json`, a 42,172-byte lump, read by ZScript through
`Wads.CheckNumForFullName` — thin wrappers over the engine's own C++ file system. Unlock state is a
`Map<Name, Int> unlocks` on `ManualItem : Inventory`, written by `Unlock()` as
`insert(key, had ? 1 : 2)`.

C++ can read that map directly, and it is an established in-tree pattern rather than a layout hack:
`ZSMap` derives publicly from `TMap` (`scripting/core/maps.h:19`) and is placement-constructed into
the field's storage (`types.cpp:2513`), and `vmnatives.cpp:63` already declares
`static ZSMap<FName, DObject*> AllServices;` in C++ and hands the same bytes to ZScript as
`Map<Name, Service>`. Field lookup follows `maploader.cpp:1256`. So no ZScript of ours ships, there is
no pk3 competing with Selaco's, and a non-Selaco load degrades to a blank panel because every lookup
returns null rather than erroring.

**Use `CheckKey`, never `Map.Get`.** `Get` is the *inserting* accessor: calling it would mutate the
player's unlock map, corrupt their save, and make Selaco's own codex print `???` for sections it should
show.

**The gate test is `!= 0`** — `manual.zs:105-109` and `:176-184`. Three other sites look like the
visibility predicate and are not: `:86` (`> 1`) drives the NEW badge, `:121` feeds `showLocked`, and
`:124` feeds the `???` teaser. Picking one of those is how you leak or over-hide.

### The spoiler rules, which are the acceptance criterion

Showing a locked entry is worse than showing nothing, so every branch resolves to LOCKED:

- **Version gate.** The manual is refused outright unless `"version"` is exactly `1.0`.
- **Key whitelist.** Sections carry `title`/`sections`/`entries`/`id`/`unlock`; entries carry
  `title`/`content`/`unlock`/`noheader`. Any other key means Selaco may have added gating we do not
  model, so that node *and its subtree* are locked. Without this, a new gate key such as `requires`
  would read as "no gate" and publish locked content — the fail-open drift a review caught.
- **Conjunctive visibility.** Three entries — Overview, Workbench, Safe Room Extension — carry no
  unlock key of their own and are hidden solely by their section's `safesect` gate. Per-node filtering
  leaks "Safe Room Extension", which names a mechanic the player has not found.
- **Gate names come from the file**, never a hardcoded list. `safesect` is section-level and would be
  missed by any list derived only from entry unlocks.

Measured on the shipped file: **51 nodes, 17 gates, 21 of 40 entries and 9 of 10 sections visible with
nothing unlocked**, rising to 22 entries with `keypad` open. Those numbers were predicted from the JSON
before deploying and matched exactly — which is what caught a bug that did not crash:
`Nodes[self].Children.Push(BuildNode(...))` evaluates the subscript into a reference before the
recursive call reallocates the array, so every child link was written through a dangling reference. It
reported `visible=0` against a correctly parsed tree, which reads as "the predicate is too strict"
rather than "the tree is corrupt". **Predict the count before believing a run.**
