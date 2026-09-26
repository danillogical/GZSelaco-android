# Release plan: v0.2.0

For an agent doing the push and the GitHub release. Read
[CLAUDE.md](CLAUDE.md) first — the build-chain warnings in it are not
boilerplate, and two of them will bite you here specifically.

Everything up to this point is **committed and verified on hardware**. What
remains is outward-facing: a push, a tag, and a public release. Those are
hard to undo, so the gates below are not optional.

---

## State at time of writing

| | |
|---|---|
| branch | `android-macos-ports` |
| remote | **`fork`** → `https://github.com/danillogical/GZSelaco-android.git` |
| artifact | built at **`e5d719f85`**, sha256 `41463351bd934662b554c659fea65f53bbfb048513103e70f142bd4f18df2a9e` — the exact file tested on the Thor |
| after the artifact | docs-only commits (Obtainium button, screenshot, this plan); no code |
| ahead of `fork/android-macos-ports` | `git rev-list --count fork/android-macos-ports..HEAD`, 0 behind |
| version | `versionName '0.2.0'`, `versionCode 200` |
| previous release | tag `v0.1.0`, asset `Selaco-android-0.1.0.apk` |

**The remote you want is `fork`. It is not `origin`, and `origin` is not this
project.** There are three remotes and only one is correct:

| remote | points at | |
|---|---|---|
| **`fork`** | `danillogical/GZSelaco-android` | **the target** |
| `origin` | `TheCockatrice/GZSelaco` | upstream Selaco engine — **never push here** |
| `upstream` | `ZDoom/gzdoom` | upstream GZDoom — **never push here** |

`git push origin android-macos-ports` would aim a personal fork's work at the
Selaco engine project. It would almost certainly be rejected for lack of
permission, but do not rely on that. Name `fork` explicitly, every time.

`gh` is authenticated to **two** hosts — `github.com` (as `danillogical`)
and `github.pie.apple.com`. Always pass `--repo danillogical/GZSelaco-android`
explicitly so a command cannot resolve against the wrong host.

---

## 1. Verify the tree before touching the remote

```bash
git status --short                 # must be empty
git rev-parse --abbrev-ref HEAD    # android-macos-ports
git log --oneline -1
git diff --stat e5d719f85 HEAD     # docs only: README.md, docs/images/, this file
```

Stop if the tree is dirty, or if anything outside those paths changed since
`e5d719f85`. A code change there means the tested artifact no longer matches
the source being released.

Scan what is about to become public for the personal identifiers this repo
must never contain. They are listed in the user's own CLAUDE.md rather than
here, deliberately — writing them into a tracked file is the very thing the
rule forbids, and this file is about to be published. `danillogical` and
`danillogical@gmail.com` are the correct identity and are fine.

```bash
git log v0.1.0..HEAD --format='%an <%ae>' | sort -u   # expect only danillogical
```

`__APPLE__` in `src/d_main.cpp` is a standard preprocessor macro in upstream
code, not an identifier — ignore it.

---

## 2. Verify the artifact — it is already built, and it is the one that was tested

```
android/app/build/outputs/apk/release/app-release.apk
```

**Do not rebuild it.** This exact file was installed on the Thor and played.
A rebuild at a later commit would produce the same code with a different
embedded version string, and it would be a file nobody has run. It is
gitignored (`/android/app/build/`), so it exists only on this machine.

It is the `release` buildType, not `debug`. Both are signed with the same
debug key (`signingConfig signingConfigs.debug`). The difference is that
`release` is not `debuggable`, so `adb shell run-as` does not work against it.
A `debug` APK is a different artifact and must not be what ships.

```bash
APK=android/app/build/outputs/apk/release/app-release.apk
shasum -a 256 "$APK"   # must be 41463351bd934662b554c659fea65f53bbfb048513103e70f142bd4f18df2a9e

unzip -p "$APK" lib/arm64-v8a/libSelaco.so > /tmp/so
strings /tmp/so | grep -c e5d719f853ce61ba1c1cca60e0387a9182f8f789   # must be 1
strings /tmp/so | grep -oE 'v0\.1\.0-[0-9]+-g[0-9a-f]+(-m)?' | head -1 # v0.1.0-33-ge5d719f85, no -m
unzip -l "$APK" | grep -c 'assets/profiles/'                         # must be 5
unzip -p "$APK" assets/autoexec.cfg | grep -c '^vid_fps 0'            # must be 1
unzip -l "$APK" | grep -c libVkLayer                                  # must be 0
```

**If the digest does not match or the file is missing, stop and ask.** Do not
rebuild to make the gate pass: that replaces the tested artifact with an
untested one, which is a decision for the user.

