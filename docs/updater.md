# Updater

The game updates itself. When a player starts `totk.exe` and a newer TotK Recomp Multiplayer release is out, a box
asks "TotK Recomp Multiplayer 0.1.1 is out. Update now?" with the first lines of the release notes and three buttons:

- **Update** downloads the release, checks it, closes the game, patches the install folder in a small progress window
  and starts the new game. Settings (`totk.ini`), saves and the online identity (`profile/`), mods (`mods/`), the shader
  caches and the game data stay. The player never runs the setup by hand.
- **Later** starts the game as it is; the box comes back at the next start.
- **Skip this version** writes `update_skip = <version>` to `totk.ini`; the box stays away until a newer release.

`[update] update_check = 0` in `totk.ini` turns the check off. Windows only for now; the Linux AppImage is updated by
downloading the new one.

## Pieces

| File | What it does |
|---|---|
| `installer/updater/update.c` | `totk-update.exe`: the check, the prompt, the download, the checks, the patch, the rollback |
| `installer/updater/json.c` | The JSON reader for the release API, the manifest and the setup's events |
| `installer/updater/key.h` | The Ed25519 public key of the release signing key |
| `runtime/update_start.c` | `totk.exe` runs `totk-update.exe --check` at start and exits when it answers "updating" |
| `installer/steps.py` `install_updater` | The setup's cleanup step copies `bin/totk-update.exe` of its release beside `totk.exe` |
| `tools/make_update.py` | Writes and signs the update manifest; makes a signing key |
| `tools/make_release.py` `build_updater` | Builds `bin/totk-update.exe` into the release folder; `--sign-key` signs the zip |
| `.github/workflows/sign-update.yml` | Signs a draft release's setup zip with the key in the repository's secrets |
| `tests/updater_test.py` | The end-to-end tests on a fake install (below) |

## The check

`totk.exe` calls `update_at_start` for a normal start only (not for `--prepare-shaders`, the probes, `--seconds` or a
restart from the settings menu), before its window opens. When the updater starts the game after an update, done,
cancelled or refused, it sets `TOTK_UPDATER_STARTED` and the game skips that one check: a refused release would
otherwise be offered again at once. `totk-update.exe --check DIR --pid PID`:

1. Restores the install folder if the last update was cut off (the journal, below).
2. Unless `update_check = 0`, asks `https://api.github.com/repos/daurosk/totk-recomp-multiplayer/releases/latest` in a
   background thread and waits at most 4 seconds. No network, a timeout, any error, a release without the update
   manifest, a version not newer than the install (the updater's own `MP_VERSION`) or the `update_skip` version: exit 0,
   the game starts, nothing is shown.
3. Shows the prompt. Update copies the updater to `update/run/` (the update replaces `totk-update.exe` itself), starts
   that copy with `--apply` and exits 10; `totk.exe` exits.

## Trust

Only the signature is trusted. The release page, its notes and the download addresses are not: they are shown or
followed, never believed.

Each release carries `TotKMP-Update-windows.json`, a manifest of the update package (the setup zip a new player
downloads, `TotKMP-Setup-<version>-windows.zip`: name, size, SHA-256) and of every file in it (path, size, SHA-256),
and `TotKMP-Update-windows.json.sig`, the Ed25519 signature of the manifest's exact bytes (Monocypher's
`crypto_ed25519_check`, `online/third_party/monocypher`). The updater refuses:

- a manifest whose signature does not check against `key.h`;
- a manifest for another product or platform, or whose version is not the release's or not newer than the install
  (no downgrade, no replay of an old signed release);
- a package whose size or SHA-256 differs from the manifest;
- an unpacked folder with a file the manifest lacks, a file that differs from it, a missing file or a link;
- manifest paths with `..`, `.`, a drive, a backslash, a Windows device name or a reserved character.

Plain `http` is accepted only for a loopback address given in `update_url` (the tests); GitHub is always `https`, and a
redirect never goes from `https` to `http`.

## The patch

`totk-update.exe --apply` shows the progress window and works in `<install>/update/`:

1. Waits for the game to exit, asks the API again, downloads and checks the manifest and its signature, checks free
   space, downloads the package to `update/download/` and checks it.
2. Unpacks it with Windows' own `tar.exe` into `update/stage/`, checks every file, and moves it to
   `update/setup-<version>/`. Its `temp/` becomes a junction to `update/temp/`, shared by every update and by the mods'
   rebuilds, so the generated C and the objects carry over and Ninja recompiles only what changed.
   Until here the install folder is untouched and Cancel stops the update.
3. Saves the install folder into `update/backup/`: top-level files and `build-cache/` are copied, the other folders are
   hard links (no space, no time). `profile/`, `mods/` and `logs/` are left out: the setup never writes them.
   `update/backup.list` names what was saved; `update/journal.txt` says `saving`, then `patching`.
