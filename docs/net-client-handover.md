# Online client: hand-over

State of branch `net-client` (main 10807fa merged in). Remove this file when the work is merged.

## Done

- `runtime/net/net_client.c`: `net_client.h` over `online/proto`, on a network thread of its own (handshake with the
  player's Ed25519 key, pinned server key, password or claim code, keep-alive, reconnect within the hold, positions,
  areas, chat on every channel, teams, teleports, host tools, plain messages for refusals and a lost connection).
  `TOTK_NET_FAKE=1` still plays the fake (`net_stub.c`, `net_fake.h`).
- CI: `.github/workflows/server.yml` runs `tests/net_client_check.py` on Linux (clang) and Windows (MSYS2 UCRT64 clang
  on the runner's own `C:\msys64`), and `net_server_check.py` on Windows too. Both jobs passed on GitHub (run
  42545183164): 0 failures each.
- In-game round on 1.2.1 (server and two bots from `tools/net_bot.c` on 127.0.0.1:27120, game at 960x540 windowed on
  the second monitor), all passed: chat in (all, whisper, /me, system line, server message) and out (all, team, /me,
  /who), player list (3 players, host, team, area, ping), team invite prompt and accept, teleport prompt from a bot
  accepted (outcome 0) and refused (outcome 1), kick from the console, `stop` ("The server shut down."), and the
  server process killed (reconnecting, then "Lost the connection to the server." after the 15 s hold). Shots in
  `C:\Users\Caleb\Desktop\totk-recomp-multiplayer-ui-shots\net\` (01 to 12).
- Fixed on the way, found in the game:
  - The chat box refused `/me` and the server's `/who`, `/teams`, `/motd`, `/help` with a "Commands" toast
    (`online_ui.cpp` `chat_submit`); they now go to `net_chat_send` on the all channel. The offline test calls
    `net_chat_send` directly, which is why it passed.
  - A kick showed its reason in two toasts (`NET_EV_KICKED` and the `NET_EV_STATE` with the same text); the state
    toast is skipped when it repeats the kick (`online_ui.cpp` `on_events`).
  - Losing the server showed a bare "Connecting..."; the reconnecting state now carries "Lost the connection to the
    server. Reconnecting..." (`net_client.c` `session`), and the client check tests that text (122 checks, 0 failures).
- Test window mode (`docs/building.md`, the test-run paragraph under "Checks"): `TOTK_TEST_WINDOW=0|1|2` opens the
  window in that third of the second monitor's working area at 16:9, frame included, at that monitor's DPI; it is shown
  with `SW_SHOWNOACTIVATE`, never raised (`gfx_window_win.c` skips `raise_window`, `swkbd_win.c` skips its
  `SetForegroundWindow`), and `input_win.c` `cursor_update` never confines or hides the cursor. `TOTK_TEST_INPUT=<file>`
  feeds the online UI (chat text and Enter, Esc, the wheel key and wheel screens, confirm, list, yes and no) from lines
  a test script appends, so no test needs the window in front. `tools/run_game.py` sets the slot by run slot.
  Checked with one 40 s launch of `build121	otk.exe` in slot 1: the window was (-1080,715)-(-24,1339), wholly on
  the second monitor (-1080,91)-(0,2011), at 96 DPI; the foreground window was never the game in 40 samples and the
  cursor clip stayed the whole desktop. `TOTK_TEST_INPUT` was not driven in a game yet (no server round in that run).

## Not done

- Teleport is on hold for release 1 (another session hides the teleport slice behind a setting in
  `runtime/ui/online_wheel.cpp`; this branch does not touch that file). The PR body should not present teleports as
  part of release 1; the teleport results above are only a record of that round.

- The PR is not opened (the session reached its context limit). Open it with
  `gh pr create --repo daurosk/totk-recomp-multiplayer --base main --head net-client` and a plain title such as
  "Connect the online UI to the real server". The body says what changed (the points above) and how it was tested;
  the screenshots are game images, so they stay out of the repository: describe them and name the local folder, or
  attach them through GitHub's web editor.
- Seen, not changed (the UI's look stays as it is): in the player list a long area name such as "Surface (-114, 186)"
  runs into the Team column.
- `place-window.ps1` (outside the repository) is replaced by `TOTK_TEST_WINDOW`, and the chat box no longer needs the
  game in front: drive it with `TOTK_TEST_INPUT` lines instead of key presses.