---

## 3. Push the branch

Outward-facing and the first irreversible step.

```bash
git push fork android-macos-ports
```

Do **not** force-push. Nothing here rewrites history, so a plain push is
correct; if it is rejected, stop and report rather than forcing.

---

## 4. Tag

Tag **`e5d719f85`**, the commit the artifact was built from, not HEAD. The
commits after it are README-only and still go public with the branch push.
GitHub shows the README from `android-macos-ports`, the default branch, so the
Obtainium button and screenshot appear whichever commit carries the tag.

```bash
git tag -a v0.2.0 e5d719f85 -m "v0.2.0 - the second screen"
git push fork v0.2.0
```

Commits on this repo are unsigned by design (`commit.gpgsign=false`;
`ac-sign` is for internal hosts only), so the tag will be unsigned too.
That is consistent with `v0.1.0` — do not reach for `-s`.

---

## 5. Create the release

Match `v0.1.0`'s shape: not a draft, not a prerelease, one APK asset named
`Selaco-android-<version>.apk`. The README links `Releases` and tells players
to download exactly that filename, so **the name matters**.

```bash
cp "$APK" /tmp/Selaco-android-0.2.0.apk

gh release create v0.2.0 /tmp/Selaco-android-0.2.0.apk \
  --repo danillogical/GZSelaco-android \
  --title "v0.2.0 — the second screen" \
  --notes-file /tmp/release-notes-0.2.0.md
```

### Release notes

Write them to `/tmp/release-notes-0.2.0.md`. Draft below — edit freely, but
keep the two honesty items, which are there deliberately:

```markdown
## The second screen

On the AYN Thor, the bottom screen now shows Selaco's codex while you play —
datalogs, objectives, milestones, statistics and the manual. It is the game's
own PDA rather than a reproduction: open your codex and the lower panel goes
live and gamepad-driven, close it and it becomes the standby codex, which
keeps your last tab and refreshes when you find a secret.

Size is adjustable in **Options → Handhelds**, along with turning the panel
off entirely.

## Pick your device on first launch

Selaco's two first-run dialogs are replaced by one device picker: **AYN
Thor**, **AYN Odin 2**, **AYN Odin**, or a Steam Deck preset. That choice
sets graphics, handheld UI, controls and the second screen together.

Change it later with **Options → Handhelds → Reset Device Choice**.

Device settings are plain text in [`profiles/`](profiles/) — adding or
retuning a handheld is a text file, not a code change. See
[`profiles/README.md`](profiles/README.md); pull requests welcome.

## Also

- The fps counter is off by default
- Fixed a crash on engine restart caused by stale script references
- Fixed the second screen coming back blank after the device sleeps
- Install and update through Obtainium: see the button in the README

## Known limitations

- **Only the AYN Thor has been tested.** The Odin 2, Odin and Steam Deck
  profiles are derived from Selaco's own presets but have never run on that
  hardware — including the entire single-screen code path.
- The APK is **debug-signed**. It installs fine, but it is not a
  production-signed build.
- A gamepad is required; there are no touch controls. Vulkan only.
```

---

## 6. Verify the release

```bash
gh release view v0.2.0 --repo danillogical/GZSelaco-android \
  --json tagName,name,assets,isDraft,isPrerelease
```

Check: asset named `Selaco-android-0.2.0.apk`, `isDraft: false`,
`isPrerelease: false` (the README's Obtainium button tracks stable releases
only), and a size of 12826734 bytes. A size near 19 MB means a debug APK was
uploaded by mistake.

Then download the published asset and confirm it is the tested artifact,
rather than trusting the upload:

```bash
gh release download v0.2.0 --repo danillogical/GZSelaco-android \
  -D /tmp/verify --clobber
shasum -a 256 /tmp/verify/Selaco-android-0.2.0.apk /tmp/Selaco-android-0.2.0.apk
```

Both digests must match, and must equal the one in section 2.

---

## Stop and ask rather than improvising

- The push is rejected, or the remote has commits you do not have
- `v0.2.0` already exists on the remote
- Any gate in section 2 fails. Do not rebuild to fix it
- Anything suggests pushing to upstream GZDoom rather than the fork

## Explicitly out of scope

- **macOS.** There is no pre-built macOS release and the README says so.
  Do not build or attach one.
- **Production signing.** The `release` buildType in
  `android/app/build.gradle` uses `signingConfigs.debug`, so the artifact this
  plan ships is debug-SIGNED despite being the release buildType. Changing that
  needs a real keystore and is a separate decision.
- Rewriting history, retagging `v0.1.0`, or touching any other branch.
