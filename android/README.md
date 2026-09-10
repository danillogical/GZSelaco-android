# Android port

Builds GZSelaco for Android arm64-v8a, targeting the **Ayn Thor** (Snapdragon 8 Gen 2 / Adreno 740,
Android 13). Renderer is **Vulkan**.

See [../TECHNICAL.md](../TECHNICAL.md) for architecture, port decisions and the patches
carried against upstream.

## Build

```sh
./android/deps/build-deps.sh     # once: SDL2, ZMusic, libvpx, openal-soft -> deps/prefix/
./android/build-android.sh       # host tools, then libSelaco.so
./android/package-apk.sh         # strip, stage, assemble the APK
```

Then:

```sh
adb install -r android/app/build/outputs/apk/debug/app-debug.apk
```

### Game data

The APK ships only the engine's own `.pk3`s, which are extracted on first run. Selaco's game data is
commercial and must be supplied by the user:

```sh
adb push Selaco.ipk3 /sdcard/Android/data/com.selaco.game/files/
```

That directory is the app's own external files dir, so it needs **no runtime permission** on
Android 13. It is also what the engine uses as `progdir`.

## Layout

| Path | Purpose |
|---|---|
| `host-tools/` | Standalone CMake project building `re2c`, `lemon`, `zipdir` for the **build** machine. The engine shells out to these at compile time and they cannot be cross-compiled. |
| `deps/build-deps.sh` | Fetches and cross-compiles the four external libraries into `deps/prefix/<abi>/`. |
| `build-android.sh` | Two-stage driver: host tools, then the engine as `libSelaco.so`. |
| `package-apk.sh` | Strips the libraries (118 MB → 21 MB), stages `jniLibs`/`assets`, copies SDL's Java sources, assembles. |
| `app/` | Gradle project. `SelacoActivity extends SDLActivity`. |

Everything under `deps/src`, `deps/build`, `deps/prefix`, `app/src/main/jniLibs`,
`app/src/main/assets` and `app/src/main/java-sdl` is generated and gitignored.

## Environment

Defaults point at the Homebrew Android command-line tools; override with env vars if yours differ.

```sh
ANDROID_NDK=/opt/homebrew/share/android-commandlinetools/ndk/28.2.13676358
CMAKE_BIN=/opt/homebrew/share/android-commandlinetools/cmake/3.22.1/bin/cmake
ANDROID_ABI=arm64-v8a
ANDROID_API=26
```

`package-apk.sh` uses `./gradlew` if present, else `gradle` on PATH. Do **not** use Homebrew's
Gradle 9.x — AGP 8.9 requires Gradle 8.x. There is no wrapper JAR checked in yet; generate one with
`gradle wrapper --gradle-version 8.13`.

## Terminology

"Launcher" is overloaded in this project - both senses appear in these docs:

- **Android launcher** - the home screen / app drawer. Tapping the app icon is
  `adb shell monkey -p com.selaco.game -c android.intent.category.LAUNCHER 1`.
- **GZDoom launcher** - the ZWidget IWAD-picker window shown before the game window
  on desktop (`src/launcher/*.cpp`). It does not exist in the Android build; see
  TECHNICAL.md.

## Notes for anyone touching the build

- **`-Wl,-z,nostart-stop-gc` is not optional.** The NDK links with `--gc-sections`, and without that
  flag lld can collect the `areg`/`creg`/`freg`/`greg`/`yreg`/`vreg` sections that hold every
  ZScript class, CVar and action-function registration. It fails **silently** — no link error, the
  game just boots with nothing registered.
- ZWidget, the GZDoom launcher (the IWAD-picker window - not the Android home screen), discord-rpc
  and `crashcatcher.c` are excluded on Android.
- Vulkan and OpenAL are **not** linked: `volk` and `DYN_OPENAL` `dlopen` them at runtime.
- `libomp.so` **is** a real dependency and is copied out of the NDK by `package-apk.sh`.
