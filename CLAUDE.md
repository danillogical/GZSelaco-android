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

**Every exit code in that chain lies. Gate on the artifact.** `package-apk.sh` exits **1** on
success, because its last step tries to run gradle and cannot find it — so a real failure and a
normal run look identical. Gradle then reports `BUILD SUCCESSFUL in 3s` against a tree it did not
rebuild, and `adb install -r` reports `Success` installing the stale APK. Three green-looking
signals, nothing deployed. Compare the packaged library against the freshly built one and refuse
to test unless they match:

```bash
DISK=$(stat -f %z android/app/src/main/jniLibs/arm64-v8a/libSelaco.so)
APKSZ=$(unzip -l android/app/build/outputs/apk/debug/app-debug.apk \
  | awk '/lib\/arm64-v8a\/libSelaco.so/{print $1}')
[ "$DISK" = "$APKSZ" ] || echo "STALE APK - do not test"
```

Do not compare mtimes — APK entries are normalised to 1981. This burned a whole device test:
the source was 41 seconds newer than the APK, the change was absent from the binary, and the
resulting "the feature does not work" reading sent me diagnosing `CreatePath` instead. Confirm a
change is *in* the APK (`strings` on the `.so` for a new literal) before concluding anything from
device behaviour.

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

## adb commands that answer about the wrong thing

Each of these returns a confident, plausible answer to a question you did not ask. None of them
error, and two of them made new code look broken when the tooling was at fault.

**`screencap` captures the wrong display.** This device has two: the game's internal panel is
SurfaceFlinger display `4630946441858561667` (1920x1080), and there is a second at
`4630946482288158084` (1240x1080) which `cocoonshell/.ExternalDisplayActivity` sits on. Bare
`adb shell screencap -p` defaults to the **second** one, so you get a correctly-formed screenshot
of the wrong screen. Use `-d 4630946441858561667`, and note `-d` wants a SurfaceFlinger id from
`dumpsys SurfaceFlinger --display-id`, not the `0`/`4` that `dumpsys window displays` shows. Treat
those ids as this device's current values rather than constants — re-derive them if a capture comes
back the wrong size, and cross-check which one the game is on with
`dumpsys window displays | grep -B2 mFocusedApp`.

Second trap on top of that: if the panel is asleep, capturing the right display returns **pure
black**. That reads as "the renderer is broken" rather than "the device is asleep" — check
`dumpsys power | grep mWakefulness` before concluding anything, and `input keyevent KEYCODE_WAKEUP`
first. Screenshots are also how the second screen bites you a second time; see the frame-time
warning under Measuring performance.

**`appops set <pkg> <op> deny` sets only the PACKAGE mode, and the framework reads the UID mode.**
`Environment.isExternalStorageManager()` consults the uid mode, so a package-level deny changes
nothing observable and the app behaves exactly as if the permission were still granted. Use
`appops set --uid <pkg> <op> deny`, and confirm with `appops get` — it prints both, and the line
you want is `Uid mode:`. This made a genuine first-launch crash look like it did not reproduce,
which nearly got the finding dismissed as a misreading of the framework contract.

**`adb uninstall` then `adb install` IS NOT A CLEAN INSTALL.** `allowBackup="true"` is set
deliberately in the manifest, so Android's auto-backup restores the old app data about two seconds
after the install finishes — the ini, the extracted `autoexec.cfg`, everything. Both commands print
`Success` and nothing warns you. A "clean" install done this way came up with a device already
recorded and a stale config, which read as two separate bugs in new code and cost an hour. The tell
is in logcat: `BackupManagerService: restoreAtInstall pkg=com.selaco.game`, followed by
`restoreFinished`. To actually get a fresh install: `adb shell bmgr enable false`, uninstall,
install, then `adb shell pm clear com.selaco.game`, and **put backup back with `bmgr enable
true`** — it is the only protection the saves have. Saves themselves live in
`/sdcard/Selaco/savegames/`, which is not app-specific storage and survives all of this; the ini is
internal and only reachable through `run-as com.selaco.game` on a debug build.

