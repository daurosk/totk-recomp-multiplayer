# TotK Recomp Multiplayer: the wire, version 1

The protocol between the game's client and the multiplayer server. One C codec, `online/proto/`, is compiled into
both ends and into the offline tests, so neither side can disagree about a byte. Its tables in
`online/proto/ow_messages.h` are the source of truth for every message; sections 12 to 14 of this document must list
exactly what they define, and `tests/net_proto_check.py` fails when they differ.

| File | What |
|---|---|
| `online/proto/ow_messages.h` | Every message, struct and component: number, direction, stage, limit, feature, fields |
| `online/proto/ow_wire.h` | Constants, value sets, errors, the C structs and the API |
| `online/proto/ow_codec.c` | Encoding and canonical decoding, the tables |
| `online/proto/ow_frame.c` | Frames, the stream reader, sealing, features |
| `online/proto/ow_handshake.c` | Identity keys, the handshake, key derivation, the clock sample |
| `online/proto/ow_os.c` | Random bytes from the operating system |
| `online/third_party/monocypher/` | Monocypher 4.0.2, unmodified (X25519, Ed25519, XChaCha20-Poly1305, BLAKE2b) |

## 1. Release 1

The wire is designed for the whole of multiplayer, but the first release carries only the core: chat, the online
interface and the radial menu (teleport to a player, open chat, host tools, team tools). Its messages are every
type marked `core` in section 12:

| Part | Messages |
|---|---|
| Handshake and session: join, leave, keep-alive, reconnect | `greet` `challenge` `proof` `admit` `refuse` `bye` `join_ready` `ping` `pong` |
| Player list: names, character, rank, team, area | `roster` `arrive` `depart` `area` `vitals` `character` |
| Positions for teleports and map markers, about once a second | `position` `positions` `mark` `marker` |
| Chat: all, team, whisper, `/me`, system | `say` `chat` `notice` |
| Teams: create, invite, join, leave, kick, list | `team_create` `team_invite` `team_invited` `team_join` `team_leave` `team_kick` `teams` |
| Teleport: request with accept or refuse, host summon | `tp_ask` `tp_asked` `tp_reply` `tp_result` `go_to` `summon` |
| Host tools: kick, ban, mute, ranks, server message | `mod_action` `mod_done` `announce` `rules` |

Everything else belongs to an add-on (section 9): puppets, saves and world state, actors, duels, the economy,
companions, the sky clock and emotes. Their layouts are part of version 1 so the numbers and the codec are settled,
but they travel only on a link that granted their feature, and release 1 grants none.

## 2. Transport

One TCP connection per game, to the host's address, port 27120 unless the host sets another, with `TCP_NODELAY`.
Everything after the first two frames is encrypted (section 6). There is no UDP and no second connection.

## 3. Frames

```
u32 length | u16 type | length bytes
```

Little-endian. Before sealing the `length` bytes are the payload; after it they are the encrypted payload followed by
its 16-byte tag, so `length` is the payload's size plus 16.

A reader checks the header the moment its 6 bytes are in, before it buffers any of the rest:

- the type is in the table (section 12); otherwise the connection ends (`OW_E_TYPE`);
- the type travels this way (`c2s`, `s2c` or `both`; `OW_E_DIRECTION`);
- the type belongs to this stage: `plain` types only before sealing, `sealed` types only after it, `any` in both
  (`OW_E_STAGE`);
- the type is `core` or its feature was granted on this link (`OW_E_FEATURE`);
- `length` is at most the type's limit (plus 16 once sealed) and at least its smallest encoding (plus 16)
  (`OW_E_TOO_BIG`, `OW_E_TOO_SMALL`).

Every payload limit is at most `OW_PAYLOAD_MAX` (65000 bytes), so a reader needs one buffer of 65022 bytes. A type's
limit is the largest encoding its fields allow, unless the table gives a smaller cap (the actor batches, 60000); the
encoder refuses a message over the limit (`OW_E_TOO_BIG`) and the sender splits it. The writer makes the same
checks as the reader, so a side cannot send what the other side refuses.

Both ends are ours, so a frame that breaks a rule is a bug and not a peer to tolerate: any error ends the connection.
The server first sends `refuse` with reason `OW_WHY_PROTOCOL` and the error's text as detail, then closes. A client
closes without answering. The reader's errors stick: once one is reported, nothing more is read.

## 4. Fields

A payload is its fields in table order, with nothing between them and nothing after the last. All numbers are
little-endian.

| Type | Bytes | Rule |
|---|---|---|
| `u8` `u16` `u32` `u64` `i16` `i32` | 1, 2, 4, 8, 2, 4 | `u8<=N`, `u16<=N`: the value is at most N (a value set, section 11) |
| `bool` | 1 | 0 or 1 |
| `f32` `vec3` `quat` | 4, 12, 16 | IEEE-754 single floats; NaN and infinity are refused |
| `b32` `b64` | 32, 64 | raw bytes: keys, nonces, signatures |
| `str(N)` | 2 + n | `u16` byte count n <= N, then UTF-8: no overlong forms, no surrogates, nothing over U+10FFFF, no C0 control character and no DEL |
| `blob(N)` | 2 + n | `u16` byte count n <= N, then the bytes |
| a struct name | its fields | the struct's fields inline (section 13) |
| `list(N, s)` | 2 + items | `u16` count <= N, then that many structs `s` |
| `comps(N)` | 1 + items | `u8` count <= N, then that many components (section 5) |

Decoding is strict and canonical: every byte must belong to a field, every value must be one the encoder would
write, and a payload that decodes encodes back to exactly the same bytes. Cutting a payload anywhere, or adding a
byte, makes it fail.

## 5. Components

Add-on state messages carry their optional parts as components, each self-delimiting:

```
u8 kind | u16 length | length bytes
```

`kind` 0 is never used; `length` is at most `OW_COMP_BODY_MAX` (256). The frame's decoder checks only this outer
shape. Each body is decoded on its own (`ow_comp_get`) by its kind's layout in section 14, strictly as in section 4.
A kind the reader does not know, or a body that does not decode, is skipped by its length and the other components of
the frame are used as normal. New kinds take new numbers; a kind's layout never changes within a version.

## 6. Handshake and sealing

Every account is an Ed25519 key, made on the player's first start from 32 random bytes and kept in the player's
profile. The server has its own Ed25519 key in its data folder. There are no passwords.

```
client                                            server
greet      magic, version, build, nonce_c, X_c  ->          (plain)
                                                 <-  challenge  nonce_s, ID_s, X_s, sig_s   (plain)
           both derive the keys and seal
proof      ID_c, sig_c, nickname, character, claim, features ->   (sealed)
                                                 <-  admit  account, nickname, rank, resumed, features, clock, rules
                                                     or refuse
```

1. The client makes a fresh X25519 key for the connection (secret from 32 random bytes) and sends `greet`.
2. The server reads magic and version first (`ow_greet_peek`). A wrong magic closes the connection; another version
   gets `refuse` with `OW_WHY_VERSION`, the server's version and its build text ("server is 0.5, update your game"),
   in the clear, before anything else is decoded.
3. The server makes its own X25519 key and computes the transcript hash
   `T = BLAKE2b-512("TKRW1 transcript" || greet payload || nonce_s || ID_s || X_s)`, where the greet payload is the
   canonical encoding of the greet as decoded. It signs `"TKRW1 server signs" || T` with its identity and sends
   `challenge`.
4. Both sides compute `K = X25519(own secret, peer's X)`. An all-zero `K` (a low-order point) ends the handshake
   (`OW_E_KEY`). Each direction's 48 bytes are `BLAKE2b-384(key = K, "TKRW1 client to server" || T)` and
   `BLAKE2b-384(key = K, "TKRW1 server to client" || T)`: a 32-byte key, then a 16-byte nonce prefix. The server
   seals its link after writing `challenge`.
5. The client checks the server's signature against `ID_s` (`OW_E_AUTH`). When it has seen this server before it also
   requires `ID_s` to be the key it recorded (`OW_E_PIN`); how it records keys and lets a player accept a new one is
   the client's business. It then seals its link and sends `proof`: its identity, its signature over
   `"TKRW1 client signs" || T`, the nickname it asks for, its character, a claim code or nothing, and the add-ons it
   offers.
