# CLAUDE.md

**Read [TECHNICAL.md](TECHNICAL.md) before changing anything in `src/`, `android/` or
`macos/`.** It covers how the ports work, every patch carried against upstream, and the
performance findings. This file is only the things that will bite you while working here.

This is a **personal fork** of GZSelaco adding Android (arm64) and macOS (arm64) targets.
Not upstreamed, no contributions expected. It merges from upstream GZDoom, so every change
is a permanent delta: prefer a targeted `#ifdef __ANDROID__` guard over a forked file, and
comment *why*, not what.

---

## Build commands

```bash
./android/build-android.sh && ./android/package-apk.sh
cd android && java -Xmx4g \
  -classpath /tmp/gradle-8.13/lib/gradle-launcher-8.13.jar \
  org.gradle.launcher.GradleMain --no-daemon assembleDebug
adb install -r android/app/build/outputs/apk/debug/app-debug.apk
```

**Gradle must be invoked as `java` directly** — a shell wrapper fails with
`java.net.SocketException: Operation not permitted`. Use Gradle **8.x**; AGP 8.x rejects 9.

macOS: `./macos/build-deps.sh`, `build-macos.sh`, `package-macos.sh`, `run-macos.sh`.
`package-macos.sh` re-signs but does not always recompile — if an engine change seems
absent, run `build-macos.sh` explicitly.

---

## Sandbox constraints

- **Never run adb from a `.sh` wrapper.** It dies with `could not install *smartsocket*
  listener: Operation not permitted` while a bare `adb` in the same shell works — the
  wrapper adds a process layer. Run the adb lines directly. This silently sent zero
  keyevents for an entire measurement sweep once.
- `unzip` on `Selaco.ipk3`: entries use **backslash** separators and unzip treats `\` as an
  escape. Quote with doubled backslashes:
  `unzip -o -q Selaco.ipk3 'ACTORS\\Effects\\SMOKE.zsc' -d out`
- Gradle: see above.

---

## Do not do these

**Do not add graphics cvars to `thor.cfg`.** It is the shipped autoexec. Three separate
times a graphics block here silently overrode the player's in-menu choices, and demo-era
values rotted invisibly when the full game renamed cvars (`r_effectLOD` deleted,
`reflections`→`r_reflections`, `r_gamedetail`→`g_gamedetail`, `r_permanentblood` split).

A cvar belongs there only if Selaco's menus and defaults cannot express it. Note that
**every current entry except `vid_vsync` and `vid_maxfps` exists only because the cvar is
`CVAR_ARCHIVE` and something else once set it.** An archived cvar left unset is not
"default", it is "whatever the last profile did" — which is how a measurement run came out
locked at 30 fps, and how the shipped profile nearly went out loading an experimental driver.

**Do not reconstruct a graphics preset from an ini.** The authoritative source is the
`OptionValue "GFXPreset..."` block in `MENUDEF.zsc` inside `Selaco.ipk3`. An ini snapshot
**omits every cvar sitting at its engine default**, so rebuilding from one silently drops
entries — it dropped seven from a DeckLow alias here and the result was visibly not DeckLow.
Also note **DeckLow is heavier than Low**: of the 13 cvars that differ, DeckLow takes the
more expensive value in all 13.

**Do not bypass the first-run dialog.** `g_tos 1` skips it, and that skips
`SetSteamdeckPresets()` plus ~45 preset cvars. Doing so caused three separate bugs: walking
instead of running, 20x view bob, and a stale gamepad layout with no weapon-wheel binding.
The full game's dialog is gamepad-navigable. Let it run.

**Do not `adb logcat -c` in a measurement loop.** It destroyed the evidence for a crash
here. Reading the *last* marker and the *last two* windows is already correct on an
uncleared buffer.

**Do not assume `setdefault` is broken.** Selaco ships a `CVARINFO.defaults` lump (~65
entries) that changes stock GZDoom defaults, and this engine implements it
(`d_main.cpp:1738`). `cL_run = true` and `movebob = 0.0126` are in there — never set those
by hand. The engine's raw `movebob` default of 0.25 is ~20x Selaco's own maximum.

**`nosave` does not mean "not saved".** In CVARINFO it maps to `CVAR_CONFIG_ONLY`, which per
`c_cvars.h:75` means *not in savegames and not over the network*. Those cvars **do** persist
in the ini.

---

## Measuring performance

**Measure at a FIXED VIEWPOINT or do not bother.** Per-draw-list cost swings enormously with
camera direction — `plainwalls` measured **0.12 ms** in one frame and **12.64 ms** in
another, purely from looking down a corridor versus at a floor. Comparing those between
hand-played runs is meaningless, and doing it invalidated three separate conclusions in one
afternoon.

Use `android/configs/thor-fixedview-ab.cfg`: load a savegame, stand still, and change one
setting at a time with `adb shell input keyevent 131` (F1) and so on. Baseline reproduces to
**±0.1 ms**, which is what makes single-variable A/B possible at all.

Four ways this has been silently invalidated:

- **Keyevents fail silently.** Always confirm the echoed `BENCHMARK_SET` marker names the
  test you meant. Invalid key names also fail silently (`KPPlus` is not valid; `KP+` is), and
  bindings do not fire with the console open or at the title screen.
- **A key bound twice** — the last binding wins, with no warning. That made an eight-window
  sweep measure one configuration while appearing to measure eight.
- **Changing anything else mid-run.** Switching the second screen off between two readings
  moved the frame ~2.7 ms, the same size as the effect being measured, and produced two false
  results in a row.
- **Not restoring the baseline.** The test aliases set one cvar and do not undo the previous
  one, so re-press F1 before *every* test.

**One run is never evidence.** A ~50% failure rate made a single run each way "prove" a false
cause twice in this project (`TU_DEBUG=noconform`, and a driver theory). Repeat counts, or
nothing.

**Debug keybinds persist.** GZDoom serialises bindings into the ini, so a bind set by a dev
config survives switching configs. `thor-clean-binds.cfg` restores them; only **F6** has a
stock default (`quicksaveselaco`).

---

## Working notes

`docs/scratch/` is gitignored — local scratch only, so never reference it from a tracked
file. Durable findings belong in TECHNICAL.md.
