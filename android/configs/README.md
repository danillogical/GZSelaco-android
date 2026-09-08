# Android graphics configs

`thor.cfg` is the shipped default for the **full game** and sets only three things
(`vid_vsync`, `vid_maxfps`, `i_benchmark 0`). The full game owns its own graphics
via its in-menu presets; do not add graphics cvars. `thor-bench.cfg` is the same
plus frame-time logging. `thor-demo.cfg` is for the demo ipk3, which needs the old
explicit graphics block because its first-run dialog is not gamepad-navigable.
`archive/` holds the demo-era A/B configs the older measurements came from.

## Full game baseline (Ayn Thor, native 1080p, 30 fps lock)

GFXPresetDeckLow + VisibilityPresetSpectacleDeck, chosen in-game:

| | |
|---|---|
| baseline | **33.2-33.4 ms** (locked, 7 of 11 windows) |
| explosion peak window | **39.2 ms** (25.5 fps) |
| worst gameplay frame | **52 ms** |
| map load (excluded) | 1355 ms |

Attribution on a 47 ms explosion frame: VM 3.8 ms, think 3.1 ms (1076 thinkers),
shadowmap 1.46 + ssao 2.13 + exposure 0.30 + ExplosionDistortion 0.48 = 4.4 ms.
That accounts for ~11 ms, leaving **~36 ms in the main scene pass** - translucent
overdraw, same conclusion as on the demo.

### Not comparable to the demo numbers below

The full game is a different measurement, not a continuation:

| | demo deck-low | full game |
|---|---|---|
| `r_ThingLOD` | 1400 | 700 |
| `smokeeffects` | 1 | 0 |
| `r_smokedensity` | (cvar absent) | 1 |
| explosion density multiplier | `0.2 x v` = **0.4x** | `0.2 x (v-1) + 1` = **1.2x** |

The last row is the big one: at the same `r_particleIntensity 2`, the full game
spawns **3x** the explosion particles the demo did (EXPLOSIONS.zsc:110). That alone
plausibly covers the 34.4 -> 39.2 ms difference in peak. `ExplosionDistortion` is
also a new postprocess pass that does not exist in the demo.

### Post-load spikes are ZScript, not autosave

A 130-265 ms frame appears shortly after each map load. Earlier notes guessed
autosave and could not confirm it (2 of 4 saves correlated). With per-frame stat
sampling it is now attributed:

```
worst 182 ms:  VM time in last 10 tics: 45.97 ms, 404410 calls, peak = 40.18 ms
               Think time = 0.70 ms - 780 thinkers
```

**404k VM calls** against ~82k in a normal window, with a 40 ms single-tic VM peak
and think time essentially idle. That is a ZScript burst from level-init scripts,
not a save and not rendering. The autosave hypothesis is retired.

Still unexplained: the `gpu` stat sometimes reports **two** `ssao` entries in one
frame (e.g. `ssao=0.82` and `ssao=2.29`), implying two scene renders despite
`mirrorsurfaces 0`. Harmless, but not accounted for.

## Result: ship deck-low

Three profiles for the Ayn Thor, all at **native 1920x1080** with identical UI
scaling and benchmarking, so switching between them changes only the graphics
ladder. Swap with:

    ./android/set-config.sh deck-high

`deck-high.cfg` is the one shipped in the APK
(`app/src/main/assets/autoexec.cfg`) - the most balanced of the three in play.

## Settings

19 cvars differ; the other 38 are identical in all three. `thor-max` is
"everything on" rather than a Selaco preset, so it differs in more than the
effects ladder - it also turns on postprocess passes and raises shadow quality.