6. The server checks the signature against `ID_c` (`OW_E_AUTH`, refused with `OW_WHY_AUTH`). A known key is admitted.
   An unknown key needs an unused claim code from the host, which binds the key to an account; without one the server
   refuses with `OW_WHY_UNKNOWN_KEY` (or admits it as a new account, when the host lets anyone in), with a bad one
   `OW_WHY_BAD_CLAIM`. A banned key gets `OW_WHY_BANNED`.
7. `admit` carries the account id, the name the room uses, the account's rank, whether the session was resumed, the
   granted add-ons, the server's clock and the room's rules. Both sides then set the granted features on the link.

The signatures cover the transcript, which holds both nonces and both exchange keys, so neither signature can be
replayed into another connection and a man in the middle cannot swap an exchange key without breaking a signature.
The keys come from per-connection X25519 secrets that are wiped once used, so a later theft of an identity key does
not open recorded traffic. Identity, nickname, claim code and features travel only sealed.

After the handshake every frame, `refuse` and `bye` included, is sealed with XChaCha20-Poly1305
(`crypto_aead_lock`):

- key: the direction's key;
- nonce (24 bytes): the direction's 16-byte prefix, then the direction's frame counter as `u64` little-endian;
- additional data: the frame's 6 header bytes;
- the encrypted payload replaces the payload, and the tag follows it.

Each direction's counter starts at 0 and goes up by one per frame, on the writer when it writes and on the reader
when a frame checks. No counter is sent: TCP keeps the order, so a frame that is changed, replayed, dropped, moved or
sent the other way fails its tag (`OW_E_SEAL`), and the connection ends. A counter at 2^64 - 1 ends it too.

## 7. Session

- **Join.** After `admit` the server sends what the granted add-ons need first (the save and world state, as
  `save_part` and `world_part`, only with `saves`), then `roster`, `teams` and `positions`, then `join_ready`. The
  game leaves the title only after `join_ready`. The others get `arrive` and a new `roster`.
- **Keep-alive.** Both sides send `ping` once a second and answer each with `pong` at once. 10 s with no frame ends
  the session (`OW_WHY_TIMEOUT`).
- **Clock.** Times are on the server's clock in microseconds. From a ping sent at local `t0` and its pong,
  received at `t1`, with the peer's `reply_us`: round trip `t1 - t0`, offset `reply_us - (t0 + (t1 - t0) / 2)`
  (`ow_clock_sample`). `admit` gives the first estimate; every pong refines it.
- **Area and position.** A game sends `area` on every change of scene, section or state (a loading screen is
  `OW_PLACE_LOADING`, not a disconnect) and `position` about once a second, and at once after a teleport. The server
  sends `positions` about once a second to every game.
- **Leaving.** `bye` either way, or `refuse` from the server, then close. The others get `depart` and a new `roster`.
- **Reconnect.** The server holds a lost session for `rules.hold_ms` (30 s). A new connection with the same key
  within the hold resumes it: same account, team and rank, `admit.resumed` = 1, and the others see no `depart`. An
  older session of the same account that is still open ends with `OW_WHY_DUPLICATE`.

## 8. Rules of the core messages

**Ranks.** Each account is a player, a moderator or the host (`OW_RANK_*`). The server enforces what each may do;
a refused action answers with `mod_done` (host tools) or `tp_result` (teleports) and changes nothing.

| Action | Player | Moderator | Host |
|---|---|---|---|
| chat, whisper, teams, teleport requests, markers | yes | yes | yes |
| `announce` | no | yes | yes |
| `mod_action` kick, mute, unmute | no | players only | anyone but the host |
| `mod_action` ban, unban, set rank | no | no | yes |
| `summon` | no | no | yes |

**Chat.** `say` with `OW_SAY_ALL` goes to everyone, `OW_SAY_TEAM` to the sender's team, `OW_SAY_WHISPER` to the
account in `to` and back to the sender, `OW_SAY_ME` to everyone as an action line. The server sends each as `chat`
with the matching kind, the speaker's account and name; `from` 0 with `OW_CHAT_SYSTEM` is the server's own line. A
text starting with `/` is a command the server reads and answers with a system line. A muted player's lines are
dropped and answered with a system line saying so.

**Teams.** A player is in at most one team. `team_create` makes one with the creator as leader; `team_invite` from
any member sends `team_invited` to the target, which joins with `team_join`. An open team can be joined without an
invitation. `team_leave` leaves; the last member leaving ends the team, and a leader leaving hands the lead to the
member who joined first. `team_kick` is the leader's. Every change sends `teams` and `roster` to everyone. Team ids
are never 0 and are not reused while the server runs.

**Teleport.** `tp_ask` sends `tp_asked` to the target, unless the room's rules turn teleports off (`OW_RULE_TELEPORT`
clear: `tp_result` `OW_TP_NOT_ALLOWED`). The target answers with `tp_reply`; on accept the server sends the asker
`go_to` with the target's last position (`OW_GO_REQUEST`) and `tp_result` `OW_TP_ACCEPTED`, on refuse
`OW_TP_REFUSED`. No answer in 30 s is `OW_TP_TIMEOUT`; a target who left is `OW_TP_GONE`; a second request while one
is open is `OW_TP_BUSY`. The host's `summon` sends `go_to` with the host's position (`OW_GO_SUMMON`) to one player, a
team or everyone, without asking.

**Host tools.** `mod_action` kicks (the target gets `refuse` with `OW_WHY_KICKED` and the reason), bans for
`minutes` or until lifted (`OW_WHY_BANNED`, and the key is refused until then), unbans, mutes and unmutes for
`minutes`, or sets a rank. Each is answered with `mod_done` and told to everyone as a system line. `announce` is
shown to everyone as a `notice` of kind `OW_NOTICE_ANNOUNCE`. The host's changes to the room's rules reach every
game as `rules`.

**Markers.** `mark` places or removes one of a player's four shared markers; the server sends `marker` to the
player's team, or to everyone when the player has no team.

## 9. Features and add-ons

An add-on is a group of message types behind one `OW_FEAT_*` bit. The client offers the add-ons it has in
`proof.features`; the server answers in `admit.features` with the ones it grants, a subset of the offer. Both set
the result on the link (`ow_link_set_features`), and from then on a type of an add-on that was not granted is refused
like a bad frame, by writer and reader alike (`OW_E_FEATURE`). A bit a side does not know is ignored in an offer and
never granted. New add-ons take new bits and new message numbers in a later version.

| Bit | Feature | Types |
|---|---|---|
| 1 | `puppets` | `self_state` `player_state` |
| 2 | `saves` | `save_part` `world_part` `save_write` `world_write` `world_set` `save_fix` |
| 4 | `actors` | `offer` to `taken` (0x0400 to 0x040E) |
| 8 | `duels` | `duel_ask` `duel_asked` `duel_reply` `duel` |
| 16 | `economy` | `wallet_ask` `wallet` `ledger_note` `ledger` |
| 32 | `companions` | `companion` |
| 64 | `sky` | `sky` `sky_set` |
| 128 | `emotes` | `emote` `emoted` |

The add-ons' behaviour is set when each is built; until then their entries in section 12 fix only the layout. The
actors add-on's is below (9.1); it adds no message or field to version 1, only the rules both ends follow.

### 9.1 The actors add-on

Rules for `offer` to `taken` (0x0400 to 0x040E), `herd` aside: its leader rule is the wildlife work's and is not
described here yet, past its numbering below. The server's side is `server/sv_actors.c` (records, interest,
relays) and `server/sv_owner.c` (the owner rules); `tests/actor_lab.py` replays the cases that set them.

