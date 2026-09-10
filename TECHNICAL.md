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

---

## The shipped config

`android/configs/thor.cfg` is the shipped autoexec and is deliberately tiny. **Do not add graphics
settings to it** — see CLAUDE.md for why that has burned this project three times.

| cvar | why it is there |
|---|---|
| `vid_vsync 1` | not in `CVARINFO.defaults`; the engine default is false |
| `vid_maxfps 30` | Selaco defaults 200. 30 divides a 60 Hz panel exactly; 35 gives 1.71 vblanks and judders |
| `vid_fps 1` | on for the beta, so a tester reports a number rather than "choppy". Revisit for release |
| `i_benchmark 0` | ours, `CVAR_ARCHIVE`, so it stays on across launches unless stated |
| `con_notifylines 0` | `CVAR_ARCHIVE` — otherwise console text draws over the game |
| `con_scale 0` | `CVAR_ARCHIVE` — benchmark profiles set 4, this puts it back to auto |
| `vk_driver ""` | `CVAR_ARCHIVE` — otherwise the *shipped* profile keeps loading whatever driver was last set |
| `vk_driver_env ""` | ditto |

Note the pattern: apart from the first three, **every entry exists only because the cvar is
`CVAR_ARCHIVE` and something else once set it.** An archived cvar left unset is not "default", it
is "whatever the last profile did" — which is how a measurement run came out silently locked at
30 fps, and how the shipped profile nearly went out loading an experimental driver.

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
out of bounds and can be caught. Costs ~93 MB more at depth 2, nearly all of it `FFlatVertexBuffer`
at 61 MB per copy, which is a good trade for making a class of silent corruption inexpressible.

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

Per-group GPU timings (the `gpu` stat) are also unavailable, because the timestamp pool is
shared between overlapping frames and reading it with `VK_QUERY_RESULT_WAIT_BIT` would block on
the GPU — reintroducing exactly the stall this removes. Restoring them means per-slot query
pool ranges.

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
