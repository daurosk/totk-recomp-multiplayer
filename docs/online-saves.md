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
the table cannot be written by hand) and is not part of this repository; tools/world_keys.py makes it (Key table, below).
Columns after the rule are ignored.

## Files

In the server's data folder: `world.json` and `accounts/<id>/save.json`. One entry per line
(`{"k": "<hash>", "i": <index>, "t": <kind>, "v": "<hex bytes>"}`), so they read without a JSON library. A file is
written whole to `<name>.tmp`, flushed and then put in place (`sv_replace_file`), at most every 5 s while something in
it changed and at shutdown. The copy it replaces goes to `backups/<name>.0`; the older ones move up to `.19` and the
oldest falls off.

## Key table

`server/data/world-keys.toml` holds our groups in order (name patterns and a rule each; the first match decides).
`tools/world_keys.py` reads the GameData list of the user's own RomFS (`GameData/GameDataList.Product.*.byml.zs`,
big-endian BYML, zstd with the dictionaries of `Pack/ZsDic.pack.zs`), keeps every section with 32-bit hashes
(`Bool64bitKey`, 64-bit hashes, stays in each game's save and is never sent live), and recovers names by hashing
candidates with MurmurHash3 x86 32: identifier-like strings of the game's data files and main module, and the
table's heads (`IsGet.`, `IsTreasureBoxOpen.`, ...) joined to strings of parsed BYML documents, file name stems and
the decimal 64-bit integers of BYML documents (placed objects are named by their instance hash:
`IsTreasureBoxOpen.<decimal hash>`). A decoy list (every key hash xor a constant) counts chance hits, so the run says
how far its names can be trusted. It writes `generated/world-keys.tsv`: `<hash>\t<rule>\t<kind>\t<name or ->`, which
the server (copied into its data folder) and the game (next to totk.exe, or `TOTK_WORLD_KEYS`) read.

    .venv\Scripts\python tools\world_keys.py --romfs build\extracted\<ver>\romfs --exefs build\extracted\<ver>\exefs

Unnamed keys are the player's own. Without the file the server treats every shared key as `latest` and the game
shares nothing.

## Client (runtime/net/net_save.c)

- A table of settled values per (key, index): what the game wrote and sent, or what the server sent. A game write
  equal to it is not sent, so a value the game took from the server does not go back as a new write.
- Pending writes, one per (key, index), go out every 250 ms while live (`save_write` for own keys, `world_write` for
  shared ones, 200 entries each). A key that went out in 8 batches in a row (a counter rewritten every frame) goes
  out at most every 2 s from then on, until a batch passes without it.
- s2c values (`save_part`, `world_part`, `world_set`, `save_fix`) become settled and queue for the game's next
  GameData commit (`net_save_take_incoming`), unless the game wrote that key since and the write has not gone out yet:
  the newer write wins and reaches the server in the next batch. Own values (`save_part`, `save_fix`) go to the
  listeners of `net_save_listen` too.
- `net_save_snapshot_done` is 0 from the start of a connection until `join_ready` (the server sends the whole
  snapshot before it), or until the attempt ends or the server does not grant `saves`.
- net_client.c offers `OW_FEAT_SAVES` in `proof` and calls net_save_connecting / net_save_admitted / net_save_on_msg /
  net_save_joined / net_save_pump / net_save_offline. Writes not sent when a session ends are dropped: the next
  join's snapshot is the server's word.
- Every s2c message the client handles needs a member in net_client.c's message union `M`: `on_frame` decodes into
  it, and a type without a member (53 KB for these four) wrote past it before; such a message is now skipped with a
  log line.

## Checks

- `python tests/sv_save_check.py`: the rules on made-up writes from two accounts, the owner hand-over, per-account
  isolation, the snapshot's parts, the feature API's `save_fix`, and the files (rewrite, read back, 20 backups, no
  `.tmp` left). Offline, no network.
- `python tests/net_save_check.py`: net_save.c in the client harness (tests/net_client_main.c, commands gdwrite,
  gdput, gdtake, gdget) against the real server, two clients: shared and own writes, a refused write set back for its
  writer, no echo, a quest step refused, puts, the join snapshot after a reconnect, the server's files.
- `python tests/net_server_check.py`: the server against bots; the admit now grants `saves` (2) when it is offered.

## Not done yet

1. **GameData hooks.** The earlier port's notes say the game's setters append to per-type queues inside the GameData
   manager and a later commit applies them. That has to be re-checked on our recompiled main with a read-only probe
   first (find the manager, its queues and the commit, log queued entries for a while), then caught with a host
   callback through a patch site (recomp/patches-<ver>.txt, `bl` to a free patch-page slot below +0xf70;
   runtime/mouse_direct.c and ov_zstd.c show the pattern) at the commit's entry: read the queues, call
   net_save_game_wrote for each, then apply what net_save_take_incoming gives. Every address cited to our own probe
   and hook. No 1.4.3 extraction or build is on this PC yet (only v121 builds of the multiplayer tree; the brief
   asks for 1.4.3).
2. **Title hand-over.** Hold the title until `net_save_snapshot_done` (with a time-out, so a slow server never locks
   the player out).
3. **Shipping the table.** build_game.py / the installer run tools/world_keys.py on the user's RomFS and put
   world-keys.tsv next to totk.exe; the server host copies it into the server's data folder (the server package
   cannot ship it: it is made from the game's files).
4. **Rounds.** Recorded writes from a probe run through the rules offline; then one short two-game round (shared
   quest step, each player's own chest) with TOTK_TEST_WINDOW (now on net-client, merged here).
