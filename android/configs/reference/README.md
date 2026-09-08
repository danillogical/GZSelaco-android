# Reference snapshots

Captured device state, kept for comparison. These are **not** loaded by anything -
`set-config.sh` only offers `*.cfg` files one directory up.

## `thor-fullgame-selaco-ea.ini`

The full `selaco-ea.ini` pulled from the Ayn Thor on 2026-09-07, after the setup that
finally played well on the handheld. Pull a fresh copy with:

    adb shell run-as com.selaco.game cat files/selaco-ea.ini > out.ini

That needs a debug build (`run-as`), and the ini only flushes when the game exits
cleanly - a `force-stop` is a SIGKILL and writes nothing, so quit from the in-game
menu first or you will be reading a stale file.

### How this state was reached

1. `g_tos` reset to 0 so Selaco's first-run dialog ran again.
2. The engine defaults `g_steamdeck` on for Android (d_main.cpp, after
   `ParseCVarInfo`), so `UIHelper.TestForSteamDeck()` returned true and Selaco applied
   its own handheld profile. Its automatic check cannot fire here - it wants a
   1280x800 screen (helper.zs:675) and this is 1920x1080.
3. `GFXPresetDeckLow` + `VisibilityPresetSpectacleDeck` picked in the dialog.
4. `android/configs/thor.cfg` supplies the few platform settings Selaco cannot know.

### Steam Deck profile - `SetSteamdeckPresets()`, helper.zs:616

Nine cvars, not the five an earlier note in this repo claimed. The aim-assist trio is
part of it, which is easy to miss because the function reads as UI-only:

| cvar | value | |
|---|---|---|
| `ui_scaling` | 1.2 | huge UI |
| `hud_scaling` | 1.2 | |
| `hud_meter_scaling` | 1 | |
| `snd_subtitlesize` | 4 | huge text |
| `snd_subtitlebg` | true | |
| `g_hudopacity` | 0 | |
| `AIMASSIST_ENABLE` | true | helper.zs:643 |
| `AIMASSIST_STYLE` | 1 | helper.zs:645 |
| `AIMASSIST_STRENGTH` | 0.15 | helper.zs:648 |

These only apply while `IntroHandler.needsTOS()` is true, i.e. during the first-run
dialog. Setting `g_steamdeck` later does nothing for them - reset `g_tos` to 0 and let
the dialog run.

### GFXPresetDeckLow, as stored

| cvar | value | | cvar | value |
|---|---|---|---|---|
| `r_shadowQuality` | 1 | | `r_smokequality` | 2 |
| `gl_texture_filter` | 6 | | `r_particleIntensity` | 2 |
| `gl_light_shadowmap` | true | | `r_particleLifespan` | 1 |
| `gl_shadowmap_quality` | 512 | | `r_BloodQuality` | 1 |
| `gl_shadowmap_filter` | 0 | | `lightingquality` | 2 |
| `gl_fxaa` | 0 | | `r_fogeffects` | 1 |
| `gl_ssao` | 2 | | `gl_bloom` | true |
| `r_waterquality` | 1 | | `r_filmgrain` | 0 |
| `r_rainquality` | 1 | | `gl_lens` | false |
| `r_reflections` | 0 | | `g_gamedetail` | 3 |
| `cl_maxdecals` | 500 | | `r_mirror_recursions` | 2 |
| `r_ThingLOD` | 700 | | `r_smokeDensity` | 1 |
| `smokeeffects` | 0 | | | |

Bools serialise as `true`/`false` while the preset lists them as `1`/`0`; that is not
a mismatch. `r_drawplayersprites` is absent from the ini and defaults to 1.

### From our autoexec

| cvar | in thor.cfg | in this snapshot | |
|---|---|---|---|
| `vid_vsync` | 1 | true | |
| `vid_maxfps` | 30 | **35** | drifted - see below |
| `i_benchmark` | 0 | 0 | |
| `cl_run` | 1 | true | Selaco never sets this; a fresh config walks |
| `movebob` | 0.0126 | 0.0126 | Selaco's max; the engine default 0.25 is ~20x it |

### Two things to read carefully

**`vid_maxfps` is 35 here, not the 30 the autoexec sets.** Most likely the Max FPS
menu item was cycled during the session; the autoexec puts it back to 30 on the next
launch. Worth knowing that 35 is *worse* than 30 on this panel - 60/35 is 1.71
vblanks, so frames alternate between one and two and it reads as judder, whereas 30 is
exactly two.

**`vid_scalemode` is 5 (Custom), which is correct, not drift.** Selaco's
`ResolutionOption` (MENUDEF.zsc:1683) expresses a chosen resolution as Custom mode
plus explicit dimensions, and `vid_scale_customwidth/height` are 1920x1080 here. So
this is native. Only worry if those two numbers are not the panel size.
