# GZSelaco — Android and macOS

A personal fork of [GZSelaco](https://github.com/TheCockatrice/GZSelaco) (the Selaco
engine, itself a GZDoom fork) that adds **Android** and **macOS** builds.

Built and tested on an **AYN Thor** (Snapdragon 8 Gen 2, Android 13, Vulkan 1.3) and
on Apple Silicon macOS. It runs the full retail Selaco at a locked 30 fps on the Thor.

You need to own Selaco. **No game data is included here** — you bring your own
`Selaco.ipk3` from your Steam install.

---

## Install on an AYN Thor

1. Download `Selaco-android.apk` from [Releases](../../releases) and install it.
2. Find `Selaco.ipk3` in your Steam folder:
   `steamapps/common/Selaco/Selaco.ipk3` (~1.2 GB)
3. Copy it to the Thor at **`Internal storage/Selaco/`**.
   Any method works — USB file transfer, an SD card, a cloud download on the device.
4. Launch Selaco. It will ask for file access — tap **Allow access to manage all
   files**, then reopen Selaco.
5. Pick your settings in the first-run dialog.
   **Steam Deck: Favour Performance** and **Favour Spectacle (Steam Deck Optimized)**
   are the tested combination and hold 30 fps.

That's it. The Thor is detected as a Steam Deck automatically, which gives you the
larger UI, larger subtitles and aim assist that Selaco ships for handhelds.

> **Updating:** install the new APK *over* the old one. Don't uninstall first —
> that would delete anything in the app's private folder. Your `Internal
> storage/Selaco/` copy is safe either way.

## Install on other Android devices

Same steps, with two caveats:

- **Vulkan 1.1+ is required.** There is no OpenGL ES fallback in this build.
- **A gamepad is strongly recommended.** Touch input is not implemented, so on a
  device with no physical controls you cannot play. Selaco's own on-screen keyboard
  handles text entry, but everything else needs buttons.

Only the Thor has been tested. Other hardware may need different graphics settings —
pick a lower preset in the first-run dialog if it struggles.

## Install on macOS

Apple Silicon only (arm64), macOS 11+. Nothing is pre-built, so you compile it:

```bash
./macos/build-deps.sh        # SDL2, ZMusic, OpenAL, MoltenVK, codecs
./macos/build-macos.sh       # the engine
./macos/package-macos.sh     # -> build-macos-arm64/Selaco.app, signed
```

Then put your game data in `gamedata/full/` and run:

```bash
./macos/run-macos.sh
```

Or double-click `Selaco.app` after copying (or symlinking) `Selaco.ipk3` into
`~/Library/Application Support/Selaco-EA/`.

Runs on Vulkan via MoltenVK. Mouse and keyboard work normally, and so do Xbox
controllers over USB or Bluetooth.

---

## Known gaps

- **No touch input.** Android needs a gamepad.
- **No OpenGL ES fallback.** Vulkan only.
- **macOS has no pre-built release.** Build it yourself.
- The Thor dips to roughly 25 fps in heavy explosions. That is translucent overdraw,
  not a bug — see [CLAUDE.md](CLAUDE.md) if you care why.

## Technical detail

Everything about how the ports work — the build system, the Android storage and
permission model, performance measurements, and the patches carried against upstream —
is in **[CLAUDE.md](CLAUDE.md)**.

Also useful:

- `android/configs/README.md` — graphics profiles and what each setting costs
- `report-android-port.md` — decisions and dead ends from the Android port
- `plan-android-port.md`, `plan-macos-port.md` — the original plans
- `gamedata/README.txt` — which game files go where

---

## License and credits

GPL v3, inherited from GZDoom. See the license files for individual contributor
licenses.

- GZDoom — Copyright © 1998-2023 ZDoom + GZDoom teams, and contributors
- Doom source — Copyright © 1997 id Software, Raven Software, and contributors
- Selaco and the Selaco engine changes — Altered Orbit Studios / TheCockatrice

Selaco itself is a commercial game and is **not** distributed here.

Upstream resources: [zdoom.org](https://zdoom.org/) ·
[wiki](https://zdoom.org/wiki/) · [forum](https://forum.zdoom.org/) ·
[Discord](https://dsc.gg/zdoom)
