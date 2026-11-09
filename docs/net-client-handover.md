# Online client: hand-over

State of branch `net-client` (off main 5db318b). Remove this file when the work is merged.

## Done

- `runtime/net/net_client.c`: `net_client.h` over `online/proto`, on a network thread of its own. The game thread only
  swaps queues in `net_tick` under a mutex that is never held across a socket call. Handshake with the player's
  Ed25519 key (`<profile>/online/identity.key`, made on the first connection), server key pinned per address
  (`<profile>/online/servers.txt`), password or claim code from `net_password`, keep-alive, reconnect within the
  server's hold, position about once a second (and at once after a jump of over 30 m), area by map square, chat on
  every channel (`/me` included), teams, teleport requests and answers, every host tool, plain messages for every
  refusal and for a lost connection (`net_status_text`, and the text of `NET_EV_STATE`).
- `TOTK_NET_FAKE=1` still plays the fake: `net_stub.c` now defines `net_fake_*` (`runtime/net/net_fake.h`) and the
  client hands every call to it. With no address set `net_enabled` is 0 and nothing online is drawn.
- UI: the `NET_EV_STATE` toast shows the reason when there is one; the Online tab has a Password row (masked) and shows
  the reason under the status. `net_password` added to `cfg.c` and `totk.ini.example`.
- Build: `tools/build_game.py` compiles `online/proto/*.c` and Monocypher into the runtime and links `ws2_32`.
- Test: `tests/net_client_check.py` (with `tests/net_client_main.c`): 122 checks, 0 failures on each of three runs, about 30 s.

## Not done

- The game itself was not built or run with this branch: the in-game round (server on localhost, the game at
  `window_size = 960x540`, `fullscreen = 0`, placed with `place-window.ps1 -Slot 2`, two bots, screenshots of the
  chat, the list and a bot's teleport prompt into `C:\Users\Caleb\Desktop\totk-recomp-multiplayer-ui-shots\net\`).
  Build as in the client-ui hand-over (gen + `build_game.py` with `--exefs build/extracted/v121/exefs`); set
  `net_address = 127.0.0.1:27120` and `net_autoconnect = 1` in `[online]`.
- The PR is not opened yet.
- Linux: the client and the harness compile for POSIX but were only run on Windows; CI does not run
  `net_client_check.py` yet (`.github/workflows/server.yml` could add it).
