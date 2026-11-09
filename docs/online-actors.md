# Actor sync, client side

The actors add-on (`OW_FEAT_ACTORS`, [online-wire.md](online-wire.md) 0x0400 to 0x040E) on the game's side: enemies,
bosses and their parts, wildlife, map objects, townspeople and projectiles are one body each on every screen. The game
the server names owner runs the actor; every other game shows a copy driven by the owner's states; a hit on a copy goes
to the owner and counts there; life travels in the owner's states, so it is equal on every screen.

## Parts

| Part | File | State |
|---|---|---|
| Sync core: roles, epochs, offers, states, copies, hits, gone, spawn rule, wildlife leader rule | `runtime/net/net_actors.c`, `.h` | done, offline lab passes |
| Offline lab: two invented games and a stub server, every message through the real codec | `tests/net_actors_lab.c`, `tests/net_actors_check.py` | 32 checks, built optimised and with UB traps |
| Game side: `NaGame` over the recompiled game (scan, make a copy, drive, AI off, hit, removal) | `runtime/net_actors_game.c` | **not started** |
| Hook into `net_client.c`: offer `OW_FEAT_ACTORS`, queue 0x04xx frames for the frame step, send the core's messages | `runtime/net/net_client.c` | **not started**: waits for the game side, so the feature is never offered without it |

## The core

An actor in one game is `LOCAL` (not shared), `OFFERED` (a placed actor this game runs and offered), `OWNED` or `COPY`.
An `owner` message moves it between the last two at any time with the last full state, so a hand-over keeps position and
life. Ids are the wire's `aid`: placed actors by map instance id, run-time ones by (creator, counter); herd copies use
(leader, bit 63 | the leader's number) so they never meet a spawn counter.

- Owned actors send a state every 100 ms while moving and every 250 ms while still (the wire's floor), with a `life`
  component when the actor has life.
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

The class of an actor comes from `na_class_of` (name prefixes) until the server's class table
(`online/proto/classes.txt`, branch owner-server) lands; then the core reads that table instead.

## Open questions (asked of owner-server, which owns the wire)

1. The hash of `herd_row.name_hash` and `anim_slot.anim`. The core uses FNV-1a 32 of the UTF-8 name.
2. The numbering of `herd.cell`. The core sends what `na_set_place` gives it.
3. Whether the creator owns a spawn at epoch 1 without an `owner` message. The core assumes yes.

## Next: the game side

Every fact below comes from the old version's behaviour documents and must be re-checked on our recompiled main with a
read-only probe before code relies on it, then cited to our own probe:

- Resident actors: `runtime/frame_rate.c` `player_actor` walks scene component 13 (name at actor+0x218, position at
  +0x2b4). Whether that list holds every actor or a subset is the first probe; the old documents name
  `game::scene::AutoPlacementMgr` as scene component 15.
- The auto-placement mark: the old documents say bit 24 of actor+0x5ac.
- The placement request for wildlife (one function with one caller, per the old documents): the hook for
  `na_allow_auto_place`.
- The game's own removal (`BaseProc::requestDelete` per the old documents): trees and rocks do not fall at life 0.
- The damage entry point and a way to create an actor by name at run time: not found yet.

Order: objects first (scan, map id, life, removal), then enemies (AI off, damage hook), bosses, wildlife, projectiles.
Tests after the game side: lab cases with recorded scans, then short two-game rounds (enemy camp, a boss, trees and
rocks) per the worker rules.
