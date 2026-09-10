# macOS port

Builds GZSelaco for macOS (Apple Silicon by default), rendering with **Vulkan via MoltenVK**.

See [../TECHNICAL.md](../TECHNICAL.md) for architecture and cross-platform decisions.

## Build

```sh
brew install cmake                # SDL2 needs CMake >= 3.24
./macos/build-deps.sh             # ZMusic, libvpx, openal-soft, MoltenVK
./macos/build-deps.sh sdl2        # SDL2 (the backend this build uses)
./macos/build-macos.sh            # -> build-macos-<arch>/Selaco.app
./macos/package-macos.sh          # self-contained, ad-hoc signed bundle
```

## Run

Stage game data under `gamedata/demo/` or `gamedata/full/` (see `gamedata/README.txt` for the
expected files), then:

```sh
./macos/run-macos.sh              # prefers gamedata/full, falls back to demo
./macos/run-macos.sh demo
./macos/run-macos.sh demo -- +map MAP01
```

It always passes `+logfile` and prints the errors on exit, because the engine reports fatal errors in
a modal dialog and startup detail never reaches stdout.

Alternatively drop `Selaco.ipk3` into `~/Library/Application Support/selaco-ea/` and launch the app
with no arguments.

**Verified:** loads the 2023 Selaco demo (235 MB ipk3, 13432 lumps) and reaches the main menu in
~2 s, rendering through MoltenVK. Audio does not work yet — see TECHNICAL.md.

## Notes

- **SDL2 backend, not Cocoa.** Selaco's auxiliary-GL-context API was never added to
  `cocoa/gl_sysfb.h`, so `OSX_COCOA_BACKEND=ON` does not compile. See the plan for detail.
- **No Vulkan SDK needed.** volk `dlopen`s `libMoltenVK.dylib`, which `build-deps.sh` pulls from the
  Khronos release.
- **`dlopen` ignores `LC_RPATH`.** MoltenVK and OpenAL are loaded by name, so the bundle-relative
  `@executable_path/../Frameworks/...` fallbacks in `volk.c` and `oalsound.cpp` are what make the
  packaged app self-contained.
- **Non-code goes in `Contents/Resources`.** `codesign` rejects data files in `Contents/MacOS`;
  `package-macos.sh` relocates them and `i_main.cpp` points `progdir` there for bundled builds.
- Signing is ad-hoc by default. `CODESIGN_IDENTITY="Developer ID Application: ..."` for something
  distributable (notarization is a separate step).
- Universal binaries: build each arch separately and `lipo`. Do **not** set
  `CMAKE_OSX_ARCHITECTURES="arm64;x86_64"` globally — a single compile pass for both arches makes
  arch-conditional defines like `ARCH_IA32` and `HAVE_VM_JIT` impossible to set correctly.
