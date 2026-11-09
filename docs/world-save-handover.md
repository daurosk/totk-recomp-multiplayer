# World save: hand-over

Branch `world-save` (local `claude/world-save-efee76` in .claude/worktrees/optimistic-heyrovsky-7a344c). No PR yet.
Design and status: [online-saves.md](online-saves.md); read its "Not done yet" first.

## Done

- Server side: server/sv_save.c, tests/sv_save_check.py (all pass).
- Key table: server/data/world-keys.toml and tools/world_keys.py. Run on the 1.2.1 RomFS on this PC
  (P13/project/build/extracted/v121, read only): 35447 keys (Bool64bitKey left out), 18102 named in about
  15 minutes, among them 940 Koroks, 415 chests, 351 shrine states, 292 lightroots, 269 quest steps. The decoy list got
  1051 chance hits: about 1000 recovered names are collisions, which can put a key in a wrong (shared) group.
- Client: runtime/net/net_save.c behind net_save.h (API unchanged for the loot/wallet work, net_client.c section
  added), wired into net_client.c. Fixed on the way: net_client.c's message union `M` lacked the saves messages, so
  decoding one overran it (now members, plus a size guard that skips any message without room).
- tests/net_save_check.py: 25 checks, 0 failures. tests/net_client_check.py and tests/net_server_check.py: 0 failures.
- origin/net-client (TOTK_TEST_WINDOW) is merged in.

## Next

0. Cut the chance hits before the table is used: join the 3.8M parts only to the heads that take them (decimal
   instance hashes only after heads such as IsTreasureBoxOpen. and IsAppearKorok.; names only after IsGet., HaveTalked.,
   ...), or keep a composed name only when the decoy rate of its head is near zero. Re-run and aim for a decoy count
   near 0. Check sample names of each group by eye.
1. A 1.4.3 build to probe: no `build/extracted/v143` or v143 build exists on this PC. Extract (tools/extract.py
   with a config.local.toml naming the 1.4.3 update; P13's config names the user's game paths), generate and build per
   docs/building.md, or ask the coordinator whether release 1 runs on 1.2.1 (every multiplayer build here is v121).
2. Read-only probe of the GameData manager's write queues and commit; then the hook (online-saves.md item 1).
3. Title waits for net_save_snapshot_done; shipping world-keys.tsv (items 2 and 3).
4. One short two-game round under the WORKER-RULES game-window rules (game.lock, TOTK_TEST_WINDOW slots 0 and 1).
5. PR against main from world-save with a tested and a "Not tested" list.
