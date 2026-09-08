Selaco game data
================

Put Selaco's game data here. Nothing in demo/ or full/ is committed - the data is
commercial and large (the demo ipk3 alone is 235 MB), so .gitignore excludes
everything except this file and the .gitkeep markers.

    gamedata/demo/    the free demo
    gamedata/full/    the retail / Early Access game

The build scripts do not read these directories automatically. They are a staging
area, and the launch commands at the bottom point at them.


IMPORTANT: engine files vs game files
-------------------------------------

Selaco distributions ship BOTH the game data AND a copy of the GZDoom engine
resources. Only the game data belongs here.

  DO put here          Selaco.ipk3, game_support.pk3, game_widescreen_gfx.pk3,
                       soundfonts/, fm_banks/

  DO NOT put here      gzdoom.pk3, lights.pk3, brightmaps.pk3

The engine resources must come from THIS build, not from the distribution. They
are version-locked to the engine: our build reports g4.13pre-724-g95130c3a8,
while the demo shipped with a 2023-era engine. Mixing them causes subtle
resource-version failures. Our build produces its own gzdoom.pk3, lights.pk3 and
brightmaps.pk3, and on macOS they are placed inside Selaco.app/Contents/Resources
automatically.

Likewise ignore everything Windows-specific in a distribution: SELACO.exe,
*.bat, and the bundled *.dll files (openAL32.dll, zmusic.dll, libsndfile-1.dll,
libmpg123-0.dll, libfluidsynth*.dll). We build or ship our own equivalents.


gamedata/demo/  - expected files
--------------------------------

Verified against "SELACO_DEMO_-_V0" (contents dated 2023-06-27):

  REQUIRED
    Selaco.ipk3                 ~235 MB   the IWAD. 13906 entries, all-PNG
                                          textures (9598 .png, 1706 .ogg,
                                          298 .obj). No .dds, so this demo does
                                          NOT exercise the BC7 texture path.

  RECOMMENDED
    game_support.pk3            ~1.7 MB   2514 lumps
    game_widescreen_gfx.pk3     ~5.3 MB   214 lumps

  OPTIONAL
    soundfonts/                           MIDI soundfonts
    fm_banks/                             OPL/FM instrument banks
    selaco-demov2.ini                     SEED THE ENGINE CONFIG FROM THIS - see
                                          "Suppressing the "configuration file is out of date" warning
----------------------------------------------------------

Selaco nags on the title screen:

    Your configuration file is out of date or does not exist.
    Please delete your user INI file and reinstall the game to fix this error.

The check is `OnlyIfNot g_iniexists` (MENUDEF.COCK:54). `g_iniexists` is declared
`nosave bool ... = false` in CVARINFO (nosave maps to CVAR_CONFIG_ONLY), and the
shipped ini sets it to true in [Selaco.ConfigOnlyVariables.Mod]. This engine build
does NOT apply that value - verified on macOS, where a config containing
g_iniexists=true still reports `"g_iniexists" is "false" (default: "false")`. So it
is a demo/engine version mismatch, not a port problem, and it is cosmetic.

DO NOT fix it by copying the shipped ini over the engine's config. That ini's
[IWADSearch.Directories] is Windows-oriented (. / $DOOMWADDIR / $HOME / $PROGDIR)
and replaces the platform defaults, so on macOS the game then cannot find its own
data.

Instead put this in autoexec.cfg, which is verified to work:

    g_iniexists true

autoexec.cfg is read from $PROGDIR, i.e.:
    macOS     Selaco.app/Contents/Resources/autoexec.cfg
    Android   /sdcard/Android/data/com.selaco.game/files/autoexec.cfg


Graphics presets
----------------

Selaco ships Steam Deck presets, and on Android they appear automatically because
the menu gates them on IfOption(Unix) (MENUDEF.txt:198) and clang defines __unix__
for linux-android.

    GFXPresetDeckHigh = "Steam Deck: Favour Quality"    (QUALITY_DECK_ONE)
    GFXPresetDeckLow  = "Steam Deck: Favour Performance" (QUALITY_DECK_TWO)

Start with Favour Performance on the Thor. The presets were tuned for the Steam
Deck's 1280x800 (1.02 MP); the Thor is 1920x1080 (2.07 MP) - just over twice the
pixels - on a tile-based mobile GPU that pays more than the Deck's RDNA2 for
full-screen postprocess passes.

If you want more fidelity, prefer lowering resolution over raising the preset:
vid_scalefactor 0.75 gives ~1440x810 (1.17 MP, Deck-ish), which makes Favour
Quality plausible. vid_fps 1 shows the frame counter so this can be measured.

Those presets live in a custom mouse-driven dialog a gamepad cannot fully
navigate (there is no touch input yet), so setting the cvars via autoexec.cfg is
currently the reliable route. See report-android-port.md.


Running
-------

macOS, against the demo:

    ./build-macos-arm64/Selaco.app/Contents/MacOS/Selaco \
        -iwad gamedata/demo/Selaco.ipk3 \
        -file gamedata/demo/game_support.pk3 gamedata/demo/game_widescreen_gfx.pk3 \
        +logfile /tmp/selaco.log

Reading /tmp/selaco.log is the only reliable way to see what happened - the
engine reports fatal errors in a modal dialog, and startup detail never reaches
stdout.

Alternatively, drop the files in the engine's own search path and pass no
arguments:

    ~/Library/Application Support/selaco-ea/          (macOS)

Android - push to the app's external files dir, which needs no runtime
permission on Android 13:

    adb push gamedata/demo/Selaco.ipk3 \
        /sdcard/Android/data/com.selaco.game/files/

That directory is what i_main.cpp sets progdir to on Android, so the engine picks
files up from there with no arguments.