**The class table.** `online/proto/classes.txt` says what is shared: per actor-name pattern its class (`OW_CLASS_*`),
how its id is formed, its components and its owner rules (the file's header gives the format). Both ends compile the
same table: `tools/gen_classes.py` writes it into `online/proto/ow_classes_data.h`, `ow_classes.c` reads it
(`ow_class_find`), and `tests/net_proto_check.py` fails when the two files differ. A row is a name prefix or `*TEXT`, a name
containing TEXT; a matching `*TEXT` row decides before any prefix row, and among rows of one kind the longest pattern
decides. A name no row matches, or one whose row is `local`, is not shared: the game keeps its own and never offers
it, and the server ignores an offer or spawn of it. The table's class wins over the class a message names.

**Hashes and cells.** Every name hash on the wire (`herd_row.name_hash`, `anim_slot.anim`, the `gear` component) is
FNV-1a 32 over the name's UTF-8 bytes without the terminating NUL (`ow_name_hash`). `herd.cell` is the 250 m grid:
column `floor(x / 250)` in the low 16 bits and row `floor(z / 250)` in the high 16 bits, each a signed 16-bit number
(`ow_herd_cell`); it is a label only, not the map section of `place`.

**Ids.** A placed actor is `origin` 0 and `serial` its map instance id, the same in every game. A run-time actor is
`origin` the creating account and `serial` that game's counter, which starts at the session's `admit.server_time_us`
and goes up by one per actor, so it is not reused, not even across a reconnect. The server refuses a run-time id whose
origin is not the sender, and a placed id for a row whose identity is `creator` only.

**Records.** The server keeps one record per shared actor: id, class, name, scene (the scene of the game that first
offered or spawned it), owner (an account, or 0 for none), owner epoch, the last state and its time, the games that have
it loaded, and the time of its last owner change. The epoch is 0 before the first owner and goes up by one at every
change, to an owner or to none. Anything about an actor that names an older epoch is dropped, on both ends.

**Interest.** A game is in an actor's interest while its player is in the actor's scene and within
`rules.interest_m` (250 m) of the actor; it leaves the interest only 20 m further out, so a player on the edge does not
flicker. Games in interest receive `owner`, `actor_recv`, `spawned` and `gone` for the actor; other games receive
nothing about it. A player's scene is its last `area`; its position the newest of its `position` and `self_state`.

**What a game sends.**

- `offer` once, when the actor is loaded and running in its game and the table shares it, and again after it loaded the
  actor anew. An offer says "I have it and can run it". The server keeps the game among the actor's *candidates* until
  the game sends `gone` with `OW_GONE_UNLOADED` or leaves, and decides by the rules below, now or later, whether it
  gets the actor. A game does not repeat an offer to ask again.
- `actor_send` for every actor it owns, at least every 250 ms each, still ones included, under the epoch the server
  gave it. States from a game that is not the owner, or under another epoch, are dropped.
- `release` when it owns an actor it can no longer run. `gone` with `OW_GONE_UNLOADED` when it unloads an actor it
  offered or was told about; from the owner that is a release too.
- `gone` with `OW_GONE_REMOVED`, `OW_GONE_KILLED` or `OW_GONE_TAKEN` only as the owner: the actor no longer exists
  anywhere. From any other game these are dropped: a copy does not die by itself, its owner's state or `gone` ends it.
- `hold` when its player begins or ends an exclusive hold: Ultrahand, carrying, riding.
- `hit` for every hit its player lands on a copy, with the epoch it knows.
- `spawn` for a run-time actor its game created and the table shares, once it has an id. A spawn whose `parent` is a
  record another game owns is a copy's doing (a copy's summons or drop) and is refused: the server answers the creator
  with `gone` `OW_GONE_REMOVED`, and the creator removes its actor.
- `group` as the owner of the members, when its game glues actors into one structure or comes apart; an empty list ends
  the group. The server keeps the members it has records of.

**What the server sends.** `owner` to every game in interest whenever the owner changes, with the new epoch, the cause
and the last state it has, so the new owner seeds its actor from it before it runs it and the others turn theirs into
copies at the same epoch. `owner` 0 means nobody runs it: every game shows it still. A game that comes into an
actor's interest gets its current `owner` once. `hold_ms` is how long the actor stays with this owner at least, unless
the owner leaves or a player holds it. `actor_recv` relays each owner's states to the games in interest, `hit_in`
takes a hit to the owner with the attacker's account, `spawned` and `gone` tell the games in interest.

**The owner rules.** Distances run from a player's position to the actor's last position (for a group, its centre). A
player is *present* while its game is connected and its place is `OW_PLACE_PLAYING`. The numbers are the ones
SyncFix's rounds settled; `server/sv_owner.h` names each.

| # | Rule | Cause |
|---|---|---|
| 1 | **No owner.** The nearest candidate gets it when its player is within 120 m and no other present player is 5 m nearer. A nearer player whose game has not offered within 3 s stops holding it back. Otherwise the actor stays without an owner until a near game offers. | `OFFER` |
| 2 | **Owner far.** When the owner's player is over 120 m away, a candidate within 120 m that is 10 m nearer takes it, but only 5 s after the actor's last change. | `FAR` |
| 3 | **Clearly closer.** A candidate whose player is at most half the owner's distance away and 15 m nearer takes it, but only 20 s after the actor's last change, wherever the players are. | `CLOSER` |
| 4 | **Pick-up.** For items, world objects and devices: a candidate whose player is within 3 m, with the owner's player further away, takes it at once; when both players are within 20 m it waits the same 20 s. | `PICKUP` |
| 5 | **Still things keep their owner.** For a row marked `still`, an actor that has not moved 1 m in the last 5 s does not change owner by rule 3. | |
| 6 | **Asleep.** An actor whose owner's last state has `OW_POSE_ASLEEP` does not change owner by rules 2 or 3. | |
| 7 | **Fights.** An enemy (row `fight`) hit in the last 10 s, and a boss (row `lock`) while a present player is within 120 m, stay with their owner: rules 3 and 4 do not apply, rule 2 only beyond 160 m, and an owner paused for over 6 s counts as away (rule 9). | |
| 8 | **Followers.** A part (row `part=PREFIX`) goes to the owner of the nearest record within 60 m whose name starts with the prefix and that is not a part itself, and follows it on every change; a hit on a part counts as one on its body for rule 7. A held object (row `held`) within 12 m of an enemy or boss goes to that one's owner, unless a player holds it. A spawn whose parent is a record follows the parent's owner (summons). A row marked `fixed` (projectiles) never changes owner. A row marked `quest`, once owned, moves only by rules 9 and 10. | the leader's |
| 9 | **The owner leaving.** An owner that is away (loading, dead, on the title, or its connection lost and held; paused, after 6 s) frees its actors for rule 1 at once. Until a candidate qualifies they stay with it, so a quick reconnect finds them. When its session ends for good, each actor it owned goes by rule 1, or to owner 0. | `LEFT` |
| 10 | **Holds.** `hold` on takes the actor, and its whole group, for the holder at once and at any distance, unless another game holds it: then the asker gets `owner` restating the holder, and lets go. While held, no other rule moves it. When the hold ends the actor stays with the holder 5 s at least. | `HOLD` |
| 11 | **Groups.** A group (from `group`, or for rows marked `cluster` the actors within 6 m of one another, chained, when one is a `core` row or two have moved 1 m since the server first saw them) is decided as one record at its centre, by the same rules, and its members change together. | the group's |

The server tries the rules for each actor once a second and on every offer, release, hold and leave, in this order:
holds, the owner leaving, followers, then 1 to 4 limited by 5 to 7. Rules 2 to 4 need the candidate to have offered
the actor, so a game never gets one it cannot run. In steady play the rules keep owner changes under 5 a minute for
the whole room, which `tests/actor_lab.py` measures on every case.

**Hits.** The server passes a `hit` to the owner as `hit_in` when the target is a record under that epoch, its owner is
another game and the hitter's player is within 80 m of it; otherwise the hit is dropped, and so is a hit on an actor
with no owner. At most 120 hits in 5 s from one game.

**Spawns and gone.** A spawned actor's record starts owned by its creator at epoch 1, and `spawned` goes to the other
games in interest. The creator gets no `owner` for it: it owns the actor from its `spawn` on, and an `owner` follows
only a hand-over. A drop is such an actor (class `item`, `parent` what made it).

