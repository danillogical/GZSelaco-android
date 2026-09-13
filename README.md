# Selaco for Android and macOS

An unofficial port of [Selaco](https://store.steampowered.com/app/1592280/Selaco/) to
**Android** and **macOS**, built on the Selaco engine (itself a GZDoom fork).

Tested on an **AYN Thor** (Snapdragon 8 Gen 2, Android 13) and on Apple Silicon Macs.

**You need to own Selaco.** No game data is included — you bring your own
`Selaco.ipk3` from your Steam install.

---

## Install on Android

1. Download `Selaco-android-<version>.apk` from [Releases](../../releases) and install it.
2. Find `Selaco.ipk3` in your Steam folder:
   `steamapps/common/Selaco/Selaco.ipk3` (~1.2 GB)
3. Copy it to **`Internal storage/Selaco/`** on your device — the folder your file manager
   shows as "Internal storage", i.e. `/sdcard/Selaco/`. USB, an SD card or a download on the
   device all work.
4. Launch Selaco. It asks for file access — tap **Allow access to manage all files**, then
   reopen it.

   This is what lets the game read the `Selaco.ipk3` you just copied, since it sits outside
   the app's own private folder, and it lets your savegames live in `Selaco/savegames/` where
   they survive uninstalling or updating the app. Android has no narrower permission that
   covers this: the media-only permission cannot read an `.ipk3`, and anything the app can
   reach without asking is deleted along with the app.
5. Pick your settings in the first-run dialog. **Steam Deck: Favour Performance** plus
   **Favour Spectacle (Steam Deck Optimized)** is the tested combination.

The Thor is detected as a handheld automatically, which gives you the larger UI,
larger subtitles and aim assist that Selaco ships for Steam Deck.

**Requirements:** Vulkan 1.1+ (there is no OpenGL fallback) and **a gamepad** — touch
controls are not implemented, so a device with no physical buttons cannot play. Only
the Thor has been tested; other hardware may need a lower preset.

**Updating:** install the new APK *over* the old one — that keeps your settings and key
bindings, which live in the app's private storage and are the one thing an uninstall does
destroy. Your savegames are safer: they live in `Internal storage/Selaco/savegames/`
alongside the `Selaco.ipk3`, so they survive uninstalling, reinstalling, or clearing the
app's storage.

## Install on macOS

Apple Silicon (arm64), macOS 11+. There is no pre-built download — you compile it:

```bash
./macos/build-deps.sh     # dependencies
./macos/build-macos.sh    # the engine
./macos/package-macos.sh  # -> build-macos-arm64/Selaco.app
```

Put `Selaco.ipk3` in `gamedata/full/` and run `./macos/run-macos.sh`. Mouse, keyboard
and Xbox controllers all work.

---

## Reporting a problem

**If it crashes,** just relaunch and keep playing — crash details are saved
automatically and each crash is appended to the same file, so nothing is lost.

Send us **`Internal storage/Selaco/selaco-ea-crash.log`** — the same folder as your
game data. Note roughly when it happened and what you were doing; timestamps in the
file are UTC.

**If it feels choppy,** open the console (`~` on a keyboard, or the on-screen keyboard)
and type:

```
fpsdips
```

That prints how many frames ran below 26 fps while you played, and the worst one.
`fpsdips reset` starts the count over. Include that with any performance report, along
with your graphics preset.

---

## Optional: a faster graphics driver (Turnip)

Adreno devices can use Mesa's open-source Turnip driver instead of Qualcomm's. On the
Thor it measured about **8% faster** with no loss of image quality.

1. Download a Turnip build for your device (MrPurple's and stevenmx's both work).
2. Extract the driver and **rename it to `vulkan.so`**.
3. Put it in **`Internal storage/Selaco/`**, next to your game data.

It is picked up automatically on the next launch. To go back, delete `vulkan.so`.

> **Experimental.** This is an unofficial driver. If a level fails to load or the game
> crashes, delete `vulkan.so` and it reverts to the system driver.

---

## Known gaps

- **No touch controls.** A gamepad is required on Android.
- **Vulkan only** — no OpenGL fallback.
- **No pre-built macOS release.** Build it yourself.
- Heavy combat can still dip below 30 fps on the Thor.

---

## For developers

Build instructions, the Android storage and permission model, performance
measurements and the patches carried against upstream are in
**[TECHNICAL.md](TECHNICAL.md)**. [CLAUDE.md](CLAUDE.md) covers the traps that bite
while working in the tree. Graphics profiles used for testing are in
`android/configs/`.

---

## License and credits

GPL v3, inherited from GZDoom.

- GZDoom — © 1998-2023 ZDoom + GZDoom teams and contributors
- Doom source — © 1997 id Software, Raven Software and contributors
- Selaco and the Selaco engine — Altered Orbit Studios / TheCockatrice

Selaco is a commercial game and is **not** distributed here.

[zdoom.org](https://zdoom.org/) · [wiki](https://zdoom.org/wiki/) ·
[forum](https://forum.zdoom.org/) · [Discord](https://dsc.gg/zdoom)
