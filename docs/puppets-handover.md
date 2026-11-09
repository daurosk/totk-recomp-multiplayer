# Puppets: hand-over

Branch `puppets` (from `net-client` at 98004dc). Remove this file when the work is merged. No code is written yet:
the first session spent its budget on reading and the first static probe. This file holds what it learned, so the
next session starts at the plan below instead of the reading.

## The deliverable

Other players appear as puppets that walk, run, climb, glide, swim and animate, as Link or Zelda per account,
smoothed (p95 position error 0.3 m), with name tags over their heads (faded by distance, hidden behind the camera)
and a marker per player on the map screen; they appear and leave cleanly on join, leave, area change and loading
screens. Server: relay `self_state` to players in the same area. Offline client-against-server checks with moving bot
puppets, then one short two-game round (WORKER-RULES: `game.lock`, `TOTK_TEST_WINDOW=<slot>`, slots 0 and 1).

## Already settled (read once, no need to read again)

- Wire: `self_state` 0x0280 (c2s: `time_us`, `body` = pos, vel, rot quat, spin; `comps(8)`) and `player_state`
  0x0281 (s2c: the same plus `account`) behind feature bit 1 `puppets`. Components that matter: 1 anim (up to 8
  `anim_slot` = layer, anim name hash, frame, rate), 3 gear, 5 ride, 6 glide (on, lean), 7 aim. `room_rules.interest_m`
  is 250. Nothing in the wire needs changing for puppets.
- Server (`server/sv_room.c`): `SV_FEATURES_GRANTED` in `server/sv.h` is 0; `on_proof` grants
  `pr.features & SV_FEATURES_GRANTED` and sets it on the link; `on_live` is the dispatch (its default case ends the
  connection, so a new type must get a case). `player` already holds `area` (place: scene, cell, state) and the 1 Hz
  `position`. Other sessions edit `sv_room.c` too (owner-server, actors, world-save, loot): put the relay in a new
  `server/sv_puppets.c` with two or three calls from `sv_room.c` (dispatch case, player removed, area change) to keep
  merges small.
- Client: `runtime/net/net_client.c` (network thread, `net_client.h` contract, `TOTK_NET_FAKE=1` stub in
  `net_stub.c`). Player actor: `frame_step_player_actor()` (`runtime/frame_rate.c` player_actor), position at actor
  +0x2b4 (`runtime/teleport.c` lists the other copies of it). Host-to-guest calls: `rt_call(Cpu*, fn, n<=8, ...)`
  (`runtime/host.h`).
- Smoothing: P13 `runtime/online_smooth.c` (ours, allowed) is the method to re-express: interpolation behind a
  learned delay, short extrapolation, correction.
- Name tags: the renderer already finds the scene camera block each frame: `runtime/vk_dlaa.c`, end of the per-draw
  hook, `ex->ubo[0][1]` (constant bank c4) of size 0x940 on a scene draw with D32 depth gives `D.cam_addr`; rows 3 to
  6 (byte 48, 64 bytes) are the view-projection, and the block also holds view, projection and inverse view (camera
  position for the distance fade). It is only recorded on the DLAA path today: export it independently (a small
  locked copy the present thread reads) and project tags with it.
- Map markers: nothing known yet about the map screen's projection. Positions for it already come at 1 Hz in
  `positions` (`net_players` pos). Needs its own probe (map open flag, centre, zoom), or leave it for a follow-up.
- Packs at run time: `runtime/camera_speed.c` shows how `romfs.c` serves an edited copy of a stock pack
  (`cam_pack_build`: SARC member lookup, zstd with `pack.zsdic`). A puppet pack can be built the same way, in memory
  from the user's RomFS, or by a tool into `build/` as the brief asks; either keeps game data out of the repo.
- Zelda (P13 `docs/online-characters.md`): the Zelda look is a mod folder of the player's (`Zelda Edition`), never
  ours. A game without those files shows a Zelda account as Link.

## The body: first static probe

Main 1.2.1, read with capstone on the decompressed NSO (`recomp/nso.py`; the extracted ExeFS is in
`C:\Users\Caleb\Desktop\totk-recomp-multiplayer-net\build\extracted\v121\exefs\main`):

- main+0xab92cc is `ActorInstanceMgr::requestCreateActorAsync` (a name from the P13 notes, a fact about main).
  x0 = the manager (byte +0xe1 bit 1 refuses), x1 = a pointer whose first word is the actor name's `char *`, then
  x2 to x7 and one stack argument (`[x29+0x68]` in its frame), so nine arguments: one more than `rt_call` passes.
  It builds the path `Actor/<name>` (+ the ActorParam suffix) on its stack. The argument layout (the create arg) is
  not known yet.
