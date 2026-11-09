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

## Not done

- The PR is not opened (the session reached its context limit). Open it with
  `gh pr create --repo daurosk/totk-recomp-multiplayer --base main --head net-client` and a plain title such as
  "Connect the online UI to the real server". The body says what changed (the points above) and how it was tested;
  the screenshots are game images, so they stay out of the repository: describe them and name the local folder, or
  attach them through GitHub's web editor.
- Seen, not changed (the UI's look stays as it is): in the player list a long area name such as "Surface (-114, 186)"
  runs into the Team column.
- `place-window.ps1 -Slot 2` works in system-DPI pixels; DISPLAY1 runs at 96 DPI, so the 960x540 window ended about
  100 px past DISPLAY1's right edge, on the main monitor. It was moved back with per-monitor-aware `SetWindowPos`.
- Driving the chat box: a window that lost the focus drops the text field's active state, so Enter has to be a real
  key press with the game in front; Esc, Y, N and P work as posted window messages.