| | thor-max | deck-high | deck-low |
|---|---|---|---|
| source | everything on | `GFXPresetDeckHigh` (MENUDEF.txt:584) | `GFXPresetDeckLow` (MENUDEF.txt:618) |
| **shadows** | | | |
| `gl_shadowmap_quality` | 1024 | 512 | 512 |
| `gl_shadowmap_filter` | 3 | 3 | 0 |
| `r_shadowQuality` | 3 | 1 | 1 |
| `r_FlashlightShadows` | 1 | 0 | 0 |
| **postprocess** | | | |
| `gl_ssao` | 3 | 2 | 2 |
| `gl_lens` | 1 | 0 | 0 |
| `r_filmgrain` | 1 | 0 | 0 |
| `gl_fxaa` | 4 | 0 | 0 |
| `gl_multisample` | 2 | 0 | 0 |
| **effects** | | | |
| `r_smokequality` | 5 | 3 | **2** |
| `r_particleIntensity` | 5 | 3 | 2 |
| `r_particleLifespan` | 3 | **1** | **1** |
| `r_BloodQuality` | 4 | 2 | 2 |
| `lightingquality` | 3 | 2 | 2 |
| `r_waterquality` | 4 | 1 | 1 |
| **LOD / decals** | | | |
| `r_ThingLOD` | 3000 | 1800 | 1400 |
| `r_effectLOD` | 3500 | 1800 | 1100 |
| `cl_maxdecals` | 4000 | 1000 | 500 |
| **mirrors** | | | |
| `mirrorsurfaces` | 1 | 0 | 0 |

## What those effect values actually do

The three effect cvars are not linear dials. Resolved against the ZScript
tables (`TICRATE = 35`, doomdef.h:61):

| derived quantity | source | thor-max | deck-high | deck-low |
|---|---|---|---|---|
| particle spawn chance | `spawnChanceSettings[]` PARTICLES.zsc:3 | **100%** | 45% | 30% |
| particle lifespan | `lifespanSettings[]` PARTICLES.zsc:10 | **160 tics (4.6 s)** | **1 tic (0.03 s)** | **1 tic (0.03 s)** |
| explosion smoke density | `0.2 * value` EXPLOSIONS.zsc:70 | 1.0x | 0.6x | 0.4x |
| explosion debris density | `0.2 * value` EXPLOSIONS.zsc:61 | 1.0x | 0.6x | 0.4x |
| steam-jet set pieces | `< 3` kills them, TEXTUREMATERIALS.zsc:231 | yes | yes | **no** |

### `r_particleLifespan` is the lingering-smoke gate

```
ACTORS/Effects/PARTICLES.zsc:10
    static const int lifespanSettings[] = {1, 60, 160, 900, 3500};
    lifespan = lifespanSettings[GetCVar("r_particleLifespan") - 1];
```

Setting **1 means a lifespan of one tic** - 29 ms, a single frame. Both Deck
presets use 1, so leftover explosion smoke dies instantly on either of them.
thor-max uses 3 = 160 tics = 4.6 s. That is a 160x difference and it is the
reason the drifting post-explosion smoke appears only on thor-max; confirmed by
observation on all three.

This is a *different* mechanism from the steam-jet gate below, and it is the one
that governs explosion residue. If you want the residue back cheaply, try
`r_particleLifespan 2` (60 tics, 1.7 s) on top of deck-high - one rung up, and it
does not touch the 100% spawn rate that makes thor-max expensive.

### `r_smokequality` is a threshold too, for a different thing

```
ACTORS/Logics/TEXTUREMATERIALS.zsc:231
    if (GetCVar("r_smokequality") < 3) {
        SetStateLabel("Killed");     // PipeBurster - the steam-jet actor
```

Below 3, `PipeBurster` kills itself on spawn, so the standing steam-vent set
pieces do not exist at all. deck-low is below the line; deck-high is exactly on
it. This affects *vents*, not explosion residue.

`r_effectLOD` is a red herring for both. It is a `DistanceCheck` draw distance on
effects that have *already* spawned (`SMOKE.zsc:222,293`), so it cannot restore
effects that were never created - raising it 1100 -> 1800 on top of
`r_smokequality 2` changed nothing, verified on device.

## Result: ship deck-low

Every attempt to raise quality above deck-low cost measurable frame time in
explosions. Same route, same instrument, peak explosion window:

| config | peak window | worst frame | vs deck-low |
|---|---|---|---|
| **deck-low** | **34.4 ms** | **43 ms** | - |
| deck-low-plus, 4 increases | 37.7 ms | 51 ms | +3.3 ms |
| deck-low-plus, 6 increases | 39.6 ms | 53 ms | +5.2 ms |
| deck-high | 41.4 ms | 47 ms | +7.0 ms |
| deck-high-lod | 43.4 ms | 59 ms | +9.0 ms |
| thor-max | 70.6 ms | - | +36 ms |