**But do not gate on that line's count being 0 — it fires even when nothing is restored, and
`bmgr enable false` does not suppress it.** A clean install done exactly as above still logged one
`restoreAtInstall`, which reads as the trap having struck when it had not. The field that answers the
question is on the same line: **`restoreSet=0` means there was nothing to restore**, and a non-zero
set is the case to worry about. The authoritative check is neither — it is that the data directory is
empty, which is one command and cannot be misread:

```bash
adb shell run-as com.selaco.game ls -A /data/data/com.selaco.game/   # must print nothing
```

**A shipped `autoexec.cfg` change never reaches an existing install.** `extractAssets` writes the
config once and then skips it forever if the file exists (`SelacoActivity.java`, the `isConfig`
branch) so the player's edits survive a reinstall. The consequence is that editing
`android/app/src/main/assets/autoexec.cfg` does nothing on any device that has already run the
game, and the APK and the device can disagree indefinitely. Compare sizes rather than assuming —
a device carrying a 5850-byte config while the APK ships 6436 is the signature. Delete the file on
the device to pick up the shipped one. The pk3s do not have this problem: they are refreshed
whenever the size differs.

---

## Do not do these

**Do not add graphics cvars to `thor.cfg`.** It is the shipped autoexec. Three separate
times a graphics block here silently overrode the player's in-menu choices, and demo-era
values rotted invisibly when the full game renamed cvars (`r_effectLOD` deleted,
`reflections`→`r_reflections`, `r_gamedetail`→`g_gamedetail`, `r_permanentblood` split).

A cvar belongs there only if Selaco's menus and defaults cannot express it. The current
contents and the justification for each are tabulated in
[TECHNICAL.md](TECHNICAL.md#the-shipped-config). Note the pattern there: apart from vsync,
the frame cap and the beta fps readout, **every entry exists only because the cvar is
`CVAR_ARCHIVE` and something else once set it.** An archived cvar left unset is not
"default", it is "whatever the last profile did" — which is how a measurement run came out
locked at 30 fps, and how the shipped profile nearly went out loading an experimental driver.

**Do not reconstruct a graphics preset from an ini.** The authoritative source is the
`OptionValue "GFXPreset..."` block in `MENUDEF.zsc` inside `Selaco.ipk3`. An ini snapshot
**omits every cvar sitting at its engine default**, so rebuilding from one silently drops
entries — it dropped seven from a DeckLow alias here and the result was visibly not DeckLow.
Also note **DeckLow is heavier than Low**: of the 13 cvars that differ, DeckLow takes the
more expensive value in all 13.

**Do not bypass the first-run dialog by hand — and `g_tos 1` does not even do that.**
`IntroHandler.needsTOS()` compares against `TOS_ID`, which is `2`, so `g_tos 1` leaves the
dialog pending; `TOS_ID` is also a `PSymbolConstNumeric` that `RemoveUnusedSymbols()` strips
before any console command could read it, so there is no cvar value that skips the dialog —
only Selaco's own `clearNeedsTOS()` clears it, and a device ini shows `g_tos=2` written by
that call, never a `1`. What actually caused the three bugs once attributed to `g_tos 1`
(walking instead of running, 20x view bob, a stale gamepad layout with no weapon-wheel
binding) was `SetSteamdeckPresets()` and the preset block never running, by whatever means
the dialog got skipped. Only bypass the dialog through something that redoes that work
itself and enforces the ordering — which is what the device picker in
[TECHNICAL.md](TECHNICAL.md#the-device-picker) does now, deliberately, in place of Selaco's
two dialogs. Do not reintroduce a hand-rolled `g_tos`/`aux_device` write that skips it
without also calling `SetSteamdeckPresets()` and applying a real preset.

**Do not `adb logcat -c` in a measurement loop.** It destroyed the evidence for a crash
here. Reading the *last* marker and the *last two* windows is already correct on an
uncleared buffer.

**Do not assume `setdefault` is broken.** Selaco ships a `CVARINFO.defaults` lump (~65
entries) that changes stock GZDoom defaults, and this engine implements it
(`d_main.cpp:1804`). `cL_run = true` and `movebob = 0.0126` are in there — never set those
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
