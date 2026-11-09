# client-ui hand-over (client-ui branch)

Done: runtime/net/net_client.h + net_stub.c (fake server, TOTK_NET_FAKE=1), chat box, player list, toasts, wheel menu
(V / Minus+L3), confirmations, Online tab, settings (cfg.c, totk.ini.example, [online]), docs/online-ui.md. Builds on
Windows 1.2.1 and runs with the fake server; screenshots of chat, list, toasts, wheel rings and the kick confirmation are
in C:\Users\Caleb\Desktop\totk-recomp-multiplayer-ui-shots\auto.

Not done / not verified:
- Teleport does not move the player on 1.2.1: the position copies we write are rewritten every frame (see the Teleport
  section of docs/online-ui.md for what was measured). Needs the game's own warp function or the physics body.
- No screenshot of the Online tab (TOTK_MENU_TEST saves one per tab, the Online tab is the last, TAB_ONLINE).
- Real keyboard / pad input was not tried (hold V, Enter, Minus+L3); the demo script drives the same state directly.
- SDL (Linux) adapter edits in menu_sdl.cpp are untested; only Windows was built.
- Build trick used: gen + build_game with --exefs build/extracted/v121/exefs (copied from the PC port folder, without the
  extra subsdk4 that folder also holds) and --romfs from the P13 extraction; translator built with tools/build_xlate.py.