**Take-once.** For a row marked `take`, a game whose player picks the thing up sends `take` with the epoch it knows,
and does not add it to its pouch yet. The first `take` of a record the server has wins: the taker's game gets `taken`
with `yours` 1 and adds it, the other games in interest get `taken` with `yours` 0 and remove their copy, and the
record goes (as `gone` `OW_GONE_TAKEN`, which the server does not send again). A later `take` of the same thing, or
one of an id the server has no record of, gets `taken` with the first taker and `yours` 0, so its game leaves the
thing alone. A taken id is remembered for 10 minutes. A record goes when its owner sends `gone`, when
the last game that has it loaded unloads it and it has no owner, and for a run-time actor when its creator's session
ends and nobody else has it loaded.

## 10. Versions

- `OW_VERSION` is the protocol version, now 1. A client of version N talks only to a server of version N; within a
  version, add-ons are negotiated with features (section 9).
- Within a version nothing changes: no message, field, order, limit, feature, value set, component layout or number.
  Any change to `online/proto/ow_messages.h` or to a constant of this document raises `OW_VERSION` and this
  document's title in the same commit.
- Each message type therefore has one fixed layout per version, and its number never takes another meaning: a removed
  type's number is not reused.
- Frozen across all versions, so that a server can refuse any client with a reason: type `0x0001` is `greet` and
  starts with `u32 magic` and `u16 version` with a payload limit of 1024; type `0x0002` is `refuse` with exactly its
  version 1 fields and limit 1024, allowed in both stages; `OW_WHY_*` numbers keep their meaning.
- The refusal names the server's version and build so the player knows which side to update.
- `tests/net_proto_check.py` keeps a SHA-256 fingerprint of the tables for each version and fails when the tables of
  a version change, so a layout change cannot ship under the old number.

## 11. Limits and value sets

| Constant | Value | What |
|---|---|---|
| `OW_PAYLOAD_MAX` | 65000 | largest payload of any type |
| `OW_HEADER` | 6 | frame header |
| `OW_TAG` | 16 | tag of a sealed frame |
| `OW_BUILD_MAX` | 64 | build text |
| `OW_DETAIL_MAX` | 240 | refusal and bye text |
| `OW_NICK_MAX` | 32 | nickname |
| `OW_CLAIM_MAX` | 32 | claim code |
| `OW_TEXT_MAX` | 256 | chat line |
| `OW_NOTICE_MAX` | 480 | notice and server message text |
| `OW_REASON_MAX` | 120 | reason of a host tool |
| `OW_TEAM_NAME_MAX` | 24 | team name |
| `OW_TEAMS_MAX` | 16 | teams in the list |
| `OW_PLAYERS_MAX` | 32 | roster rows and positions |
| `OW_ACTOR_NAME_MAX` | 64 | actor name |
| `OW_ANIM_NAME_MAX` | 64 | animation name |
| `OW_GD_BATCH` | 200 | GameData values per message |
| `OW_POSE_BATCH` | 48 | actor states per batch |
| `OW_GROUP_MAX` | 64 | group members |
| `OW_HERD_MAX` | 64 | animals per herd message |
| `OW_COMPS_MAX` | 8 | components per state |
| `OW_COMP_BODY_MAX` | 256 | component body |

A field typed `u8<=N` takes one of these sets; N is its last value. Numbers keep their meaning within a version.

| Set | Values |
|---|---|
| `OW_CHAR_*` | 0 Link, 1 Zelda |
| `OW_RANK_*` | 0 player, 1 moderator, 2 host |
| `OW_PLACE_*` | 0 title, 1 playing, 2 loading, 3 paused, 4 dead |
| `OW_RULE_*` (bits of `room_rules.options`) | 1 drops shared, 2 teleport allowed, 4 player hits allowed outside duels |
| `OW_SAY_*` | 0 all, 1 team, 2 whisper, 3 `/me` |
| `OW_CHAT_*` | 0 all, 1 team, 2 whisper, 3 `/me`, 4 system |
| `OW_NOTICE_*` | 0 message of the day, 1 toast, 2 warning, 3 server message |
| marker slot | 0 to 3 |
| `OW_TP_*` | 0 accepted, 1 refused, 2 timed out, 3 not allowed, 4 target gone, 5 busy |
| `OW_GO_*` | 0 accepted request, 1 summon |
| `OW_SUMMON_*` | 0 one player, 1 a team, 2 everyone |
| `OW_MOD_*` | 0 kick, 1 ban, 2 unban, 3 mute, 4 unmute, 5 set rank |
| `OW_MOD_OUTCOME_*` | 0 done, 1 not allowed, 2 unknown account, 3 already so |
| `OW_WHY_*` (`u16`) | 0 goodbye, 1 version, 2 protocol, 3 auth, 4 unknown key, 5 bad claim, 6 full, 7 banned, 8 shutdown, 9 timeout, 10 duplicate, 11 kicked |
| `OW_GD_*` | 0 bool, 1 s32, 2 f32, 3 string, 4 vec3, 5 u32, 6 s64, 7 u64, 8 binary |
| `OW_POSE_*` (bits) | 1 still, 2 asleep, 4 held |
| `OW_HIT_*` | 0 weapon, 1 arrow, 2 bomb, 3 thrown, 4 elemental, 5 other |
| `OW_ELEMENT_*` | 0 none, 1 fire, 2 ice, 3 shock, 4 water, 5 wind, 6 light, 7 gloom |
| `OW_CLASS_*` | 0 map object, 1 world object, 2 enemy, 3 boss, 4 animal, 5 townsperson, 6 item, 7 projectile, 8 puppet, 9 device |
| `OW_OWNER_*` | 0 offer granted, 1 owner far, 2 clearly closer, 3 pick-up, 4 owner left, 5 hold, 6 host |
| `OW_HOLD_*` | 0 none, 1 Ultrahand, 2 carrying, 3 riding |
| `OW_GONE_*` | 0 removed, 1 killed, 2 taken, 3 unloaded |
| `OW_DUEL_*` | 0 asked, 1 count-down, 2 live, 3 ended |
| `OW_DUEL_END_*` | 0 none, 1 won, 2 left the ring, 3 loading screen, 4 time, 5 declined, 6 cancelled |
| `OW_WALLET_*` | 0 spend, 1 earn, 2 trade, 3 stake |
| `OW_LEDGER_*` | 0 Light found, 1 Light spent, 2 seed found, 3 seed spent |
| `OW_COMPANION_*` | 0 horse, 1 Sage, 2 other |
| `OW_DRAWN_*` | 0 nothing, 1 weapon, 2 bow, 3 shield |

## 12. Messages

Each entry: number and name; direction, stage, payload limit and smallest payload in bytes, and feature (`core` or
the add-on); then the fields in wire order.

### Handshake and session

#### 0x0001 greet

c2s · plain · limit 1024 · smallest 72 · core

First frame of every connection, client to server, in the clear.

| Field | Type | Meaning |
|---|---|---|
| `magic` | `u32` | `0x57524B54`, the bytes `TKRW`. Frozen. |
| `version` | `u16` | The client's protocol version. Frozen. |
| `build` | `str(64)` | The client's build text, for the host's log. |
| `nonce` | `b32` | 32 random bytes. |
| `exchange` | `b32` | The client's X25519 public key for this connection. |

#### 0x0002 refuse

s2c · any · limit 1024 · smallest 8 · core

Why the server ends a connection. In the clear before sealing, sealed after. Frozen in every version.

| Field | Type | Meaning |
|---|---|---|
| `reason` | `u16` | An `OW_WHY_*` reason. |
| `version` | `u16` | The server's protocol version. |
| `build` | `str(64)` | The server's build text ("0.5"). |
| `detail` | `str(240)` | Text for the player. |

#### 0x0003 challenge

s2c · plain · limit 160 · smallest 160 · core

The server's answer to a greet, in the clear.

| Field | Type | Meaning |
|---|---|---|
| `nonce` | `b32` | 32 random bytes. |
| `identity` | `b32` | The server's Ed25519 public key. |
| `exchange` | `b32` | The server's X25519 public key for this connection. |
| `signature` | `b64` | The server's signature over its label and the transcript. |

#### 0x0004 proof

c2s · sealed · limit 169 · smallest 105 · core

The client proves its key; first sealed frame.

