# Releasing

A release of TotK Recomp Multiplayer has three downloads. All are built from one tag; none is published by a script.

| Download | Built by | Where |
|---|---|---|
| `TotKMP-Server-<version>-windows.zip`, `TotKMP-Server-<version>-linux.tar.gz` | `tools/make_server_release.py` | CI ([`.github/workflows/release.yml`](../.github/workflows/release.yml)) or locally |
| `TotKMP-Setup-<version>-windows.zip` | `tools/make_release.py` | locally only |
| `TotKMP-Setup-<version>-x86_64.AppImage` | `tools/make_release_linux.py` | locally only |

`<version>` is the file `MP_VERSION` (0.1.0). `VERSION` stays the TotK Recomp release this tree is built on (0.3.13);
a release follows an upstream release by changing `VERSION` (and `UPSTREAM`) when that release is merged. Where the
versions show:

- the setup window's title and header: `TotK Recomp Multiplayer Setup 0.1.0 (TotK Recomp 0.3.13)`;
- the game's settings menu (F1) version line, its log and the Discord status: `TotK Recomp Multiplayer 0.1.0 (TotK
  Recomp 0.3.13), game 1.2.1`. `tools/build_game.py` writes it into the build as `rt_port_version` from `MP_VERSION`
  and `VERSION`; the menu's text is `runtime/ui_text.c` `UI_MENU_VERSION` ("TotK Recomp %s, game %s");
- the archive names above.

## Steps

1. Bump `MP_VERSION`, write `release-notes/<version>.md`, merge to `main`.
2. Run the **Release** workflow (Actions, *Run workflow*, branch `main`; a pushed tag `v<version>` also starts it).
   It builds the server on a Linux and a Windows runner and drafts the GitHub release `v<version>` with both archives,
   `SHA256SUMS-server.txt` and the release notes. It only creates or updates a *draft*; it refuses to touch a
   published release.
3. Build the player setups on a machine that has the game (below) and add them to the draft:

   ```bash
   gh release upload v0.1.0 build/release/TotKMP-Setup-0.1.0-windows.zip --clobber
   ```

4. Read the draft: notes, files, sizes. Publish it by hand on GitHub.

## Why the setup is not built in CI

The setup carries a prebuilt runtime library, and that library includes the generated `layout.h` of a lifted game
version (`tools/make_release.py` takes `generated/v121`, else `generated/v143`). Lifting needs the game's own files, and
game files never go to CI or into a package. The server needs none of that, so CI builds it.

## Building the setups locally

Needs the toolchain and the lifted game of [building.md](building.md) and [installer.md](installer.md).

Windows (`.venv-py314`, Python 3.14, `uv` on PATH, network on the first run):

```bash
.venv-py314/Scripts/python tools/make_release.py --out build/release/TotKMP-Setup --zip build/release
```

writes the folder `build/release/TotKMP-Setup/` and `build/release/TotKMP-Setup-<version>-windows.zip`. The folder holds
`TotKMP-Setup.exe`, `VERSION` (`0.3.13+<commit>`) and `MP_VERSION`. The runtime library in it is built from `runtime/`,
so the multiplayer client (`runtime/net/`, `runtime/ui/online_*`) is in every game the setup builds.

Linux (needs the same lifted version, `make` and `pkg-config`):

```bash
.venv/bin/python tools/make_release_linux.py --out build/release/TotKMP-Setup-linux --appimage build/release
```

writes `build/release/TotKMP-Setup-<version>-x86_64.AppImage`.

Server archives by hand, on the system each is for:

```bash
python tools/make_server_release.py --out build/server-release
```

Before uploading anything, run `python tools/check_content.py` (the builds run it over their folders too). A package
never holds game files, keys or generated game code.

## The setup leaves TotK Recomp alone

The setup's install folder starts at `TotK Recomp Multiplayer` in the user's home, its remembered paths live in
`%LOCALAPPDATA%\TotK Recomp Multiplayer\setup.json` (`$XDG_STATE_HOME/totk-recomp-multiplayer/setup.json`), and
`totk-install.json` carries `"product": "totk-recomp-multiplayer"`. A folder whose `totk-install.json` names no such
product was made by TotK Recomp itself: the setup stops at its check step (`check.dest_other_install`) before it writes
anything.
