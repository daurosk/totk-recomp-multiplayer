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
| Game side: `NaGame` over the recompiled game (scan, make a copy, drive, AI off, hit, removal) | `runtime/net_actors_game.c` | 1.2.1 only, behind `TOTK_ACTORS_SYNC=1` (patch set `actors_frame`, off by default): scan, drive (the game's `updateTrans` and `updateRotation`), hold (main body keyframed), removal (`requestDelete`), names for herd rows. Not yet: life, AI off, applying a forwarded hit, the damage entry, making a copy by name. See "Game calls" |
| Link to the network thread: two queues of encoded payloads, the offer, the grant, loss and re-admit | `runtime/net/net_actors_link.c`, `.h` | done, `tests/net_actors_link_lab.c` passes |
| Hook into `net_client.c`: offer `OW_FEAT_ACTORS`, hand 0x04xx frames to the link, send the core's messages, the player's place | `runtime/net/net_client.c` | done; inert while `na_game()` is NULL (nothing offered, nothing stepped) |
| Frame hook: the core steps on the game's own thread | `recomp/patches-v121.txt` set `actors_frame`, `runtime/net_actors_game.c` `actors_frame` | done on 1.2.1, off by default |

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
- `nal_tick` runs in the game's own frame step: the frame hook (`actors_frame`, below) calls it at most every 15 ms on
  the first thread that runs the hook. `net_tick` (the present thread) only passes the player's position with
  `nal_player`, which gives the herd cell.
- Not done: the clock offset is the one from admit (one-way delay off); `nal_clock` takes a better one once the pong
  round trips are measured (the puppets branch does that for its own use).
- The loot work handles `spawned` and `taken` of items in `net_client.c` on its own branch; when both land, those two
  types must reach both the link and the loot code.

## Game calls (1.2.1)

The game side calls the game's own functions, named here by the game's symbols. Their 1.2.1 main offsets come from the
old port's binding table of game symbols (our own P13 work); each was checked in the 1.2.1 main as a function start (a
prologue, or a first load, right after the previous function's last branch), and the manager slot of the same table,
`engine::actor::BaseProcMgr::sInstance` at main+0x4722268, is the one our probe had found on its own.

| Call | main | Used for | Checked |
|---|---|---|---|
| `engine::actor::BaseProc::requestDelete(proc, u32)` | 0x83aa68 | `remove`: an owner's gone takes the copy out | start; its body returns at once for a state of 7 to 11 at +0x24, the field our scan reads |
| `engine::actor::ActorBase::updateTrans(actor, const Vector3f *, int)` | 0xe8daf0 | `drive`: the copy goes where the owner's state is (0 as the int, as our P13 code called it) | start |
| `engine::actor::ActorBase::updateRotation(actor, const Matrix33f *, int)` | 0xe655d8 | `drive`: the copy turns; the nine floats are written in the order the scan reads them at +0x290, so a copy stands as the owner's actor whether the game means rows or columns | start |
| `engine::actor::ActorBase::getMainRigidBodyEntity(actor)` | 0x1346e94 | `puppet` | start |
| `phive::RigidBodyBase::requestChangeMotionType(body, u32)` | 0xe8be30 | `puppet`: 2 keyframed on a copy, 0 dynamic when this game takes it back | start; type 2 as our P13 code read a body's flags at +0x68 |
| `engine::actor::ActorInstanceMgr::requestCreateActorSync` | 0x12a8b44 | not used yet: `make_copy` needs its `CreateArg` | start |

The frame hook is a patch site in the root update (main+0xb36920): its `ldr x0, [x0, #0xa8]` at main+0xb36948 becomes a
`bl` to the patch page +0xf60, where `actors_frame` steps the core and then does the load. A displaced write has to be
to x0..x18: the lifted caller keeps x19..x28 in locals across a call and does not read them back. Every game call of the
step changes registers the root update still holds (x1), so the hook saves the whole register file and puts it back
before the load. Calls that pass a vector or a matrix put it on the guest stack below the hook's frame.

What this gives without the rest: a placed map object (tree, rock, crate, barrel) that every game places itself is one
body. The owner's states move it on every other screen (keyframed, so the copy's own physics do not fight the state),
and the owner's gone takes it out everywhere through the game's own removal. An enemy copy is moved the same way but
keeps its own AI until the AI switch is found, so it will twitch between its own steps and the owner's.

### Two-game rounds (1.2.1, 2026-11-08)

Two games of this branch's build and the server of this branch on 127.0.0.1, each game on its own copy of the test
profile with its own key, the first save (the player near (-114, 128, 186)), no input after the load, 150 s,
`TOTK_ACTORS_SYNC=1`, windows on the second monitor (`TOTK_TEST_WINDOW` 0 and 1).

- First round, with the step pinned to the first thread that ran the hook: both games admitted, 17 offers each, 40 and
  58 actors made copies, about 30 owned each, 1,000 and 460 states sent, no crash, no removal. The root update runs on
  several of the game's threads: the pinned thread fell to about one call a second after the load while the others ran
  10 to 50, so the core stepped about once a second. Hence the step now runs on any of them, one at a time.
- Third round (the second never reached the world: the build had no shader translator; `TOTK_XLATE` fixed that): the
  hook ran 600 times every 10 s and stepped about 590 times on each game (it moves between threads about 100 times in
  that time), 10,200 frames on each game at 60 per second, 32 and 46 copies, about 5,000 states sent by each, `gone`
  as unloaded 4 times, no crash, no shader failure. The pictures at the end show both games at the same place with
  nothing out of place. Nothing was moved or hit by a player, so whether a moved object follows on the other screen is
  not shown yet.

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

A sixth round (same save and place, 186 actors):

- Orientation: the nine floats at +0x290 form an orthonormal 3x3 on 136 of 186 actors (49 of them not the identity), with
  the position right after it at +0x2b4; the 50 others are system procs (`Chm_*`). So +0x290 to +0x2bf is a 3x4
  transform, three axes and the position. The scan reads it as a quaternion taking the three vectors as columns; whether
  the game stores columns or rows (one is the other's inverse) is not checked yet: a copy turned the wrong way would show it.
- Bit 24 of +0x5ac is clear on every enemy, animal and object listed (set only on `WorldMgrSystemBaseProc`). The old
  notes' auto-placement mark is not confirmed here; the five enemies and one insect near the player may all be placed
  by the map (four of them have a map instance id), so the mark needs a round with wildlife or random monsters about.
- No single tree or rock actor is listed (no `Obj_Tree_*`), only 30 `MergedActorD100` and a few `MergedActorD500`,
  `D1000` and `Uncond`: trees and rocks far from interaction likely live inside merged actors and get an actor of their
  own only when woken (not checked). Gameplay actors (enemies, crates, barrels, `AsbObj`) are listed out to about 145 m;
  only large town and field pieces and areas are listed beyond 200 m.

Still to probe, in order (the game calls above are done): life (the scan reports `life_max` 0 until then), what wakes a tree or rock out of its merged
actor and whether the woken actor keeps a map instance id, the auto-placement mark with wildlife about, the game's
removal, the damage entry, creating an actor by name, the AI switch for copies, and all of it on the version the
release ships if that is not 1.2.1.

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