| Field | Type | Meaning |
|---|---|---|
| `identity` | `b32` | The player's Ed25519 public key (`profile/net/identity.key`). |
| `signature` | `b64` | The player's signature over its label and the transcript. |
| `nickname` | `str(32)` | The name the player asks for. |
| `character` | `u8<=1` | `OW_CHAR_*`: Link or Zelda. |
| `claim` | `str(32)` | A one-time claim code from the host, or empty. |
| `features` | `u32` | The `OW_FEAT_*` add-ons the client offers. |

#### 0x0005 admit

s2c · sealed · limit 69 · smallest 37 · core

The server lets the client in.

| Field | Type | Meaning |
|---|---|---|
| `account` | `u64` | The account id the key belongs to. |
| `nickname` | `str(32)` | The name the room shows (the server may change it). |
| `rank` | `u8<=2` | `OW_RANK_*`: player, moderator or host. |
| `resumed` | `bool` | 1 when this connection resumed a held session. |
| `features` | `u32` | The offered add-ons the server grants; the link carries only these. |
| `server_time_us` | `u64` | The server's clock when it wrote the frame. |
| `rules` | `room_rules` | The room's rules. |

#### 0x0006 bye

both · sealed · limit 244 · smallest 4 · core

An orderly end, either way; the sender closes after it.

| Field | Type | Meaning |
|---|---|---|
| `reason` | `u16` | An `OW_WHY_*` reason. |
| `detail` | `str(240)` | Text for the log or the player. |

#### 0x0007 join_ready

s2c · sealed · limit 6 · smallest 6 · core

Sent after admit, what the granted add-ons need (save, world state) and the roster: the game may leave the title.

| Field | Type | Meaning |
|---|---|---|
| `save_parts` | `u16` | Parts of the save sent (0 without saves). |
| `world_parts` | `u16` | Parts of the world state sent (0 without saves). |
| `players` | `u16` | Players in the roster sent. |

#### 0x0010 ping

both · sealed · limit 12 · smallest 12 · core

Sent each second by both sides.

| Field | Type | Meaning |
|---|---|---|
| `seq` | `u32` | Counter of the sender's pings. |
| `sent_us` | `u64` | The sender's clock. |

#### 0x0011 pong

both · sealed · limit 20 · smallest 20 · core

The answer to a ping, sent at once.

| Field | Type | Meaning |
|---|---|---|
| `seq` | `u32` | The ping's seq. |
| `echo_us` | `u64` | The ping's sent_us. |
| `reply_us` | `u64` | The answering side's clock. |

### Saves and world state (add-on)

#### 0x0100 save_part

s2c · sealed · limit 53006 · smallest 6 · saves

One part of the account's save, sent on join.

| Field | Type | Meaning |
|---|---|---|
| `part` | `u16` | Part number from 0. |
| `parts` | `u16` | How many parts the save has. |
| `entries` | `list(200, gd_entry)` | Per-player GameData values. |

#### 0x0101 world_part

s2c · sealed · limit 53006 · smallest 6 · saves

One part of the room's world state, sent on join after the save.

| Field | Type | Meaning |
|---|---|---|
| `part` | `u16` | Part number from 0. |
| `parts` | `u16` | How many parts the world state has. |
| `entries` | `list(200, gd_entry)` | Shared world GameData values. |

#### 0x0103 save_write

c2s · sealed · limit 53006 · smallest 6 · saves

Per-player GameData writes, batched every 250 ms.

| Field | Type | Meaning |
|---|---|---|
| `seq` | `u32` | Counter of the client's batches. |
| `entries` | `list(200, gd_entry)` | The values written. |

#### 0x0104 world_write

c2s · sealed · limit 53006 · smallest 6 · saves

Shared world GameData writes, batched every 250 ms; the server applies each key's rule.

| Field | Type | Meaning |
|---|---|---|
| `seq` | `u32` | Counter of the client's batches. |
| `entries` | `list(200, gd_entry)` | The values written. |

#### 0x0105 world_set

s2c · sealed · limit 53010 · smallest 10 · saves

World values the server accepted, sent to every game.

| Field | Type | Meaning |
|---|---|---|
| `source` | `u64` | The account whose write it was (0: the server). |
| `entries` | `list(200, gd_entry)` | The values as the room now has them. |

#### 0x0106 save_fix

s2c · sealed · limit 53006 · smallest 6 · saves

The server corrects per-player values (wallet, ledger, a clamped write).

| Field | Type | Meaning |
|---|---|---|
| `seq` | `u32` | The save_write batch it answers, or 0. |
| `entries` | `list(200, gd_entry)` | The corrected values. |

### Presence

#### 0x0200 area

c2s · sealed · limit 9 · smallest 9 · core

Where the game is; sent on every change.

| Field | Type | Meaning |
|---|---|---|
| `where` | `place` | Scene, section and state. |

#### 0x0201 vitals

c2s · sealed · limit 5 · smallest 5 · core

Numbers for the player list; sent when they change, at most once a second.

| Field | Type | Meaning |
|---|---|---|
| `hearts_q` | `u16` | Hearts in quarters. |
| `hearts_max_q` | `u16` | Heart containers in quarters. |
| `fps` | `u8` | Frame rate. |

#### 0x0202 roster

s2c · sealed · limit 2050 · smallest 2 · core

The whole player list; sent on join and on every change.

| Field | Type | Meaning |
|---|---|---|
| `players` | `list(32, roster_row)` | One row per player. |

#### 0x0203 arrive

s2c · sealed · limit 43 · smallest 11 · core

A player joined (for the toast).

| Field | Type | Meaning |
|---|---|---|
| `account` | `u64` | Their account. |
| `nickname` | `str(32)` | Their name. |
| `character` | `u8<=1` | `OW_CHAR_*`. |

#### 0x0204 depart

s2c · sealed · limit 10 · smallest 10 · core

A player left (for the toast).

| Field | Type | Meaning |
|---|---|---|
| `account` | `u64` | Their account. |
| `reason` | `u16` | An `OW_WHY_*` reason. |

#### 0x0205 position

c2s · sealed · limit 20 · smallest 20 · core

The player's position at a low rate (about once a second, and on a teleport or a loading screen), for the map and for teleports.

| Field | Type | Meaning |
|---|---|---|
| `scene` | `u32` | Scene hash. |
| `pos` | `vec3` | Position, metres. |
| `yaw` | `f32` | Heading, radians. |

#### 0x0206 positions

s2c · sealed · limit 898 · smallest 2 · core

Positions of the players this game may see (everyone, or its team when the host hides others), about once a second.

| Field | Type | Meaning |
|---|---|---|
| `spots` | `list(32, spot)` | One row per player. |

#### 0x0280 self_state

c2s · sealed · limit 2133 · smallest 61 · puppets

The player's own state, at least every 250 ms while in the world.

| Field | Type | Meaning |
|---|---|---|
| `time_us` | `u64` | Server-clock time of the state. |
| `body` | `body` | Pose and velocity. |
| `comps` | `comps(8)` | Animation, gear, glide, aim, ride. |

#### 0x0281 player_state

s2c · sealed · limit 2141 · smallest 69 · puppets

Another player's state, relayed to games within interest.

| Field | Type | Meaning |
|---|---|---|
| `account` | `u64` | Whose state. |
| `time_us` | `u64` | Server-clock time of the state. |
| `body` | `body` | Pose and velocity. |
| `comps` | `comps(8)` | As in self_state. |

### Teams

#### 0x0300 team_create

c2s · sealed · limit 27 · smallest 3 · core

Make a team and lead it. The creator leaves any team it was in.

| Field | Type | Meaning |
|---|---|---|
| `name` | `str(24)` | The team's name. |
| `open` | `bool` | 1: anyone may join; 0: only the invited. |

#### 0x0301 team_invite

c2s · sealed · limit 8 · smallest 8 · core

Invite a player to the sender's team. Any member may invite.

| Field | Type | Meaning |
|---|---|---|
| `target` | `u64` | The account. |

#### 0x0302 team_invited

s2c · sealed · limit 70 · smallest 14 · core

Someone invites this player to a team (the toast).

