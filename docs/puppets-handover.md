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

## Done (third session, same branch)

- Game side, commit f861b24: `runtime/puppet_self.c` (`puppet_self_frame`, called from `online_pump` in
  `runtime/ui/online_ui.cpp`) reads the player actor each present: position +0x2b4, velocity by difference (warps over
  30 m give 0), rotation from the 3x3 matrix at actor+0x290 when it checks as a rotation, else the direction of travel.
  In game (1.2.1, fake network) the log said `puppets: local facing from the actor's rotation matrix` all run, so
  +0x290 is the rotation on 1.2.1. `in_world` is 0 with no actor, out of the map or during our teleport. Not read yet:
  animation, glide, ride (nanim 0).
- Camera export, commit 29fe902: `runtime/vk_camera.c` (`vk_camera_draw` beside `vk_dlaa_draw` in
  `runtime/vk_exec_draw.c`, `vk_camera_viewproj` for any thread, seqlock). Name tags: `online_tags_layout` in
  `runtime/ui/online_ui.cpp` (background draw list, fade 40 to 120 m, hidden at clip w <= 0.3, stale camera 250 ms,
  no y flip knob any more). A log line every 5 s, `puppets: tags: camera ... [name w .. ndc .. ..]`, gives each
  puppet's projection.
- Delay spikes, commit 0fdb7a4: `tools/net_bot.c` `walk ... [SPIKE_MS]`; `tests/net_client_check.py` checks a 150 ms
  spike every 1.5 to 3 s: p95 0.185 m, delay 99 ms. Client check 0 failures; server check 0 failures.
- Game runs (slot 0, `TOTK_NET_FAKE=1`, `tools/run_game.py build/v121 --env TOTK_TEST_WINDOW=0`, logs
  `build/v121/logs/pup*.log`): the camera block is captured (1 ms old each frame). **The tag projection is not right
  yet**: in pup4 the fake player Cleo, about 74 m north of the player (camera facing the castle, north), gave clip w
  48.6 and NDC x 1.43 (expected about 75 and 0.5), so no tag was on screen. Next: dump the 0x940 block once (rows 0 to
  15) with the player's and the puppets' positions, and try the transposed matrix and the other 4x4 runs in the block
  (view, projection, inverse view) offline against the known positions; then fix the row read in
  `vk_camera_draw`/`online_tags_layout`. `--shot` images do not include the overlay; `TOTK_NET_DEMO_DIR=<dir>` saves
  swapchain shots with the UI.
- Build in this worktree: `build/` copied from `totk-recomp-multiplayer-net`; build with
  `tools/build_game.py --gen C:/Users/Caleb/Desktop/totk-recomp-multiplayer-ui/generated/v121 --romfs
  C:/Users/Caleb/Desktop/P13/project/build/extracted/v121/romfs` (about 12 s, runtime only).

## Done (fourth session, same branch)

- Merged origin/main (owner-server PR #8, updater) at 374baf5: `SV_FEATURES_GRANTED` is now puppets | actors,
  `player_drop` and the area case call both `sv_puppets_*` and `sva_*`, `tools/build_server.py` builds
  `sv_puppets.c`, `sv_actors.c` and `sv_owner.c`. Server check, client check, actor server check, actor lab and
  proto check: 0 failures.
- Tag projection, commit 553ee18: **the camera was right all along.** `TOTK_CAMERA_DUMP=1` (`runtime/vk_camera.c`
  `camera_dump`, and a player line in `runtime/puppet_self.c`) logs every 0x940 block at c4 for 50 ms every 5 s. The
  scene camera is a pair of double-buffered blocks drawn with the 1600x900 D32 depth (format 126); the shadow and
  other cameras use 800x800 / 800x450 D16 or 64x64 targets and are already filtered out. Layout (vec4 rows): 0-2
  view (3x4), 3-6 view-projection, 7-10 projection, 11-13 inverse view (camera position in column 3, 5 m behind the
  player), 14-15 misc (far 25000, aspect 1.7778). Replaying the logged tag numbers through the dumped matrix gave the
  same NDC values: the invented players simply stood outside the view. Two fixes: the screen y is now
  `(0.5 - ny/2) * h` (clip y grows with world height; `TOTK_PUPPET_TAG_FLIPY` is gone), and the fake's first invented
  player (id 2, Alice) walks 8 m in front of the local player along its facing (`net_stub.c` `AHEAD_M`, `g_fwd` from
  `net_set_self`). Demo shot `build/v121/demo-tag/02-chat-idle.png`: the "Alice" tag is on screen, above the horizon.
- Body, static probe of `requestCreateActorSync` main+0x12a8b44 (1.2.1; address from the actors worker's notes,
  `side-session-notes/.../r1-actors-handover.md`). Seven register arguments, so `rt_call` can call it:
  x0 manager (`ActorInstanceMgr::sInstance` main+0x4722920 holds it; byte +0xe1 bit 1 refuses), x1 pointer to a
  struct whose first word is the actor name's `char *` (it builds `Actor/<name>` + suffix on its stack), x2 the
  CreateArg (nullable; with NULL it takes defaults at 0x12a9284; fields read: u8 +0x48, +0x4b, +0x4c, +0x4d, +0x50,
  u16 +0x51, u8 +0x53, pointers +0x68, +0x70, +0x78, +0x80, +0x88; the transform is presumably in +0x0..+0x47, not
  checked), x3 must be 0 (non-zero takes another path at 0x12a91b8), x4 nullable (defaults to [resource+0x18]),
  x5 stored beside the arg, x6 an int * for a result code (written 8 on failure, nullable). Returns the actor (the
  result of a virtual cast at 0x12a9164) or NULL. It loads the pack synchronously (main+0xabab98), so call it from the
  game thread only.
- Not done: the body itself, local animation, map markers, the two-game round, the PR.

## Plan for the next session

1. Game-thread calls: the actors worker's frame hook (`actors_frame` patch set, site main+0xb36948, branch
   `origin/actors`, `runtime/net_actors_game.c`, saves and restores the whole Cpu around game calls) is the place to
   call `requestCreateActorSync`, `updateTrans` main+0xe8daf0, `updateRotation` main+0xe655d8 and `requestDelete`
   main+0x83aa68 from. It is not on main yet: ask the coordinator whether to wait for the actors PR or to merge
   `origin/actors` into this branch, rather than adding a second hook at the same site.
2. Body: first spawn a stock actor by name with a NULL CreateArg (any small NPC) from that hook, log the returned
   pointer, move it with updateTrans to a puppet's position every frame. Then the puppet pack (the stock `Player`
   model without control and camera components, built from the user's RomFS into `build/` the way
   `runtime/camera_speed.c` `cam_pack_build` edits a pack).
3. Local animation for `NetSelf.anim`, glide and ride; map markers; the two-game round (slots 0 and 1, `game.lock`);
   the PR against main with tested and not-tested lists.

Probe helper (outside the repo): a 10-line capstone script over `recomp/nso.py` `Nso(...).image`, run with the P13
venv; the main NSO is `C:/Users/Caleb/Desktop/totk-recomp-multiplayer-net/build/extracted/v121/exefs/main`.
