# Actor sync, client side

The actors add-on (`OW_FEAT_ACTORS`, [online-wire.md](online-wire.md) 0x0400 to 0x040E) on the game's side: enemies,
bosses and their parts, wildlife, map objects, townspeople and projectiles are one body each on every screen. The game
the server names owner runs the actor; every other game shows a copy driven by the owner's states; a hit on a copy goes
to the owner and counts there; life travels in the owner's states, so it is equal on every screen.

## Parts

| Part | File | State |
|---|---|---|
| Sync core: roles, epochs, offers, states, copies, hits, gone, spawn rule, wildlife leader rule | `runtime/net/net_actors.c`, `.h` | done, offline lab passes |
| Offline lab: two invented games and a stub server, every message through the real codec | `tests/net_actors_lab.c`, `tests/net_actors_check.py` | 38 checks (and 24 of the link), built optimised and with UB traps |
| Game side: `NaGame` over the recompiled game (scan, make a copy, drive, AI off, hit, removal) | `runtime/net_actors_game.c` | **scan only** (`na_game_scan`, read-only, 1.2.1); `na_game()` is NULL until copies can be made, driven and removed |
| Link to the network thread: two queues of encoded payloads, the offer, the grant, loss and re-admit | `runtime/net/net_actors_link.c`, `.h` | done, `tests/net_actors_link_lab.c` passes |
| Hook into `net_client.c`: offer `OW_FEAT_ACTORS`, hand 0x04xx frames to the link, send the core's messages, step each frame | `runtime/net/net_client.c` | done; inert while `na_game()` is NULL (nothing offered, nothing stepped) |

## The core