| Field | Type | Meaning |
|---|---|---|
| `team` | `u16` | The team id. |
| `name` | `str(24)` | The team's name. |
| `from` | `u64` | The inviting account. |
| `nickname` | `str(32)` | Their name. |

#### 0x0303 team_join

c2s · sealed · limit 2 · smallest 2 · core

Join a team: one that is open, or one that invited this player.

| Field | Type | Meaning |
|---|---|---|
| `team` | `u16` | The team id. |

#### 0x0304 team_leave

c2s · sealed · limit 2 · smallest 2 · core

Leave a team. A team whose last member leaves is gone; a leader who leaves hands the lead to the member who joined first.

| Field | Type | Meaning |
|---|---|---|
| `team` | `u16` | The team id. |

#### 0x0305 team_kick

c2s · sealed · limit 8 · smallest 8 · core

The leader removes a member.

| Field | Type | Meaning |
|---|---|---|
| `target` | `u64` | The member's account. |

#### 0x0306 teams

s2c · sealed · limit 610 · smallest 2 · core

The team list; sent on join and on every change. Who is in which team is in the roster.

| Field | Type | Meaning |
|---|---|---|
| `list` | `list(16, team_row)` | One row per team. |

### Actors (add-on)

#### 0x0400 offer

c2s · sealed · limit 95 · smallest 31 · actors

The game has this actor loaded and running and offers to own it.

| Field | Type | Meaning |
|---|---|---|
| `id` | `aid` | The actor. |
| `class` | `u8<=9` | `OW_CLASS_*`. |
| `name` | `str(64)` | The game's actor name. |
| `pos` | `vec3` | Where it is. |

#### 0x0401 release

c2s · sealed · limit 20 · smallest 20 · actors

The owner gives an actor up.

| Field | Type | Meaning |
|---|---|---|
| `id` | `aid` | The actor. |
| `epoch` | `u32` | The owner epoch it held. |

#### 0x0402 actor_send

c2s · sealed · limit 60000 · smallest 10 · actors

States of actors this game owns, at least every 250 ms per owned actor.

| Field | Type | Meaning |
|---|---|---|
| `time_us` | `u64` | Server-clock time of the states. |
| `items` | `list(48, actor_pose)` | One state per actor. |

#### 0x0403 actor_recv

s2c · sealed · limit 60000 · smallest 18 · actors

States of actors other games own, relayed within interest.

| Field | Type | Meaning |
|---|---|---|
| `source` | `u64` | The owning account. |
| `time_us` | `u64` | Server-clock time of the states. |
| `items` | `list(48, actor_pose)` | One state per actor. |

#### 0x0404 owner

s2c · sealed · limit 2234 · smallest 98 · actors

The owner of an actor changed: a hand-over with the last full state.

| Field | Type | Meaning |
|---|---|---|
| `id` | `aid` | The actor. |
| `class` | `u8<=9` | `OW_CLASS_*`. |
| `name` | `str(64)` | The game's actor name, for a game that has to create it. |
| `epoch` | `u32` | The new owner epoch. |
| `owner` | `u64` | The new owner's account, 0 for none. |
| `cause` | `u8<=6` | `OW_OWNER_*`: which rule moved it. |
| `hold_ms` | `u32` | How long the actor stays with this owner at least. |
| `state_time_us` | `u64` | Server-clock time of the state below. |
| `body` | `body` | The last state. |
| `flags` | `u8<=7` | `OW_POSE_*` bits. |
| `comps` | `comps(8)` | The last components (life, behaviour, animation). |

#### 0x0405 hold

c2s · sealed · limit 18 · smallest 18 · actors

An exclusive hold begins or ends (Ultrahand, carrying, riding).

| Field | Type | Meaning |
|---|---|---|
| `id` | `aid` | The actor (or group). |
| `what` | `u8<=3` | `OW_HOLD_*`. |
| `on` | `bool` | 1 begins, 0 ends. |

#### 0x0406 hit

c2s · sealed · limit 1103 · smallest 67 · actors

A hit on a copy, for its owner.

| Field | Type | Meaning |
|---|---|---|
| `info` | `hit_info` | The hit. |

#### 0x0407 hit_in

s2c · sealed · limit 1111 · smallest 75 · actors

A hit on an actor this game owns.

| Field | Type | Meaning |
|---|---|---|
| `attacker` | `u64` | The account that hit. |
| `info` | `hit_info` | The hit. |

#### 0x0408 spawn

c2s · sealed · limit 2224 · smallest 88 · actors

The game created a run-time actor (drop, spawn, summon, projectile).

| Field | Type | Meaning |
|---|---|---|
| `info` | `spawn_info` | The new actor. |

#### 0x0409 spawned

s2c · sealed · limit 2232 · smallest 96 · actors

Another game created a run-time actor.

| Field | Type | Meaning |
|---|---|---|
| `creator` | `u64` | The creating account. |
| `info` | `spawn_info` | The new actor. |

#### 0x040A gone

both · sealed · limit 21 · smallest 21 · actors

An actor no longer exists.

| Field | Type | Meaning |
|---|---|---|
| `id` | `aid` | The actor. |
| `epoch` | `u32` | The owner epoch. |
| `cause` | `u8<=3` | `OW_GONE_*`. |

#### 0x040B group

both · sealed · limit 1042 · smallest 18 · actors

The members of a glued structure or contraption, decided as one.

| Field | Type | Meaning |
|---|---|---|
| `id` | `aid` | The group's id. |
| `members` | `list(64, aid)` | Its actors. |

#### 0x040C herd

both · sealed · limit 2318 · smallest 14 · actors

The wildlife leader's list of id-less animals in a cell.

| Field | Type | Meaning |
|---|---|---|
| `cell` | `u32` | The map cell. |
| `leader` | `u64` | The leading account (the server fills it; a client sends 0). |
| `beasts` | `list(64, herd_row)` | The animals. |

#### 0x040D take

c2s · sealed · limit 20 · smallest 20 · actors

A pick-up request for a take-once actor.

| Field | Type | Meaning |
|---|---|---|
| `id` | `aid` | The actor. |
| `epoch` | `u32` | The owner epoch seen. |

#### 0x040E taken

s2c · sealed · limit 25 · smallest 25 · actors

Who got a take-once actor.

| Field | Type | Meaning |
|---|---|---|
| `id` | `aid` | The actor. |
| `taker` | `u64` | The account that has it. |
| `yours` | `bool` | 1 for the taker's game. |

### Chat and interface

#### 0x0500 say

c2s · sealed · limit 267 · smallest 11 · core

A chat line, a `/` command, or a whisper.

| Field | Type | Meaning |
|---|---|---|
| `kind` | `u8<=3` | `OW_SAY_*`: all, team, whisper or `/me`. |
| `to` | `u64` | The whispered account; 0 otherwise. |
| `text` | `str(256)` | The line. |

#### 0x0501 chat

s2c · sealed · limit 309 · smallest 21 · core

A chat line to show.

| Field | Type | Meaning |
|---|---|---|
| `from` | `u64` | The speaker's account, 0 for the server. |
| `to` | `u64` | The whispered account; 0 otherwise. |
| `kind` | `u8<=4` | `OW_CHAT_*`. |
| `nickname` | `str(32)` | The speaker's name. |
| `text` | `str(256)` | The line. |

#### 0x0502 notice

s2c · sealed · limit 487 · smallest 7 · core

Message of the day, a toast, a warning or a server message.

| Field | Type | Meaning |
|---|---|---|
| `kind` | `u8<=3` | `OW_NOTICE_*`. |
| `ttl_ms` | `u32` | How long to show it, 0 for the default. |
| `text` | `str(480)` | The text. |

#### 0x0503 mark

c2s · sealed · limit 18 · smallest 18 · core

Place or remove one of the player's shared map markers.

| Field | Type | Meaning |
|---|---|---|
| `slot` | `u8<=3` | Marker slot. |
| `on` | `bool` | 1 places, 0 removes. |
| `scene` | `u32` | Scene hash. |
| `pos` | `vec3` | Where. |

#### 0x0504 marker

s2c · sealed · limit 26 · smallest 26 · core

A player's shared marker, sent to their team.