deck-low is the only config that holds the 30 fps target (33.3 ms) through an
explosion, and it does so with ~1 ms to spare.

### Increases that were tried and cost frame time

| increase | cost | note |
|---|---|---|
| `lightingquality` 2 -> 3 | part of ~5 ms | every dynamic light's radius x1.25, so x1.56 area (LightsHandler.zs:340). Selaco's own menu text: "[CPU impact: Medium]" (LANGUAGE:940) |
| `r_ThingLOD` / `r_effectLOD` / `cl_maxdecals` up | ~3 ms together | an earlier single-run A/B suggested these were free; they are not |
| `gl_shadowmap_filter` 0 -> 3 | in the ~3 ms above | not isolated |
| `r_smokequality` 2 -> 3 | ~5-6 ms by elimination | also restores the steam vents |

### How reliable these numbers are

deck-low was run twice on the same route with the same build. Peak explosion
windows came out 34.3 / 34.6 / 34.4 and 34.3 / 34.4 / 35.5, with a 43 ms worst
frame both times - **reproducible to about 1 ms.** So the +3.3 ms from
deck-low-plus and the +7.0 ms from deck-high are real effects, not noise.

The trap is *which* window you compare. Within a single run, explosion windows
span 36.0-43.4 ms depending on where the 5-second boundary falls relative to the
blast, and non-peak windows are dominated by route coverage rather than settings.
Compare peak windows only, and the measurement is good to ~1 ms; compare whole-log
means or arbitrary windows and 5 ms of apparent difference can be an artefact.

An earlier version of this section claimed nothing below 3-5 ms was resolvable.
That was too pessimistic and drawn from cross-run window misalignment rather than
from real variance.

### The one untested direction

Resolution is the only lever with a mechanism that has not been tried at these
settings. Explosion cost is translucent overdraw, which scales with pixel count,
so `vid_scalefactor 0.75` (1440x810, 56% of the pixels) should create headroom
that could then be *spent* on `r_smokequality 3` to get the steam vents back at
30 fps. `deck-high-res.cfg` is that idea applied to deck-high; the deck-low
equivalent has not been built.

Note that an earlier test appearing to show resolution did not help was invalid:
that config set `vid_scale_customwidth/height` at `vid_scalemode 0`, where they
are ignored, so it ran at full 1080p. Use `vid_scalefactor`.

## Where the frame time actually goes

Measured on deck-high with per-frame stat sampling, on a 33 ms frame:

| component | cost | source |
|---|---|---|
| ZScript VM | **~1 ms** (8.6 ms / 10 tics = 286 ms wall, ~49k calls) | `VM` stat |
| actor thinking | **0.8 ms** (606 thinkers) | `think` stat |
| shadowmap + ssao + exposure + bloom | **1.5 - 4.5 ms** (scene dependent) | `gpu` stat |
| everything else - main scene pass | **the remainder** | not instrumented |

Two conclusions:

- **The interpreted VM is not the bottleneck.** An arm64 JIT would buy about 1 ms
  per frame. An earlier note in this file claimed the missing JIT was a fixed
  ~14 ms floor; that was wrong and has been removed.
- **Postprocess has nothing left to give on deck-high.** The whole instrumented
  chain is 1.5 ms in the quiet case. Turning SSAO and bloom off entirely would
  save ~1.2 ms of a 33 ms frame.

So the cost is the main scene pass - geometry, lights, and translucent overdraw -
and the only settings with real leverage are the ones that reduce what gets
submitted: particle counts, actor and effect draw distance, decal caps.

### Isolating what deck-low does better

deck-low beats deck-high by six values. Ranked by what the attribution above
says should matter, and by what they cost visually:

| delta | expected leverage | visual cost | status |
|---|---|---|---|
| `r_particleIntensity` 3 -> 2 | **high** - spawn chance 45%->30%, debris 0.6x->0.4x | low | **adopted into deck-high** |
| `r_effectLOD` 1800 -> 1100 | medium - culls distant effect actors | low, distance only | `deck-high-lod` |
| `r_ThingLOD` 1800 -> 1400 | medium - culls distant actors | low, distance only | `deck-high-lod` |
| `cl_maxdecals` 1000 -> 500 | medium - decal geometry accumulates | low | `deck-high-lod` |
| `gl_shadowmap_filter` 3 -> 0 | low - shadowmap pass is 0.05-1.2 ms | visible on shadow edges | not tested |
| `r_smokequality` 3 -> 2 | medium - smoke trails 0.6x->0.4x | **removes the steam vents** | avoid |

`deck-high-lod.cfg` is the three low-visual-cost rows applied together. If it
closes the gap, the last two rows are never needed - which matters because
`r_smokequality 2` is what takes the vents out.

### Measuring with the 30 fps lock on

`vid_maxfps 30` pads frames that finish early, so **only frames above 33.3 ms are
meaningful** while it is set. Explosion peaks are unaffected (every frame there
exceeds the cap, so the limiter never sleeps) and remain directly comparable. The
~27 ms plateau is *masked* by the lock - set `vid_maxfps 0` to measure that.

## Performance

Ayn Thor, native 1080p, `vid_vsync 1` on a 60 Hz panel. Frame-time distribution
over whole route runs, from the archived logcat captures:

| | thor-max | thor-max, particles low | deck-low | deck-high |
|---|---|---|---|---|
| log | bench2.log | bench3.log | bench4.log | - |
| duration | 245 s | 110 s | 96 s | - |
| best window | 16.8 ms | 16.8 ms | 16.8 ms | 16.8 ms (idle only) |
| p90 window | 41.4 ms | 37.9 ms | 30.7 ms | **not measured** |
| worst window | **70.6 ms** | 46.8 ms | **33.5 ms** | **not measured** |
| spikes >33 ms | 1219 | 1046 | 256 | - |

**Every config bottoms out at 16.8 ms.** That is the vsync cap, not a
measurement - so baseline is vsync-limited in all three and is not a
differentiator. Differences only appear once frame time exceeds 16.7 ms.

Three caveats that limit how far this table can be pushed:

1. **The peaks are not the same scene.** deck-low's 33.5 ms worst window renders
   no steam vents and 0.4x explosion density with 1-tic residue; thor-max's
   70.6 ms renders all of it. That is less game drawn, not the same game drawn
   faster, so 70.6 -> 33.5 is not a 37 ms saving.
2. **Median window means are not comparable across logs.** Route coverage
   differs (245 s vs 96 s), so a run that spent proportionally longer at
   baseline has a lower median for reasons unrelated to settings. Only the worst
   window and phase-matched points can be compared.
3. **All four runs predate the benchmark fix.** The logger was printing to the
   on-screen notify buffer, so it drew its own output over the scene - worst in
   the runs with the most spikes, which biases *against* thor-max. Fixed with
   `PRINT_HIGH | PRINT_NONOTIFY` (i_benchmark.cpp, printf.h:64).

### What is established regardless

Two independent costs, from A/B rather than inference:

- **explosions** - particle and smoke overdraw. Dropping only the particle group
  from thor-max cut the worst window 70.6 -> 46.8 ms with everything else held
  fixed, which is the single largest effect measured.
- **the plateau** - shadows, lights, postprocess. Unmoved by the particle group;
  first budged by the wider Deck presets.

Ruled out: thermal throttling (status 0, ~50 C, recovery was instant because the
explosion ended), actor count (611 thinkers at 19 ms vs 586 at 37 ms), and the
ZScript VM (10-15 ms per 10 tics during 66 ms frames).

Still open: nothing pressing. The ~1.3 s frame that appears once per run is the
**map load**, not a gameplay stall - across seven captured runs the count of
1000 ms+ frames equals the count of `OM_01A - Invasion` level-load banners
exactly, including the run with two loads. Spawning ~2996 actors and building the
BSP in one tic with the GPU idle is what loading a map looks like. It pollutes the
window it lands in (dragging one mean from 33.3 to 42.7 ms), so the benchmark now
excludes frames over `i_benchmark_ignore` from the percentiles and reports them
separately.