An actor in one game is `LOCAL` (not shared), `OFFERED` (a placed actor this game runs and offered), `OWNED` or `COPY`.
An `owner` message moves it between the last two at any time with the last full state, so a hand-over keeps position and
life. Ids are the wire's `aid`: placed actors by map instance id, run-time ones by (creator, counter); herd copies use
(leader, bit 63 | the leader's number) so they never meet a spawn counter.

- A game offers a placed actor once per load and is a candidate until it unloads it, which it says with `gone`
  `OW_GONE_UNLOADED` (from the owner that is a release too); it never offers again to ask. `release` is only for giving
  an actor up while it is still loaded, which nothing does yet.
- Owner 0 (nobody runs it) leaves the actor a copy on every screen, standing still at its last state.
- Run-time ids are (creator, serial) with serials from `na_set_serial_base`, which `net_client.c` must call with the
  session's `admit.server_time_us`. A spawn names its parent (a summons' or a shot's nearest foe); the server answers a
  spawn whose parent another game owns with `gone` `OW_GONE_REMOVED`, and the creator removes it.
- Owned actors send a state every 100 ms while moving and every 250 ms while still (the wire's floor), with a `life`
  component when the actor has life and its class row carries `life`.
- Copies are moved to the owner's last state, carried on by its velocity for at most 0.5 s.
- A state or `gone` of an older epoch is dropped.
- An owned actor that leaves the scan is released (placed) or gone as unloaded (run-time). One that dies sends
  `gone` killed; every copy then goes through the game's own death (`NaGame.remove` with `OW_GONE_KILLED`), so loot
  comes only from the owner's game.
- `na_local_hit` is what the game's damage path asks: on a copy the hit goes to the owner and the game must not count it.
- Spawn rule (old version's M5): auto-placed enemies are the leader's alone (a follower is refused the placement in the
  leader's zone through `na_allow_auto_place`; what slipped through after the first 6 s as a follower is removed). A
  summons waits 2.5 s on the leader and 5 s on a follower; a copy of its name that appeared within 25 m around the same
  time replaces it, else it is shared. A shot is shared at once unless the nearest foe within 30 m is a copy.
- Wildlife leader rule (old version's M3b): the leader's game sends its auto-placed animals within 90 m as herd rows
  each 0.25 s; followers make copies by name hash (`NaGame.name_of`).

The class and the components of an actor come from the class table both ends compile (`online/proto/classes.txt`,
`ow_class_find`); `na_class_of` reads it. Items are left to the loot work: `shared_class` in the core does not share
class `item` yet. `take` and `taken` are the server's (first take wins) and not in the core yet.

## The link

`runtime/net/net_actors_link.c` sits between `net_client.c`'s network thread and the core:

- The sign-in offers `OW_FEAT_ACTORS` only when `na_game()` returns an adapter (`nal_offer`), so a build whose game side
  cannot make copies never asks for the add-on and the server never sends it 0x04xx frames.
- `admit` passes the account, `server_time_us` (the serial base of run-time ids and the first clock offset) and the
  granted add-ons (`nal_admitted`). Without the grant the core does not step and frames of the add-on are dropped.
- `on_frame` hands every server-to-client frame of the add-on to the link undecoded, before its own decode, so
  `net_client.c`'s message union does not grow by the large actor structs; the game side decodes it. The core's
  messages go the other way encoded and are decoded again on the network thread just before `send_msg` seals them.
  Each direction is one 512 KiB queue; a full queue drops the new record and counts it (`nal_stats`).
- Every connection end (`conn_close`) is a loss: on its next tick the game side calls `na_offline` (copies removed,
  the rest given back to the game's own AI). A new admit sets the account and serial base again and the next scan
  offers every placed actor anew.
- `nal_tick` runs from `net_tick`, once per presented frame, with the player's position for the herd cell. That is the
  present thread, which is right for the read-only scan; `make_copy`, `drive` and `remove` write game state and must
  run in the game's own frame step, so the call moves there with the first adapter that writes.
- Not done: the clock offset is the one from admit (one-way delay off); `nal_clock` takes a better one once the pong
  round trips are measured (the puppets branch does that for its own use).
- The loot work handles `spawned` and `taken` of items in `net_client.c` on its own branch; when both land, those two
  types must reach both the link and the loot code.

## Answered by the server side (docs/online-wire.md 9.1)

1. Every name hash is FNV-1a 32 of the UTF-8 name (`ow_name_hash`).
2. `herd.cell` is `ow_herd_cell(x, z)`, the 250 m grid: the game side passes it to `na_set_place`.
3. The creator owns a spawn at epoch 1 and gets no `owner` for it.

## Probed on main (1.2.1, 2026-11-08)

Five read-only rounds of `build121	otk.exe` (TOTK_TEST_WINDOW slot 0, the profile's first save, the player near
(-114, 128, 186)), read from outside with ReadProcessMemory: guest addresses are host addresses, so a reader opened with
read rights alone sees the guest. The probe scripts are kept with the hand-over notes, not in the repo.

- The process manager is `[main+0x4722268]` (the same heap address, 0x1558c8300, in all five loads; `[main+0x4722260]`
  points 0x40 below its list table). Found from the player: 140 to 153 guest words hold the player actor's address; the
  ones at +0x10 of a node of a circular list lead to list heads, and the only main global that reaches them is this one.
- `[manager+0x2d0]` points at 32 circular list heads 0x18 apart (a node's next at +0, its actor at +0x10; an empty head
  points at itself). They come in four groups of eight; head 8 holds most actors (about 150, the player at its node
  player+0x4d0), heads 3, 11, 19 and 27 about 112 objects, heads 4, 12, 20, 28 areas and event actors, the rest system
  procs (`Chm_*`, `PhysicsSystemBaseProc`). The 33rd slot after the table is not a list of actors. Walking all 32 and
  keeping each actor once gives 183 to 205 named actors here.
- `manager+0x2d8` is a list of 556 to 576 nodes with its count at `manager+0x2e8`, but its nodes are other objects (one
  vtable for all) and none points at an actor: not the actor list.
- Actor: name at `[+0x18]` and at `[+0x218]` (both on every actor seen), state at +0x24 (4 on all 186 running ones),
  position at +0x2b4 (it matches the player chain of `runtime/frame_rate.c`).
- Map instance id: the u64 at `[[actor+0x370]+0x88]`. Two loads of the same save: 102 and 101 actors with one, 101 ids
  in both, each on an actor of the same name, no id twice. It is 0 or missing on run-time actors (`Chm_*`, worn
  armour, held weapons, `CombinedActorIntegrator`). Placed trees, rocks, crates (`Kibako`), barrels, `TwnObj`, `FldObj`,
  `AsbObj` and enemies have one.
- Not found in the actor's first 0x800 bytes: a field that is equal across loads and unique per actor (only the
  transform), which is why the id is behind +0x370.

Still to probe, in order: life (the scan reports `life_max` 0 until then), rotation (the scan sends identity), the
auto-placement mark (old notes: bit 24 of actor+0x5ac), sleeping objects (whether a placed object far from the player is
in any of the 32 lists), the game's removal, the damage entry, creating an actor by name, the AI switch for copies.

## Next: the game side

Every fact below comes from the old version's behaviour documents and must be re-checked on our recompiled main with a
read-only probe before code relies on it, then cited to our own probe:

- Resident actors: `runtime/frame_rate.c` `player_actor` walks scene component 13 (name at actor+0x218, position at
  +0x2b4), but it gives up past 64 entries, so it is likely a subset (the player and a few others). The old version's
  census (P13 `runtime/online_census.c`, header comment) walked the actor manager instead: its +0x2d0 holds 8 circular
  lists 0x18 apart (a head is its own address when empty), a node's next at +0, its actor at +0x10; an actor's name at
  [+0x18], its state at +0x24 (7 to 11: on its way out), its position at +0x2b4; about 1400 actors in a field scene.
  It reached the manager through the old mod's own global, which our build does not have: the first probe is the
  manager's address in main (search guest memory for a block whose +0x2d0 lists hold a node pointing at the player
  actor, then find the main global that points to it), then the actor's map instance id. The old documents name
  `game::scene::AutoPlacementMgr` as scene component 15 (`[[scene+0x58]+0x78]`).
- The auto-placement mark: the old documents say bit 24 of actor+0x5ac.
- The placement request for wildlife (one function with one caller, per the old documents): the hook for
  `na_allow_auto_place`.
- The game's own removal (`BaseProc::requestDelete` per the old documents): trees and rocks do not fall at life 0.
- The damage entry point and a way to create an actor by name at run time: not found yet.

Order: objects first (scan, map id, life, removal), then enemies (AI off, damage hook), bosses, wildlife, projectiles.
Tests after the game side: lab cases with recorded scans, then short two-game rounds (enemy camp, a boss, trees and
rocks) per the worker rules.