- P13 `docs/online-native-m3.md`: a synchronous create (`requestCreateActorSync`) exists and the actor is there when
  it returns; its address in main is not written down. Find it near 0xab92cc (same manager, a sibling taking the same
  first arguments).
- P13 `docs/online-native-m3b.md`: every placed actor goes through main+0x10f6568(cell, &name, count, ...), called
  by the sequence node `SeqExecuteRequestCreateActorAutoPlacement` (main+0x10f6394). A simpler door if the create
  arg is hard: it takes a name and a count.
- Driving the body: P13 `docs/online-native-client-plan.md` names `ActorBase::updateTrans` main+0xe8daf0,
  `updateRotation` main+0xe655d8, `RigidBodyBase::requestChangeMotionType` main+0xe8be30 (all 1.2.1, re-check each).

## Done (second session, branch `claude/puppets-2-a25eef`)

- Merged `origin/net-client` at 7458cc0: `TOTK_TEST_WINDOW` is in, so game rounds are allowed now (WORKER-RULES).
- Server, step 1 of the plan, commit 4a484d3: `server/sv_puppets.c` (relay by scene, in-world state, `interest_m`,
  20 a second per sender with a burst of 4), three hooks in `sv_room.c` (dispatch case, `player_remove` and new
  slot, area change), `sv_room_peer`/`sv_room_send` in `sv.h`, `SV_FEATURES_GRANTED` = puppets. `tools/net_bot.c`
  has `walk NAME CX CY CZ [JITTER_MS] [LOSS_PCT]` and `stand`; the path is `tools/net_bot_path.h` (walk, run,
  glide with a sharp turn, climb, fall). `tests/net_server_check.py` `run_p`: 24 checks, all pass.
- Client, step 2, commit 6b5f201: `net_set_self(NetSelf*)` and `net_puppets(NetPuppet*, max)` in `net_client.h`
  (fake in `net_stub.c`: invented players walk circles). `runtime/net/net_puppets.c` sends at 20 Hz (keep-alive
  every 200 ms when still), stamps on the server clock (offset from admit, then the shortest ping), takes the roster
  for names, Zelda and in-world state, drops a puppet at once on depart or out-of-world roster, fades it out after
  1 s of silence. `runtime/net/net_smooth.c`: Hermite with sent velocities, learned delay (60 to 300 ms),
  damped extrapolation, correction offset, warp snap. The client now offers `OW_FEAT_PUPPETS` and reports
  `OW_PLACE_LOADING` while `NetSelf.in_world` is 0. `tests/net_client_check.py` `run_p`: on localhost the p95 error
  is 0.0002 m on a clean stream and 0.045 m with 20 ms jitter and 10 % loss, delay about 64 ms; the whole client
  check passes (0 failures).
- Not yet: nothing in the game calls `net_set_self` or `net_puppets`; no name tags, body, map markers or game round.
  The localhost stream has almost no network jitter, so the 0.3 m figure needs a check with a delay-spike stream
  (add one to `net_bot` walk: hold sends for 150 ms now and then).

## Plan for the next session

1. Game side of the client: each frame in the runtime (where `net_set_local` is called, `runtime/ui/online_ui.cpp`
   `report_local`), read the player actor (`frame_step_player_actor()` in `runtime/frame_rate.c`, position at actor
   +0x2b4, rotation and animation to probe), velocity by difference, and call `net_set_self`; `in_world` 0 during
   loading screens and on the title screen.

0. (done) Server: `sv_puppets.c`, grant `OW_FEAT_PUPPETS`, keep each player's last `self_state`, relay it as `player_state`
   to live players whose area scene matches and whose state is playing, within `interest_m` of the sender's last
   position, at most 20 a second per sender per viewer; drop on leave, title and loading. Tests in
   `tests/net_server_check.py` with bots (`tools/net_bot.c` gains a moving puppet mode).
0. (done) Client: offer the feature; each frame read the local player (pos, rotation, velocity by difference, animation
   later), send `self_state` at 20 Hz; keep per-account sample buffers; smoothing; `net_puppets()` in
   `net_client.h` (account, name, character, smoothed pos and yaw, anim) and a fake in `net_stub.c`. Tests in
   `tests/net_client_check.py` against the server with moving bots, measuring the p95 error of the smoothed path.
3. Name tags in `runtime/ui/online_ui.cpp` from the exported camera block.
4. Body: find the sync create and its arg, spawn the stock `Player` model under a puppet actor (pack built from the
   user's RomFS with the control and camera components left out, found by trying), drive it kinematically. Game
   rounds only once `TOTK_TEST_WINDOW` is on `origin/net-client` (it was not at 98004dc).
5. Map markers (probe), then the two-game round, the PR against main with tested and not-tested lists.