| Field | Type | Meaning |
|---|---|---|
| `account` | `u64` | Whose. |
| `slot` | `u8<=3` | Marker slot. |
| `on` | `bool` | 1 placed, 0 removed. |
| `scene` | `u32` | Scene hash. |
| `pos` | `vec3` | Where. |

#### 0x0505 character

c2s · sealed · limit 1 · smallest 1 · core

The player chose Link or Zelda.

| Field | Type | Meaning |
|---|---|---|
| `character` | `u8<=1` | `OW_CHAR_*`. |

#### 0x0506 rules

s2c · sealed · limit 13 · smallest 13 · core

The host changed the room's rules.

| Field | Type | Meaning |
|---|---|---|
| `rules` | `room_rules` | The new rules. |

### Teleport and host tools

#### 0x0600 tp_ask

c2s · sealed · limit 8 · smallest 8 · core

Ask to teleport to a player.

| Field | Type | Meaning |
|---|---|---|
| `target` | `u64` | The player. |

#### 0x0601 tp_asked

s2c · sealed · limit 42 · smallest 10 · core

Someone asks to teleport to this player (the toast with accept and refuse).

| Field | Type | Meaning |
|---|---|---|
| `from` | `u64` | The asking account. |
| `nickname` | `str(32)` | Their name. |

#### 0x0602 tp_reply

c2s · sealed · limit 9 · smallest 9 · core

The answer to a tp_asked.

| Field | Type | Meaning |
|---|---|---|
| `from` | `u64` | The asking account. |
| `accept` | `bool` | 1 accepts. |

#### 0x0603 tp_result

s2c · sealed · limit 9 · smallest 9 · core

How a teleport request ended, to the asker.

| Field | Type | Meaning |
|---|---|---|
| `target` | `u64` | The player asked. |
| `outcome` | `u8<=5` | `OW_TP_*`. |

#### 0x0604 go_to

s2c · sealed · limit 25 · smallest 25 · core

Move there: an accepted request or a summon.

| Field | Type | Meaning |
|---|---|---|
| `target` | `u64` | The player to go to. |
| `scene` | `u32` | Their scene hash. |
| `pos` | `vec3` | Their position. |
| `cause` | `u8<=1` | `OW_GO_*`. |

#### 0x0605 summon

c2s · sealed · limit 9 · smallest 9 · core

Host only: bring players to the host without asking.

| Field | Type | Meaning |
|---|---|---|
| `scope` | `u8<=2` | `OW_SUMMON_*`. |
| `target` | `u64` | The player for one, the team id for a team, 0 for all. |

#### 0x0680 mod_action

c2s · sealed · limit 136 · smallest 16 · core

A host tool.

| Field | Type | Meaning |
|---|---|---|
| `action` | `u8<=5` | `OW_MOD_*`. |
| `target` | `u64` | The account it acts on. |
| `rank` | `u8<=2` | The new rank for set rank; 0 otherwise. |
| `minutes` | `u32` | How long a ban or mute lasts, 0 for until lifted. |
| `reason` | `str(120)` | Shown to the target and kept in the server's log. |

#### 0x0681 mod_done

s2c · sealed · limit 10 · smallest 10 · core

The result of a mod_action, to the one who sent it.

| Field | Type | Meaning |
|---|---|---|
| `action` | `u8<=5` | The action. |
| `target` | `u64` | The account. |
| `outcome` | `u8<=3` | `OW_MOD_DONE`, `OW_MOD_NOT_ALLOWED`, `OW_MOD_UNKNOWN` or `OW_MOD_ALREADY`. |

#### 0x0682 announce

c2s · sealed · limit 482 · smallest 2 · core

Host or moderator: a server message to everyone, shown as a notice of kind announce.

| Field | Type | Meaning |
|---|---|---|
| `text` | `str(480)` | The message. |

### Other add-ons

#### 0x0700 duel_ask

c2s · sealed · limit 12 · smallest 12 · duels

Ask another player for a duel.

| Field | Type | Meaning |
|---|---|---|
| `target` | `u64` | Their account. |
| `stake` | `u32` | Rupees at stake, 0 for none. |

#### 0x0701 duel_asked

s2c · sealed · limit 46 · smallest 14 · duels

Someone asks this player for a duel (the toast).

| Field | Type | Meaning |
|---|---|---|
| `from` | `u64` | The asking account. |
| `nickname` | `str(32)` | Their name. |
| `stake` | `u32` | Rupees at stake. |

#### 0x0702 duel_reply

c2s · sealed · limit 9 · smallest 9 · duels

The answer to a duel_asked.

| Field | Type | Meaning |
|---|---|---|
| `from` | `u64` | The asking account. |
| `accept` | `bool` | 1 accepts. |

#### 0x0703 duel

s2c · sealed · limit 54 · smallest 54 · duels

A duel's state, to both players.

| Field | Type | Meaning |
|---|---|---|
| `duel` | `u32` | Duel id. |
| `a` | `u64` | One player. |
| `b` | `u64` | The other. |
| `phase` | `u8<=3` | `OW_DUEL_*`. |
| `ring` | `vec3` | The ring's centre. |
| `radius` | `f32` | The ring's radius in metres. |
| `countdown_ms` | `u32` | Time left of the count-down. |
| `stake` | `u32` | Rupees held by the server. |
| `winner` | `u64` | The winning account once ended, else 0. |
| `cause` | `u8<=6` | `OW_DUEL_END_*`. |

#### 0x0710 wallet_ask

c2s · sealed · limit 9 · smallest 9 · economy

A change of rupees, asked of the server.

| Field | Type | Meaning |
|---|---|---|
| `seq` | `u32` | Counter of the client's asks. |
| `delta` | `i32` | The change. |
| `cause` | `u8<=3` | `OW_WALLET_*`. |

#### 0x0711 wallet

s2c · sealed · limit 12 · smallest 12 · economy

The account's rupees after an ask.

| Field | Type | Meaning |
|---|---|---|
| `seq` | `u32` | The ask it answers, 0 for a correction. |
| `rupees` | `u32` | The account's rupees. |
| `granted` | `i32` | The change the server applied. |

#### 0x0712 ledger_note

c2s · sealed · limit 7 · smallest 7 · economy

A Light of Blessing or Korok seed found or spent.

| Field | Type | Meaning |
|---|---|---|
| `what` | `u8<=3` | `OW_LEDGER_*`. |
| `key` | `u32` | Which one (its GameData key hash) for a find; 0 for a spend. |
| `count` | `u16` | How many. |

#### 0x0713 ledger

s2c · sealed · limit 16 · smallest 16 · economy

The account's ledger, for correcting the pouch.

| Field | Type | Meaning |
|---|---|---|
| `lights_found` | `u32` | Found by anyone. |
| `lights_spent` | `u32` | Spent by this account. |
| `seeds_found` | `u32` | Found by anyone. |
| `seeds_spent` | `u32` | Spent by this account. |

#### 0x0720 companion

c2s · sealed · limit 18 · smallest 18 · companions

A companion is out or put away.

| Field | Type | Meaning |
|---|---|---|
| `id` | `aid` | The companion actor. |
| `what` | `u8<=2` | `OW_COMPANION_*`. |
| `out` | `bool` | 1 out, 0 stabled or gone. |

#### 0x0730 sky

c2s · sealed · limit 9 · smallest 9 · sky

The Blood Moon leader's clock.

| Field | Type | Meaning |
|---|---|---|
| `info` | `sky_info` | Day and time. |

#### 0x0731 sky_set

s2c · sealed · limit 17 · smallest 17 · sky

The leader's clock for every other game.

| Field | Type | Meaning |
|---|---|---|
| `leader` | `u64` | The leading account. |
| `info` | `sky_info` | Day and time. |

#### 0x0740 emote

c2s · sealed · limit 66 · smallest 2 · emotes

The player plays an emote.

| Field | Type | Meaning |
|---|---|---|
| `anim` | `str(64)` | The game's animation name. |

#### 0x0741 emoted

s2c · sealed · limit 74 · smallest 10 · emotes

Another player plays an emote.

| Field | Type | Meaning |
|---|---|---|
| `account` | `u64` | Whose. |
| `anim` | `str(64)` | The game's animation name. |

