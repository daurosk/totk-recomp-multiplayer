# Saves and world state

The `saves` add-on (feature bit 2, messages 0x0100 to 0x0106 in [online-wire.md](online-wire.md)). No wire change:
it uses the layouts version 1 already fixes.

## What it does

- **Shared world keys** have one value for the room: quest steps, cutscenes played, Koroks, map reveals, shrines and
  temples, lightroots, felled bosses, talks, enemy records, the world clock and townspeople schedules. A game sends its
  writes of them in `world_write`; the server applies the key's rule and sends what it took to the other games in
  `world_set` (source: the writer). A write the rule refuses is answered to its writer alone with `world_set` from
  source 0 carrying the room's value, so that game goes back to it.
- **Per-player keys** are each account's own: hearts, stamina, pouch, rupees, armour, horses, chests, records, map
  pins, Autobuild drafts, where the player stands. A game sends them in `save_write`; the server stores them for that
  account and passes them on to no one.
- **On join** the server sends the account's save (`save_part`, always at least one part) and then the world
  (`world_part`) after `admit` and before `join_ready`, so the game can apply the snapshot before the title hands over.
- **Server features** (wallet, ledger, Blood Moon) read and change values through `sv_save_get` / `sv_save_put` /
  `sv_world_get` / `sv_world_put` (server/sv_save.h); a put to an account sends it `save_fix`. On the client the
  same job goes through runtime/net/net_save.h.

## Rules (server/sv_save.c `allowed`)

| Rule | A write goes ahead when |
|---|---|
| `latest` | always (the default for a shared key without a row) |
| `forward` | it does not go back: a bool does not go from true to false, an `s32`/`s64` number does not fall (signed), a `u32`/`u64` one does not fall (unsigned), an `f32` does not fall; text, vectors and bytes as `steps` |
| `steps` | the value is not one the key held before since the server started (quest steps are hashed enums, which have no order) |
| `owner` | it comes from the world owner's game: the host's while the host is connected, else the first connected game that writes an owner key, kept while that game stays connected |
| `player` | never in the world: a `world_write` of such a key is stored in the writer's own save instead |

An unchanged value is neither stored nor sent again. A write of another kind than the stored one goes ahead.

The rules come from `world-keys.tsv` in the server's data folder: one `<8 hex digit key hash>\t<rule>` per line.
Without it every shared key is `latest`. The file is made from the user's own game files (key names are hashed, so
the table cannot be written by hand) and is not part of this repository; the tool that makes it is still to be
written (below).

## Files

In the server's data folder: `world.json` and `accounts/<id>/save.json`. One entry per line
(`{"k": "<hash>", "i": <index>, "t": <kind>, "v": "<hex bytes>"}`), so they read without a JSON library. A file is
written whole to `<name>.tmp`, flushed and then put in place (`sv_replace_file`), at most every 5 s while something in
it changed and at shutdown. The copy it replaces goes to `backups/<name>.0`; the older ones move up to `.19` and the
oldest falls off.

## Checks

- `python tests/sv_save_check.py`: the rules on made-up writes from two accounts, the owner hand-over, per-account
  isolation, the snapshot's parts, the feature API's `save_fix`, and the files (rewrite, read back, 20 backups, no
  `.tmp` left). Offline, no network.
- `python tests/net_server_check.py`: the server against bots; the admit now grants `saves` (2) when it is offered.

## Not done yet

1. **Key table.** `server/data/world-keys.toml` (patterns by name, our own, following the groups above) and
   `tools/world_keys.py`, which reads the user's GameData list, recovers names by hashing candidate strings from the
   game's own files, matches them against the patterns and writes `generated/world-keys.tsv` for the server and the
   client. Unnamed keys: per player.
2. **Client.** `runtime/net/net_save.c` behind runtime/net/net_save.h; net_client.c offers `OW_FEAT_SAVES` in
   `proof`, hands the four s2c messages to it and sends its batches every 250 ms while live.
3. **GameData hooks on 1.4.3 main.** The earlier port's notes say the game's setters append to per-type queues inside
   the GameData manager and a later commit applies them. That has to be re-checked on our recompiled main with a
   read-only probe first (find the manager, its queues and the commit, log queued entries for a while), then caught
   with a host callback through a patch site (recomp/patches-v143.txt, `bl` to a free patch-page slot below
   +0xf70; runtime/mouse_direct.c and ov_zstd.c show the pattern) at the commit's entry: read the queues, send them,
   append incoming values. Every address cited to our own probe and hook.
4. **Title hand-over.** Hold the title until `net_save_snapshot_done`.
5. **Rounds.** Recorded writes from a probe run through the rules offline; then one short two-game round (shared
   quest step, each player's own chest) once `TOTK_TEST_WINDOW` is on net-client.
