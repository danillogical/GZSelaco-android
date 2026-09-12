# Android graphics configs

Autoexec profiles for the Ayn Thor. Switch with `./android/set-config.sh <name>`.

| profile | what it is for |
|---|---|
| `thor` | **the shipped default** — play on this |
| `thor-fixedview-ab` | single-variable A/B harness at a fixed viewpoint |
| `thor-clean-binds` | one-time scrub of debug keybinds from a dev device |
| `thor-turnip` | explicit Turnip driver path; usually unnecessary, since `vulkan.so` next to the game data is auto-detected |
| `thor-demo` | the demo ipk3, which needs an explicit graphics block because its first-run dialog is not gamepad-navigable |

`archive/` holds demo-era A/B configs that older measurements came from. They reference
cvars the full game has since renamed or removed — do not run them against the full game.

**Do not add graphics cvars to `thor.cfg`.** The full game owns its graphics through its
own in-menu presets, and a graphics block here has silently overridden the player's
choices three separate times. The reasoning, and the list of cvars that legitimately
belong there, is in [../../TECHNICAL.md](../../TECHNICAL.md#the-shipped-config).

Note that **none of these files ship**. The APK packages
`android/app/src/main/assets/autoexec.cfg`, which the app extracts on first run — that is
the only config a player or beta tester ever sees. Everything here is a dev profile pushed
over it with `set-config.sh`, which needs adb.

Current performance measurements live in [../../TECHNICAL.md](../../TECHNICAL.md#performance)
rather than here, so there is one place to keep up to date. The numbers that used to be in
this file predate two frames in flight and were misleading once that landed.

---

## Two findings from this directory's measurements, kept because they still hold

### Post-load spikes are ZScript, not autosave

A 130-265 ms frame appears shortly after each map load. Earlier notes guessed autosave and
could not confirm it (2 of 4 saves correlated). Per-frame stat sampling attributed it:

```
worst 182 ms:  VM time in last 10 tics: 45.97 ms, 404410 calls, peak = 40.18 ms
               Think time = 0.70 ms - 780 thinkers
```

**404k VM calls** against ~82k in a normal window, a 40 ms single-tic VM peak, and think
time essentially idle. That is a ZScript burst from level-init scripts — not a save, and
not rendering. The autosave hypothesis is retired.

### The double `ssao` entry — SOLVED

The `gpu` stat sometimes reported **two** `ssao` entries in one frame (e.g. `ssao=0.82`
and `ssao=2.29`), implying two scene renders despite `mirrorsurfaces 0`, and was recorded
here as unexplained.

It is `gl_ssao_portals` (default **1**), which applies SSAO through mirrors and portals.
So a second entry simply means a portal surface was in view, and its absence in an
otherwise identical-looking frame means one was not. Scene-dependent and harmless — not a
driver artefact, which was the other suspicion at the time.