## 13. Structs

Used inline (a type named in a Type column) or in lists. Sizes are the smallest and largest encoding.

#### struct actor_pose

size 74 to 2146

One actor's state.

| Field | Type | Meaning |
|---|---|---|
| `id` | `aid` | The actor. |
| `epoch` | `u32` | The owner epoch it was sent under. |
| `body` | `body` | Pose and velocity. |
| `flags` | `u8<=7` | `OW_POSE_*` bits. |
| `comps` | `comps(8)` | Its components. |

#### struct aid

size 16 to 16

An actor's id.

| Field | Type | Meaning |
|---|---|---|
| `origin` | `u64` | 0 for a placed actor; the creating account for a run-time one. |
| `serial` | `u64` | The map instance id of a placed actor; the creator's counter for a run-time one. |

#### struct anim_slot

size 13 to 13

One animation layer.

| Field | Type | Meaning |
|---|---|---|
| `layer` | `u8` | Layer number. |
| `anim` | `u32` | Hash of the animation name. |
| `frame` | `f32` | Frame. |
| `rate` | `f32` | Play rate. |

#### struct body

size 52 to 52

Kinematic state.

| Field | Type | Meaning |
|---|---|---|
| `pos` | `vec3` | Position, metres. |
| `vel` | `vec3` | Velocity, m/s. |
| `rot` | `quat` | Rotation quaternion x, y, z, w. |
| `spin` | `vec3` | Angular velocity, rad/s. |

#### struct gd_entry

size 9 to 265

One GameData value.

| Field | Type | Meaning |
|---|---|---|
| `key` | `u32` | The GameData key hash. |
| `index` | `u16` | Array index, `0xFFFF` for a scalar. |
| `kind` | `u8<=8` | `OW_GD_*`: how to read value. |
| `value` | `blob(256)` | The value's bytes, little-endian. |

#### struct herd_row

size 36 to 36

One id-less animal.

| Field | Type | Meaning |
|---|---|---|
| `local` | `u32` | The leader's number for it. |
| `name_hash` | `u32` | Hash of its actor name. |
| `pos` | `vec3` | Position. |
| `vel` | `vec3` | Velocity. |
| `yaw` | `f32` | Heading, radians. |

#### struct hit_info

size 67 to 1103

A hit.

| Field | Type | Meaning |
|---|---|---|
| `target` | `aid` | The actor hit. |
| `epoch` | `u32` | Its owner epoch as the hitter saw it. |
| `source` | `u8<=5` | `OW_HIT_*`. |
| `element` | `u8<=7` | `OW_ELEMENT_*`. |
| `amount` | `i32` | Damage as the game counts it. |
| `at` | `vec3` | Where it landed. |
| `dir` | `vec3` | Its direction. |
| `by` | `aid` | The weapon or projectile actor, zero for none. |
| `comps` | `comps(4)` | Extra detail (fused parts, knock-back). |

#### struct place

size 9 to 9

Where a game is.

| Field | Type | Meaning |
|---|---|---|
| `scene` | `u32` | Scene hash (the field, the sky, the depths, an interior). |
| `cell_x` | `i16` | Map section column. |
| `cell_y` | `i16` | Map section row. |
| `state` | `u8<=4` | `OW_PLACE_*`. |

#### struct room_rules

size 13 to 13

The room's rules.

| Field | Type | Meaning |
|---|---|---|
| `max_players` | `u8` | Most players at once. |
| `interest_m` | `f32` | Interest radius, metres (250). |
| `hold_ms` | `u32` | How long a leaving game's actors and account are held (30000). |
| `options` | `u32` | `OW_RULE_*` bits. |

#### struct roster_row

size 32 to 64

One player in the list.

| Field | Type | Meaning |
|---|---|---|
| `account` | `u64` | Account id. |
| `nickname` | `str(32)` | Name. |
| `character` | `u8<=1` | `OW_CHAR_*`. |
| `rank` | `u8<=2` | `OW_RANK_*`. |
| `afk` | `bool` | Away. |
| `muted` | `bool` | Muted by a moderator. |
| `team` | `u16` | Team id, 0 for none. |
| `area` | `place` | Where they are. |
| `hearts_q` | `u16` | Hearts in quarters. |
| `hearts_max_q` | `u16` | Containers in quarters. |
| `ping_ms` | `u16` | Round trip to the server. |
| `fps` | `u8` | Frame rate. |

#### struct sky_info

size 9 to 9

The world clock.

| Field | Type | Meaning |
|---|---|---|
| `day` | `u32` | Day counter. |
| `hour` | `f32` | Hour of day, 0 to 24. |
| `blood_moon_due` | `bool` | The next night is a Blood Moon. |

#### struct spawn_info

size 88 to 2224

A run-time actor.

| Field | Type | Meaning |
|---|---|---|
| `id` | `aid` | Its id (creator, counter). |
| `parent` | `aid` | What made it (the enemy that dropped it, the summoner), zero for none. |
| `class` | `u8<=9` | `OW_CLASS_*`. |
| `name` | `str(64)` | The game's actor name. |
| `body` | `body` | Its first state. |
| `comps` | `comps(8)` | Its first components. |

#### struct spot

size 28 to 28

A player's last position.

| Field | Type | Meaning |
|---|---|---|
| `account` | `u64` | Whose. |
| `scene` | `u32` | Scene hash. |
| `pos` | `vec3` | Position. |
| `yaw` | `f32` | Heading, radians. |

#### struct team_row

size 14 to 38

One team.

| Field | Type | Meaning |
|---|---|---|
| `id` | `u16` | Team id, never 0. |
| `name` | `str(24)` | Name. |
| `leader` | `u64` | The leading account. |
| `open` | `bool` | Anyone may join. |
| `members` | `u8` | How many members. |

## 14. Components

The body of each kind. A kind the reader does not know is skipped by its length; a body that does not decode as its kind is skipped the same way (section 5).

#### component 1 anim

body 2 to 106

Animation layers.

| Field | Type | Meaning |
|---|---|---|
| `slots` | `list(8, anim_slot)` | Up to 8 layers. |

#### component 2 life

body 8 to 8

Life.

| Field | Type | Meaning |
|---|---|---|
| `life` | `i32` | Life now. |
| `life_max` | `i32` | Most life. |

#### component 3 gear

body 25 to 25

What is equipped (hashes of actor names, 0 for none).

| Field | Type | Meaning |
|---|---|---|
| `weapon` | `u32` | Weapon. |
| `bow` | `u32` | Bow. |
| `shield` | `u32` | Shield. |
| `weapon_fuse` | `u32` | Material fused to the weapon. |
| `shield_fuse` | `u32` | Fused to the shield. |
| `arrow_fuse` | `u32` | Fused to the arrow. |
| `drawn` | `u8<=3` | `OW_DRAWN_*`. |

#### component 4 mind

body 12 to 12

Behaviour state of an enemy or townsperson.

| Field | Type | Meaning |
|---|---|---|
| `state` | `u32` | State hash. |
| `substate` | `u32` | Sub-state hash. |
| `timer` | `f32` | Time in the state, seconds. |

#### component 5 ride

body 17 to 17

Riding.

| Field | Type | Meaning |
|---|---|---|
| `mount` | `aid` | The mount's id. |
| `seat` | `u8` | Seat number. |

#### component 6 glide

body 5 to 5

The paraglider.

| Field | Type | Meaning |
|---|---|---|
| `on` | `bool` | Open. |
| `lean` | `f32` | Lean, -1 to 1. |

#### component 7 aim

body 13 to 13

Aiming a bow or a throw.

| Field | Type | Meaning |
|---|---|---|
| `on` | `bool` | Aiming. |
| `dir` | `vec3` | Aim direction. |

#### component 8 held

body 9 to 9

Who holds this actor.

| Field | Type | Meaning |
|---|---|---|
| `holder` | `u64` | The holding account. |
| `what` | `u8<=3` | `OW_HOLD_*`. |

#### component 9 scale

body 12 to 12

Scale, when not 1.

| Field | Type | Meaning |
|---|---|---|
| `scale` | `vec3` | Scale per axis. |