4. Runs the new setup's command line on the install folder: `python\python.exe -m installer --dest <install> --root
   <setup>` (`--dlss` when `nvngx_dlss.dll` is there). The setup keeps the extracted game, rebuilds `totk.exe` when its
   inputs changed, prepares the shaders when the runtime or the translator changed and writes `totk.ini` only to add
   what is missing ([installer.md](installer.md), "Updating the port"). Its JSON events drive the progress window.
   When it answers `check.inputs_needed` (a new release that must extract again), it runs once more with the game files
   the setup remembered for this folder (`%LOCALAPPDATA%\TotK Recomp Multiplayer\setup.json`: paths only).
   It runs in a job object that ends with the updater: a killed updater never leaves a setup writing.
5. Success: the journal says `done`, the backup, the downloads and the older `setup-*` folders go, the new game starts.
   `build-cache/rebuild.txt` now names `update/setup-<version>`, so the mods' rebuilds use the new setup.
6. Failure: every saved entry is moved back, what the update added at the top of the folder is removed, the backup is
   dropped, a message names the setup's error (its own English text from the new setup's catalog) and the old game
   starts. If even that fails, the message asks to run `TotKMP-Setup.exe` on the folder and the game does not start.

A power cut or a killed updater leaves `journal.txt` at `patching`; the next `--check` restores the backup before the
game starts and says so. The running `totk.exe` and `totk-update.exe` may be the update's new ones: they cannot be
overwritten, so they are renamed into `update/trash/` first; the updater then starts the restored game (which skips
its check this once) and the game that asked exits. `update/updater.lock` (deleted when the updater ends) keeps a second updater out, and a game
started during an update says the update is running and exits.

The log is `update/update.log`.

Windows without hard links on the install drive (FAT32, exFAT): the folders are copied instead, except `romfs/`,
`exefs/` and `shared-fonts/`, which only a new extraction rewrites; those are then not saved.

## Releasing

The signing key is 64 hex digits (the Ed25519 seed) and never in the repository, a log or an issue. Its public half is
`UPDATE_PUBLIC_KEY_HEX` in `installer/updater/key.h`. The maintainer's copy lives outside the repository
(`update-signing-key.txt`, made by `tools/make_update.py --new-key DIR`, which refuses a folder inside the repository).

Either way below, publishing the GitHub release is all it takes: players see the prompt at their next start.

**Signing locally**, on the machine that builds the setup ([releasing.md](releasing.md)):

```bash
.venv-py314/Scripts/python tools/make_release.py --out build/release/TotKMP-Setup --zip build/release --sign-key <path>/update-signing-key.txt
gh release upload v0.1.1 build/release/TotKMP-Setup-0.1.1-windows.zip build/release/TotKMP-Update-windows.json build/release/TotKMP-Update-windows.json.sig --clobber
```

or, for a zip already built: `python tools/make_update.py --zip <zip> --out <folder> --key <key file>`.
`python tools/make_update.py --verify <folder> --zip <zip>` makes the updater's checks on the result.

**Signing in GitHub Actions**: add the key once as a repository secret (Settings, Secrets and variables, Actions, New
repository secret, name `TOTKMP_UPDATE_KEY`, value the 64 hex digits; or `gh secret set TOTKMP_UPDATE_KEY <
update-signing-key.txt`). Upload the setup zip to the draft release, then run the **Sign update** workflow with the tag:
it downloads the zip from the draft, writes and verifies the manifest and uploads it with its signature. The key
reaches only `make_update.py`'s environment; the tool never prints it.

A new key needs a release whose updater carries it, signed with the old key: players on older versions accept only
manifests signed with the key their updater holds.

## Tests

```bash
.venv/Scripts/python tests/updater_test.py --shots <folder>
```

Builds the updater with a throwaway key (no test manifest is ever signed by the release key), a fake `totk.exe` linked
with the real `runtime/update_start.c`, and a fake setup that answers like the setup's command line. A fake 0.0.9
install points `update_url` at a local stand-in for the release API. The scenarios: no network, check off, Later, Skip
this version, an older release, a full update (settings, saves, mods, identity and caches kept, the new game starts,
no prompt after), a tampered package, a tampered manifest entry, a bad signature, another key, a setup that fails
halfway (every file back), a killed updater (the next start restores) and a setup that needs the game files again.
`installer/updater/update.c --selftest` (run by `make_release.py`) checks Ed25519 against RFC 8032, the version order,
the path rule, the JSON reader, the notes excerpt and the `totk.ini` edits.

The windows the tests open are moved to the second monitor and placed with `place-window.ps1`.
