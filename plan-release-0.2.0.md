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
| HEAD | confirm with `git log --oneline -1`; the APK must name it (section 2) |
| ahead of `fork/android-macos-ports` | 21 commits at time of writing, 0 behind |
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
```

Stop if the tree is dirty. A release built from an uncommitted tree is the
thing the `gitinfo.h` fix below exists to make visible, and shipping one
defeats it.

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

## 2. Verify the artifact — it is already built

**The APK is built and fully gated. Do not rebuild it unless a check below
fails.**

```
android/app/build/outputs/apk/debug/app-debug.apk
```

No size or digest is recorded here on purpose: this file is itself committed,
so any edit to it changes `HEAD`, which changes the hash embedded in the
binary, which changes the digest — a number written here could never be
right about the build it describes. The check that *is* stable is that the
binary names the commit you are about to tag:

```bash
APK=android/app/build/outputs/apk/debug/app-debug.apk
DISK=$(stat -f %z android/app/src/main/jniLibs/arm64-v8a/libSelaco.so)
APKSZ=$(unzip -l "$APK" | awk '/lib\/arm64-v8a\/libSelaco.so/{print $1}')
[ "$DISK" = "$APKSZ" ] && echo "SO MATCH OK" || echo "STALE APK - do not release"

unzip -p "$APK" lib/arm64-v8a/libSelaco.so > /tmp/so
strings /tmp/so | grep -c "$(git rev-parse HEAD)"          # must be 1
strings /tmp/so | grep -oE 'v0\.1\.0-[0-9]+-g[0-9a-f]+(-m)?' | head -1
unzip -l "$APK" | grep -c 'assets/profiles/'               # must be 5
unzip -p "$APK" assets/autoexec.cfg | grep -c '^vid_fps 0'  # must be 1
```

- **hash count must be 1.** If it is 0 the APK predates `HEAD` — most likely
  because a commit landed after it was built. Rebuild. If it stays 0 after a
  rebuild, `gitinfo.cpp.o` is stale: `touch src/common/utility/gitinfo.cpp`
  and build again. That was fixed in `cf0a7fc04`, so a recurrence means the
  fix regressed.
- **the git description must NOT end in `-m`.** `-m` means `git describe` saw
  a dirty tree, so the binary was not built from a committed state.

**If the file is missing**, you are on a different machine —
`android/app/build/` is gitignored, so the APK is on disk only and is not part
of the clone. Rebuild:

```bash
./android/build-android.sh
./android/package-apk.sh            # exits 1 ON SUCCESS - see CLAUDE.md
cd android && java -Xmx4g \
  -classpath /tmp/gradle-8.13/lib/gradle-launcher-8.13.jar \
  org.gradle.launcher.GradleMain --no-daemon assembleDebug
```

Then re-run the gate above. Every exit code in that chain lies:
`package-apk.sh` exits **1** on success, Gradle reports `BUILD SUCCESSFUL`
against a tree it did not rebuild, and `adb install` reports `Success` for a
stale APK — which is why the gate compares sizes and the embedded hash rather
than trusting any of them.

**If Gradle is missing**, `/tmp` has been cleaned — it takes the jars and
leaves the directory tree, so the install looks present but `lib/` is empty.
Re-fetch (`gradle.org` is allowlisted for this project):

```bash
cd /tmp && rm -rf gradle-8.13 && \
  curl -fsSL -o g.zip https://services.gradle.org/distributions/gradle-8.13-bin.zip && \
  unzip -q g.zip && rm g.zip
```

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

`v0.1.0` tagged the commit that bumped the version. The 0.2.0 bump
(`18f00d3b2`) is ~19 commits back now, so tag **HEAD** — it is the commit the
artifact was built from, which matters more than matching where the bump
landed.

```bash
git tag -a v0.2.0 -m "v0.2.0 - the second screen"
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
`isPrerelease: false`, and a plausible size (~19.8 MB; v0.1.0 was 12.8 MB,
so a large jump is expected, not a red flag).

Then download the published asset and confirm it is the artifact you built,
rather than trusting the upload:

```bash
gh release download v0.2.0 --repo danillogical/GZSelaco-android \
  -D /tmp/verify --clobber
shasum -a 256 /tmp/verify/Selaco-android-0.2.0.apk /tmp/Selaco-android-0.2.0.apk
```

Both digests must match.

---

## Stop and ask rather than improvising

- The push is rejected, or the remote has commits you do not have
- `v0.2.0` already exists on the remote
- Any gate in section 2 fails and the cause is not obvious
- Anything suggests pushing to upstream GZDoom rather than the fork

## Explicitly out of scope

- **macOS.** There is no pre-built macOS release and the README says so.
  Do not build or attach one.
- **Production signing.** The `release` buildType in
  `android/app/build.gradle` uses `signingConfigs.debug`, so
  `assembleRelease` would not produce a differently-signed artifact either.
  Changing that needs a real keystore and is a separate decision.
- Rewriting history, retagging `v0.1.0`, or touching any other branch.
