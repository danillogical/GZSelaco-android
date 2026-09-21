# Device profiles

A **profile** configures the game for one specific handheld: graphics preset, visibility
preset, Selaco's handheld UI block, the second screen, and the frame cap. One file per
device, all in this directory.

The point of this format is that **adding or retuning a device is a data change, not a code
change.** If you own a handheld this fork does not cover, you can add support for it by
dropping one file in here — you do not need to touch any C++.

> **Read this before editing a shipped profile:** a profile is not your personal config
> file. See [Profiles track the build](#profiles-track-the-build) — your local edits on a
> device are **overwritten on every reinstall**, by design.

---

## Trying a profile

Profiles are applied from the console, by the name of the file without `.cfg`:

```
aux_listprofiles              // what the game can see
aux_applyprofile ayn_odin     // apply profiles/ayn_odin.cfg
```

Nothing applies a profile automatically. `aux_applyprofile` is currently the only thing
that reads these files at all — the device picker that will call it is a later change.

`aux_applyprofile` prints every key it resolved, whether that key was **set** by the file or
**defaulted**, which preset block it found, and how many cvars it wrote. That output is the
thing to check when a profile does not do what you expected.

---

## The keys

**Every key is optional. A key you leave out is not an error — it takes the default below.**
Keys are matched case-insensitively (`max_fps` and `MAX_FPS` are the same key).

| key | if you leave it out | accepted values | what it does |
|---|---|---|---|
| `name` | the filename, without `.cfg` | any text, spaces allowed, no quotes | Display name for the device picker. |
| `gfx_preset` | graphics left alone | `Low`, `Medium`, `High`, `Ultra`, `DeckLow`, `DeckHigh` | Applies one of Selaco's graphics presets. |
| `visibility` | visibility left alone | `Clarity`, `Spectacle`, `SpectacleDeck` | Applies one of Selaco's visibility presets. |
| `steamdeck` | **`1` — it runs** | `0`/`1`, `true`/`false`, `yes`/`no`, `on`/`off` | Runs Selaco's own `SetSteamdeckPresets()` — the handheld UI/HUD block (bigger menus and subtitles, aim assist on). |
| `second_screen` | `aux_panel` left alone | as `steamdeck` | Sets `aux_panel`, the AYN Thor's second screen. |
| `screen_size` | `aux_codex_size` left alone | a number, useful range `0.5`–`2.0` | Sets `aux_codex_size`, how large the second screen's contents are drawn. |
| `max_fps` | `vid_maxfps` left alone | a number; `0` means no cap, otherwise 20–1000 | Sets `vid_maxfps`. |

### Leaving a key out changes nothing

For every key except `steamdeck`, an absent key means **the game's own setting is left
exactly as it was.** A profile never substitutes a number invented by this system, so there
is no second set of defaults to keep in sync with the engine, and **adding a key to this
table later cannot retroactively change what an existing profile does.**

`steamdeck` is the one deliberate exception, and it defaults to `1`: every handheld wants
it, and skipping `SetSteamdeckPresets()` has caused three separate bugs in this project
(walking instead of running, 20x view bob, and a stale gamepad layout).

All the same, **the shipped profiles state every value explicitly**, including ones that
match what the game would have done anyway. A device profile should assert what it wants
rather than depend on what happens to be in the cvar when it runs — applying two profiles in
one session otherwise leaves the second one inheriting bits of the first. New profiles
should do the same.

### Writing a file

`key value`, one per line. Blank lines are ignored. `//` and `#` both start a comment, and
both work at the end of a line as well as on their own. Values are not quoted, so a `name`
may contain spaces.

```
name        My Handheld
gfx_preset  DeckLow
max_fps     60        // 60 Hz panel
```

Anything that goes wrong prints one yellow line and is skipped — an unknown key, a value
that is not a number where a number is needed, a preset name that does not exist. The rest
of the profile still applies. A broken profile cannot stop the game from starting.

---

## Where the preset names come from

`gfx_preset` and `visibility` are **not** lists of cvars maintained in this repo. The value
you write is completed into the name of one of Selaco's own `OptionValue` blocks —
`gfx_preset DeckLow` means `OptionValue "GFXPresetDeckLow"`, `visibility Spectacle` means
`OptionValue "VisibilityPresetSpectacle"` — and the engine applies whatever that block
currently contains, exactly as Selaco's own menu would.

This matters for two reasons, and it is why the feature is built this way:

- **A preset cannot be partially copied.** Reconstructing a preset by hand, or from a saved
  ini, silently drops every cvar that happens to be sitting at its engine default, because
  an ini only records what differs from the default. That has already gone wrong in this
  project: a hand-built `DeckLow` equivalent lost seven cvars and visibly was not DeckLow.
- **It survives Selaco updates.** If the game retunes `DeckHigh` or renames a cvar inside
  it, profiles follow automatically.

The authoritative list of block names is the `OptionValue "GFXPreset..."` and
`OptionValue "VisibilityPreset..."` blocks in `MENUDEF.zsc` inside `Selaco.ipk3`. The values
in the table above are what ships today. If you name a block that does not exist, the
profile says so and leaves graphics alone rather than guessing.

`steamdeck 1` likewise **calls** Selaco's `UIHelper.SetSteamdeckPresets()` rather than
reimplementing its nine cvar writes, so it cannot drift from the shipped game either.

### `Low` is lighter than `DeckLow`

Counter-intuitive and worth knowing before you pick one: `DeckLow` is **heavier** than
`Low`. The two blocks carry the same 32 cvars, differ in 13, and `DeckLow` takes the more
expensive value in all 13 (higher shadowmap resolution, SSAO on, bloom on, more decals, and
so on). `DeckLow` is tuned for a Steam Deck, which is a strong device by handheld standards.

So for a genuinely weak device, `Low` is the lighter choice — which is why
`ayn_odin.cfg` uses `Low` while `steam_deck_performance.cfg` uses `DeckLow`.

---

## The profiles that ship

| file | graphics | notes |
|---|---|---|
| `ayn_thor.cfg` | `DeckHigh` | Reproduces what the game already does on the Thor. The only profile with `second_screen 1`. |
| `ayn_odin_2.cfg` | `DeckHigh` | The Thor's settings without the second screen. |
| `ayn_odin.cfg` | `Low` | First-generation Odin, the weakest device here. Uses `Low`, not `DeckLow`, on purpose. |
| `steam_deck_performance.cfg` | `DeckLow` | Selaco's own "Performance setting for Steam Deck". |
| `steam_deck_quality.cfg` | `DeckHigh` | Selaco's own "Quality setting for Steam Deck", and the one its first-run dialog defaults to. |

All five set `max_fps`, but not to the same value: **30 for the three AYN devices**, whose
panels are 60 Hz with no realistic prospect of 60 fps, and **60 for the two Steam Deck
profiles**, matching the LCD Deck's 60 Hz panel.

Nothing in Selaco's first-run flow sets a frame cap at all, so neither figure came from the
dialog — both are judgements about the hardware. **If you have a 90 Hz OLED Steam Deck,
`max_fps 90` is the line to change**, and `max_fps 0` removes the cap entirely.

### How the AYN Thor's values were derived

Not measured and not guessed — read out of Selaco's own first-run flow, because the goal was
to reproduce what the device already does:

1. This fork forces `g_steamdeck` on for Android (`src/d_main.cpp:3528`), because Selaco's
   own Steam Deck detection needs a 1280x800 screen and these are 1080p devices.
2. `UIHelper.TestForSteamDeck()` returns true on its first line as a result
   (`helper.zs:676`), so the Thor takes every Steam-Deck branch in the first-run dialogs.
3. That makes `intro_handler.zs:218` call `SetSteamdeckPresets()` — hence `steamdeck 1`.
4. The quality dialog selects index 6 of its option list (`quality_menu.zs:151-153`). Android
   is a Unix build, so that list is `GFXPresetsQualityMenuLinux`, whose index 6 is
   `GFXPresetDeckHigh` — hence `gfx_preset DeckHigh`. (Index 2, `High`, is what a desktop
   would get.)
5. The visibility dialog uses the same `clamp(6, ...)` (`visibility_menu.zs:134-136`) against
   a three-entry list, so it lands on the last one, `SpectacleDeck` — hence
   `visibility SpectacleDeck`.

---

## Adding a device

1. Copy the closest existing profile to `profiles/<your_device>.cfg`. Use a lowercase name
   with underscores; that name is what `aux_applyprofile` takes, so keep it short.
2. Set every key you have an opinion about, and prefer stating a value to relying on the
   cvar's current contents — see [Leaving a key out changes nothing](#leaving-a-key-out-changes-nothing).
   A key you genuinely have no opinion about is safe to omit: it leaves that setting alone.
3. Comment *why*, not what — especially any value that looks wrong at a glance, so the next
   person does not "fix" it. `ayn_odin.cfg` is the worked example.
4. Rebuild and reinstall so the file is packaged, then check it on the device:
   ```
   aux_listprofiles
   aux_applyprofile <your_device>
   ```
   Read the output. Every key should be reported either as `set` (your file) or `default`,
   the preset block should be found, and the cvar count should be non-zero. A yellow line
   naming your file is a mistake in it.

The device picker in a later change lists whatever is in this directory, so a new file
appears in it with no further wiring. Until then `aux_applyprofile` is the only consumer.

---

## Profiles track the build

Profiles are packaged into the APK and extracted **on every install, overwriting whatever is
on the device** — the same way the engine's `.pk3` files are handled, and deliberately
*unlike* `autoexec.cfg`, which is written once and then left alone so a player's own edits
survive.

This is intentional: **this repo is the source of truth for profiles.** A PR that improves a
device profile has to reach existing installs, and it cannot do that if a stale copy on the
device wins.

The practical consequence: **do not use a profile file on a device as a place to keep your
own settings.** Editing `profiles/*.cfg` on the device works for a single session's testing,
but the next reinstall replaces it. For settings you want to keep, use the in-game options
menu, or `autoexec.cfg`. To change a profile permanently, change it here and open a PR.
