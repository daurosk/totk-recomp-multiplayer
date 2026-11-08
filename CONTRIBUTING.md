# Contributing

Contributions from anyone are welcome: bug reports, fixes, features, docs and testing. Talk to us on [Discord](https://discord.gg/GwtRKAqng5) or in [Discussions](https://github.com/daurosk/totk-recomp-multiplayer/discussions) before starting something large.

## How to contribute

1. Fork the repository and create a branch for your change.
2. Build and test it ([docs/building.md](docs/building.md)), with the content hook enabled (below).
3. Open a pull request against `main`. A maintainer reviews it, may ask for changes, and merges it once it is ready.

Problems that also happen in plain TotK Recomp, without multiplayer, belong in [TotK Recomp's issues](https://github.com/valentderah/totk-pc-recomp/issues); fixes for them are best sent there, and they reach this project with the next upstream merge ([UPSTREAM](UPSTREAM)).

## The one hard rule

Nothing that came out of the game goes into the repository: no game files, keys, extracted data, recompiled C, compiled binaries, shaders, shader caches, fonts, audio or movies. Not in commits, not in issues, not in pull requests. Only information about the game goes in: addresses, hashes, patch code we wrote, shader and bind-state identifiers.

Two checks enforce this:

- CI runs `tools/check_content.py` on every push and pull request.
- The same check runs before each local commit once you enable the hook:

  ```sh
  git config core.hooksPath .githooks
  ```

A failing check is never bypassed with `--no-verify`. If it flags a file by mistake, change the check in a separate pull request and say why.

Links to game files, keys or firmware in issues or pull requests get the issue closed and the content removed.

## Code from other projects

The multiplayer code (client, server, protocol and tools) is written from scratch for this project. Do not copy code, scripts, names or file formats from other multiplayer mods or servers, closed or open; describe what you need in your own words and write it. `tests/foreign_code_check.py` runs in CI and refuses known names from projects whose code may not be used.

Code may be copied only from projects under MIT, BSD, ISC, zlib or Apache-2.0. Each copy keeps its original header and gets a row in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) with the source commit.

GPL projects (yuzu, suyu, OpenSmash-Ultimate, Unicorn inside the game binary) can be read to understand behaviour, but their code is not copied. Unicorn is fine as a separate test tool.

## Reporting bugs

Use the issue form. For multiplayer problems, also say whether you were the host, how many players were connected, and the server log if you host. Include the port version and game update from the title line of the settings menu (F1), your GPU and driver (Copy system info in the menu's Advanced tab copies them), the log (`logs\totk.log` next to `totk.exe`; after a crash, copy it before you start the game again), and the steps to reproduce. Screenshots are fine; recordings of long gameplay are not needed.

## Pull requests

- One topic per pull request.
- Describe the problem, then what changed.
- Say how you tested it: which version, which scene, how long.
- A change to the lifter (`recomp/`) or the CPU headers passes the differential tests in [docs/building.md](docs/building.md), "Checks".
- Commit messages are plain sentences, without `feat:` or other prefixes.
- AI-assisted code is welcome. You are responsible for having read and run it.

## Licence

By sending a pull request you agree that your contribution is licensed under the project's licence, the [GNU GPL v3.0 or later](LICENSE). Files that came from TotK Recomp keep their MIT notice ([licenses/TotK-Recomp-MIT.txt](licenses/TotK-Recomp-MIT.txt)).

## Code and docs

- Code, comments, commit messages and documentation are in English.
- Generated code is never edited by hand: change the generator.
