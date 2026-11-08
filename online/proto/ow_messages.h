/* The wire's message and component tables: every message type, its number, direction, stage, limit, feature and
 * fields.
 *
 * Described in words in docs/online-wire.md. This file is the one source of truth: ow_wire.h turns it into C
 * structs, ow_codec.c into the descriptors the codec walks, and tests/net_proto_check.py compares it with the tables
 * of docs/online-wire.md.
 *
 * A field is F(T, KIND, name, max, sub):
 *   U8 U16 U32 U64 I16 I32  little-endian integers; for U8 and U16 a nonzero max is the largest value allowed
 *   BOOL                    one byte, 0 or 1
 *   F32 VEC3 QUAT           1, 3 or 4 IEEE-754 floats, little-endian, finite only
 *   B32 B64                 32 or 64 raw bytes (keys, nonces, signatures)
 *   STR                     u16 byte length, then UTF-8 without control characters; max is the byte limit
 *   BLOB                    u16 byte length, then bytes; max is the byte limit
 *   OBJ                     the fields of sub-struct `sub`, inline
 *   LIST                    u16 count, then `count` sub-structs `sub`; max is the count limit
 *   COMPS                   u8 count, then per component u8 kind, u16 length, bytes; max is the count limit
 * T is the C struct the field belongs to; it is passed along so the descriptor can take offsetof.
 *
 * Changing anything here (a field, its order, a limit, a number, a feature) changes the wire: raise OW_VERSION in
 * ow_wire.h and docs/online-wire.md together. The greet's first two fields and the whole refuse message are frozen
 * across versions.
 */
#ifndef OW_MESSAGES_H
#define OW_MESSAGES_H

/* Shared sub-structs. S(name) declares ow_<name> from OW_F_<name>. A struct used by OBJ or LIST is listed before the
 * structs that use it. */
#define OW_SUBSTRUCTS(S) \
    S(aid) S(gd_entry) S(body) S(place) S(room_rules) S(roster_row) S(spot) S(team_row) S(actor_pose) S(hit_info) \
    S(spawn_info) S(herd_row) S(sky_info) S(anim_slot)

/* An actor's id: origin 0 and serial = the game's map instance id for a placed actor; origin = the creating
 * account and serial = that account's counter for a run-time actor. */
#define OW_F_aid(F, T) F(T, U64, origin, 0, _) F(T, U64, serial, 0, _)
/* One GameData value: key hash, array index (0xFFFF for a scalar), value kind, value bytes. */
#define OW_F_gd_entry(F, T) \
    F(T, U32, key, 0, _) F(T, U16, index, 0, _) F(T, U8, kind, OW_GD_KIND_LAST, _) F(T, BLOB, value, 256, _)
/* Kinematic state: position (m), velocity (m/s), rotation quaternion (x, y, z, w), angular velocity (rad/s). */
#define OW_F_body(F, T) F(T, VEC3, pos, 0, _) F(T, VEC3, vel, 0, _) F(T, QUAT, rot, 0, _) F(T, VEC3, spin, 0, _)
/* Where a game is: scene hash, map section, and its state (OW_PLACE_*). */
#define OW_F_place(F, T) \
    F(T, U32, scene, 0, _) F(T, I16, cell_x, 0, _) F(T, I16, cell_y, 0, _) F(T, U8, state, OW_PLACE_LAST, _)
/* What the host set for the room. options holds OW_RULE_* bits. */
#define OW_F_room_rules(F, T) \
    F(T, U8, max_players, 0, _) F(T, F32, interest_m, 0, _) F(T, U32, hold_ms, 0, _) F(T, U32, options, 0, _)
#define OW_F_roster_row(F, T) \
    F(T, U64, account, 0, _) F(T, STR, nickname, OW_NICK_MAX, _) F(T, U8, character, OW_CHAR_LAST, _) \
    F(T, U8, rank, OW_RANK_LAST, _) F(T, BOOL, afk, 0, _) F(T, BOOL, muted, 0, _) F(T, U16, team, 0, _) \
    F(T, OBJ, area, 0, place) F(T, U16, hearts_q, 0, _) F(T, U16, hearts_max_q, 0, _) F(T, U16, ping_ms, 0, _) \
    F(T, U8, fps, 0, _)
/* A player's last reported position, for the map and for teleports. */
#define OW_F_spot(F, T) F(T, U64, account, 0, _) F(T, U32, scene, 0, _) F(T, VEC3, pos, 0, _) F(T, F32, yaw, 0, _)
#define OW_F_team_row(F, T) \
    F(T, U16, id, 0, _) F(T, STR, name, OW_TEAM_NAME_MAX, _) F(T, U64, leader, 0, _) F(T, BOOL, open, 0, _) \
    F(T, U8, members, 0, _)
/* One actor's state in a batch. flags holds OW_POSE_* bits. */
#define OW_F_actor_pose(F, T) \
    F(T, OBJ, id, 0, aid) F(T, U32, epoch, 0, _) F(T, OBJ, body, 0, body) F(T, U8, flags, OW_POSE_ALL, _) \
    F(T, COMPS, comps, OW_COMPS_MAX, _)
#define OW_F_hit_info(F, T) \
    F(T, OBJ, target, 0, aid) F(T, U32, epoch, 0, _) F(T, U8, source, OW_HIT_LAST, _) \
    F(T, U8, element, OW_ELEMENT_LAST, _) F(T, I32, amount, 0, _) F(T, VEC3, at, 0, _) F(T, VEC3, dir, 0, _) \
    F(T, OBJ, by, 0, aid) F(T, COMPS, comps, 4, _)
#define OW_F_spawn_info(F, T) \
    F(T, OBJ, id, 0, aid) F(T, OBJ, parent, 0, aid) F(T, U8, class_, OW_CLASS_LAST, _) \
    F(T, STR, name, OW_ACTOR_NAME_MAX, _) F(T, OBJ, body, 0, body) F(T, COMPS, comps, OW_COMPS_MAX, _)
#define OW_F_herd_row(F, T) \
    F(T, U32, local, 0, _) F(T, U32, name_hash, 0, _) F(T, VEC3, pos, 0, _) F(T, VEC3, vel, 0, _) \
    F(T, F32, yaw, 0, _)
#define OW_F_sky_info(F, T) F(T, U32, day, 0, _) F(T, F32, hour, 0, _) F(T, BOOL, blood_moon_due, 0, _)
#define OW_F_anim_slot(F, T) F(T, U8, layer, 0, _) F(T, U32, anim, 0, _) F(T, F32, frame, 0, _) F(T, F32, rate, 0, _)

/* Messages. M(number, name, direction, stage, cap, feature). cap is the payload limit in bytes; 0 means the largest
 * encoding the fields allow. A nonzero cap below that bound makes the encoder refuse a message that would not fit.
 * feature is the OW_FEAT_* bit a link must have granted before the type may travel on it; CORE types always may. */
#define OW_MESSAGES(M) \
    /* handshake and session */ \
    M(0x0001, greet,        C2S,  PLAIN,  1024,  CORE) \
    M(0x0002, refuse,       S2C,  ANY,    1024,  CORE) \
    M(0x0003, challenge,    S2C,  PLAIN,  0,     CORE) \
    M(0x0004, proof,        C2S,  SEALED, 0,     CORE) \
    M(0x0005, admit,        S2C,  SEALED, 0,     CORE) \
    M(0x0006, bye,          BOTH, SEALED, 0,     CORE) \
    M(0x0007, join_ready,   S2C,  SEALED, 0,     CORE) \
    M(0x0010, ping,         BOTH, SEALED, 0,     CORE) \
    M(0x0011, pong,         BOTH, SEALED, 0,     CORE) \
    /* saves and world state */ \
    M(0x0100, save_part,    S2C,  SEALED, 0,     SAVES) \
    M(0x0101, world_part,   S2C,  SEALED, 0,     SAVES) \
    M(0x0103, save_write,   C2S,  SEALED, 0,     SAVES) \
    M(0x0104, world_write,  C2S,  SEALED, 0,     SAVES) \
    M(0x0105, world_set,    S2C,  SEALED, 0,     SAVES) \
    M(0x0106, save_fix,     S2C,  SEALED, 0,     SAVES) \
    /* presence */ \
    M(0x0200, area,         C2S,  SEALED, 0,     CORE) \
    M(0x0201, vitals,       C2S,  SEALED, 0,     CORE) \
    M(0x0202, roster,       S2C,  SEALED, 0,     CORE) \
    M(0x0203, arrive,       S2C,  SEALED, 0,     CORE) \
    M(0x0204, depart,       S2C,  SEALED, 0,     CORE) \
    M(0x0205, position,     C2S,  SEALED, 0,     CORE) \
    M(0x0206, positions,    S2C,  SEALED, 0,     CORE) \
    M(0x0280, self_state,   C2S,  SEALED, 0,     PUPPETS) \
    M(0x0281, player_state, S2C,  SEALED, 0,     PUPPETS) \
    /* teams */ \
    M(0x0300, team_create,  C2S,  SEALED, 0,     CORE) \
    M(0x0301, team_invite,  C2S,  SEALED, 0,     CORE) \
    M(0x0302, team_invited, S2C,  SEALED, 0,     CORE) \
    M(0x0303, team_join,    C2S,  SEALED, 0,     CORE) \
    M(0x0304, team_leave,   C2S,  SEALED, 0,     CORE) \
    M(0x0305, team_kick,    C2S,  SEALED, 0,     CORE) \
    M(0x0306, teams,        S2C,  SEALED, 0,     CORE) \
    /* actors */ \
    M(0x0400, offer,        C2S,  SEALED, 0,     ACTORS) \
    M(0x0401, release,      C2S,  SEALED, 0,     ACTORS) \
    M(0x0402, actor_send,   C2S,  SEALED, 60000, ACTORS) \
    M(0x0403, actor_recv,   S2C,  SEALED, 60000, ACTORS) \
    M(0x0404, owner,        S2C,  SEALED, 0,     ACTORS) \
    M(0x0405, hold,         C2S,  SEALED, 0,     ACTORS) \
    M(0x0406, hit,          C2S,  SEALED, 0,     ACTORS) \
    M(0x0407, hit_in,       S2C,  SEALED, 0,     ACTORS) \
    M(0x0408, spawn,        C2S,  SEALED, 0,     ACTORS) \
    M(0x0409, spawned,      S2C,  SEALED, 0,     ACTORS) \
    M(0x040A, gone,         BOTH, SEALED, 0,     ACTORS) \
    M(0x040B, group,        BOTH, SEALED, 0,     ACTORS) \
    M(0x040C, herd,         BOTH, SEALED, 0,     ACTORS) \
    M(0x040D, take,         C2S,  SEALED, 0,     ACTORS) \
    M(0x040E, taken,        S2C,  SEALED, 0,     ACTORS) \
    /* chat and interface */ \
    M(0x0500, say,          C2S,  SEALED, 0,     CORE) \
    M(0x0501, chat,         S2C,  SEALED, 0,     CORE) \
    M(0x0502, notice,       S2C,  SEALED, 0,     CORE) \
    M(0x0503, mark,         C2S,  SEALED, 0,     CORE) \
    M(0x0504, marker,       S2C,  SEALED, 0,     CORE) \
    M(0x0505, character,    C2S,  SEALED, 0,     CORE) \
    M(0x0506, rules,        S2C,  SEALED, 0,     CORE) \
    /* teleport */ \
    M(0x0600, tp_ask,       C2S,  SEALED, 0,     CORE) \
    M(0x0601, tp_asked,     S2C,  SEALED, 0,     CORE) \
    M(0x0602, tp_reply,     C2S,  SEALED, 0,     CORE) \
    M(0x0603, tp_result,    S2C,  SEALED, 0,     CORE) \
    M(0x0604, go_to,        S2C,  SEALED, 0,     CORE) \
    M(0x0605, summon,       C2S,  SEALED, 0,     CORE) \
    /* host tools */ \
    M(0x0680, mod_action,   C2S,  SEALED, 0,     CORE) \
    M(0x0681, mod_done,     S2C,  SEALED, 0,     CORE) \
    M(0x0682, announce,     C2S,  SEALED, 0,     CORE) \
    /* add-ons */ \
    M(0x0700, duel_ask,     C2S,  SEALED, 0,     DUELS) \
    M(0x0701, duel_asked,   S2C,  SEALED, 0,     DUELS) \
    M(0x0702, duel_reply,   C2S,  SEALED, 0,     DUELS) \
    M(0x0703, duel,         S2C,  SEALED, 0,     DUELS) \
    M(0x0710, wallet_ask,   C2S,  SEALED, 0,     ECONOMY) \
    M(0x0711, wallet,       S2C,  SEALED, 0,     ECONOMY) \
    M(0x0712, ledger_note,  C2S,  SEALED, 0,     ECONOMY) \
    M(0x0713, ledger,       S2C,  SEALED, 0,     ECONOMY) \
    M(0x0720, companion,    C2S,  SEALED, 0,     COMPANIONS) \
    M(0x0730, sky,          C2S,  SEALED, 0,     SKY) \
    M(0x0731, sky_set,      S2C,  SEALED, 0,     SKY) \
    M(0x0740, emote,        C2S,  SEALED, 0,     EMOTES) \
    M(0x0741, emoted,       S2C,  SEALED, 0,     EMOTES)

/* Handshake and session. greet's magic and version, and refuse as a whole, never change (docs/online-wire.md,
 * "Versions"). */
#define OW_F_greet(F, T) \
    F(T, U32, magic, 0, _) F(T, U16, version, 0, _) F(T, STR, build, OW_BUILD_MAX, _) F(T, B32, nonce, 0, _) \
    F(T, B32, exchange, 0, _)
#define OW_F_refuse(F, T) \
    F(T, U16, reason, 0, _) F(T, U16, version, 0, _) F(T, STR, build, OW_BUILD_MAX, _) \
    F(T, STR, detail, OW_DETAIL_MAX, _)
#define OW_F_challenge(F, T) \
    F(T, B32, nonce, 0, _) F(T, B32, identity, 0, _) F(T, B32, exchange, 0, _) F(T, B64, signature, 0, _)
#define OW_F_proof(F, T) \
    F(T, B32, identity, 0, _) F(T, B64, signature, 0, _) F(T, STR, nickname, OW_NICK_MAX, _) \
    F(T, U8, character, OW_CHAR_LAST, _) F(T, STR, claim, OW_CLAIM_MAX, _) F(T, U32, features, 0, _)
#define OW_F_admit(F, T) \
    F(T, U64, account, 0, _) F(T, STR, nickname, OW_NICK_MAX, _) F(T, U8, rank, OW_RANK_LAST, _) \
    F(T, BOOL, resumed, 0, _) F(T, U32, features, 0, _) F(T, U64, server_time_us, 0, _) \
    F(T, OBJ, rules, 0, room_rules)
#define OW_F_bye(F, T) F(T, U16, reason, 0, _) F(T, STR, detail, OW_DETAIL_MAX, _)
#define OW_F_join_ready(F, T) F(T, U16, save_parts, 0, _) F(T, U16, world_parts, 0, _) F(T, U16, players, 0, _)
#define OW_F_ping(F, T) F(T, U32, seq, 0, _) F(T, U64, sent_us, 0, _)
#define OW_F_pong(F, T) F(T, U32, seq, 0, _) F(T, U64, echo_us, 0, _) F(T, U64, reply_us, 0, _)

/* Saves and world state */
#define OW_F_save_part(F, T) \
    F(T, U16, part, 0, _) F(T, U16, parts, 0, _) F(T, LIST, entries, OW_GD_BATCH, gd_entry)
#define OW_F_world_part(F, T) OW_F_save_part(F, T)
#define OW_F_save_write(F, T) F(T, U32, seq, 0, _) F(T, LIST, entries, OW_GD_BATCH, gd_entry)
#define OW_F_world_write(F, T) OW_F_save_write(F, T)
#define OW_F_world_set(F, T) F(T, U64, source, 0, _) F(T, LIST, entries, OW_GD_BATCH, gd_entry)
#define OW_F_save_fix(F, T) F(T, U32, seq, 0, _) F(T, LIST, entries, OW_GD_BATCH, gd_entry)

/* Presence */
#define OW_F_area(F, T) F(T, OBJ, where, 0, place)
#define OW_F_vitals(F, T) F(T, U16, hearts_q, 0, _) F(T, U16, hearts_max_q, 0, _) F(T, U8, fps, 0, _)
#define OW_F_roster(F, T) F(T, LIST, players, OW_PLAYERS_MAX, roster_row)
#define OW_F_arrive(F, T) \
    F(T, U64, account, 0, _) F(T, STR, nickname, OW_NICK_MAX, _) F(T, U8, character, OW_CHAR_LAST, _)
#define OW_F_depart(F, T) F(T, U64, account, 0, _) F(T, U16, reason, 0, _)
#define OW_F_position(F, T) F(T, U32, scene, 0, _) F(T, VEC3, pos, 0, _) F(T, F32, yaw, 0, _)
#define OW_F_positions(F, T) F(T, LIST, spots, OW_PLAYERS_MAX, spot)
#define OW_F_self_state(F, T) \
    F(T, U64, time_us, 0, _) F(T, OBJ, body, 0, body) F(T, COMPS, comps, OW_COMPS_MAX, _)
#define OW_F_player_state(F, T) \
    F(T, U64, account, 0, _) F(T, U64, time_us, 0, _) F(T, OBJ, body, 0, body) F(T, COMPS, comps, OW_COMPS_MAX, _)

/* Teams */
#define OW_F_team_create(F, T) F(T, STR, name, OW_TEAM_NAME_MAX, _) F(T, BOOL, open, 0, _)
#define OW_F_team_invite(F, T) F(T, U64, target, 0, _)
#define OW_F_team_invited(F, T) \
    F(T, U16, team, 0, _) F(T, STR, name, OW_TEAM_NAME_MAX, _) F(T, U64, from, 0, _) \
    F(T, STR, nickname, OW_NICK_MAX, _)
#define OW_F_team_join(F, T) F(T, U16, team, 0, _)
#define OW_F_team_leave(F, T) F(T, U16, team, 0, _)
#define OW_F_team_kick(F, T) F(T, U64, target, 0, _)
#define OW_F_teams(F, T) F(T, LIST, list, OW_TEAMS_MAX, team_row)

/* Actors */
#define OW_F_offer(F, T) \
    F(T, OBJ, id, 0, aid) F(T, U8, class_, OW_CLASS_LAST, _) F(T, STR, name, OW_ACTOR_NAME_MAX, _) \
    F(T, VEC3, pos, 0, _)
#define OW_F_release(F, T) F(T, OBJ, id, 0, aid) F(T, U32, epoch, 0, _)
#define OW_F_actor_send(F, T) F(T, U64, time_us, 0, _) F(T, LIST, items, OW_POSE_BATCH, actor_pose)
#define OW_F_actor_recv(F, T) \
    F(T, U64, source, 0, _) F(T, U64, time_us, 0, _) F(T, LIST, items, OW_POSE_BATCH, actor_pose)
#define OW_F_owner(F, T) \
    F(T, OBJ, id, 0, aid) F(T, U8, class_, OW_CLASS_LAST, _) F(T, STR, name, OW_ACTOR_NAME_MAX, _) \
    F(T, U32, epoch, 0, _) F(T, U64, owner, 0, _) F(T, U8, cause, OW_OWNER_LAST, _) F(T, U32, hold_ms, 0, _) \
    F(T, U64, state_time_us, 0, _) F(T, OBJ, body, 0, body) F(T, U8, flags, OW_POSE_ALL, _) \
    F(T, COMPS, comps, OW_COMPS_MAX, _)
#define OW_F_hold(F, T) F(T, OBJ, id, 0, aid) F(T, U8, what, OW_HOLD_LAST, _) F(T, BOOL, on, 0, _)
#define OW_F_hit(F, T) F(T, OBJ, info, 0, hit_info)
#define OW_F_hit_in(F, T) F(T, U64, attacker, 0, _) F(T, OBJ, info, 0, hit_info)
#define OW_F_spawn(F, T) F(T, OBJ, info, 0, spawn_info)
#define OW_F_spawned(F, T) F(T, U64, creator, 0, _) F(T, OBJ, info, 0, spawn_info)
#define OW_F_gone(F, T) F(T, OBJ, id, 0, aid) F(T, U32, epoch, 0, _) F(T, U8, cause, OW_GONE_LAST, _)
#define OW_F_group(F, T) F(T, OBJ, id, 0, aid) F(T, LIST, members, OW_GROUP_MAX, aid)
#define OW_F_herd(F, T) F(T, U32, cell, 0, _) F(T, U64, leader, 0, _) F(T, LIST, beasts, OW_HERD_MAX, herd_row)
#define OW_F_take(F, T) F(T, OBJ, id, 0, aid) F(T, U32, epoch, 0, _)
#define OW_F_taken(F, T) F(T, OBJ, id, 0, aid) F(T, U64, taker, 0, _) F(T, BOOL, yours, 0, _)

/* Chat and interface */
#define OW_F_say(F, T) F(T, U8, kind, OW_SAY_LAST, _) F(T, U64, to, 0, _) F(T, STR, text, OW_TEXT_MAX, _)
#define OW_F_chat(F, T) \
    F(T, U64, from, 0, _) F(T, U64, to, 0, _) F(T, U8, kind, OW_CHAT_LAST, _) F(T, STR, nickname, OW_NICK_MAX, _) \
    F(T, STR, text, OW_TEXT_MAX, _)
#define OW_F_notice(F, T) F(T, U8, kind, OW_NOTICE_LAST, _) F(T, U32, ttl_ms, 0, _) F(T, STR, text, OW_NOTICE_MAX, _)
#define OW_F_mark(F, T) \
    F(T, U8, slot, OW_MARK_LAST, _) F(T, BOOL, on, 0, _) F(T, U32, scene, 0, _) F(T, VEC3, pos, 0, _)
#define OW_F_marker(F, T) \
    F(T, U64, account, 0, _) F(T, U8, slot, OW_MARK_LAST, _) F(T, BOOL, on, 0, _) F(T, U32, scene, 0, _) \
    F(T, VEC3, pos, 0, _)
#define OW_F_character(F, T) F(T, U8, character, OW_CHAR_LAST, _)
#define OW_F_rules(F, T) F(T, OBJ, rules, 0, room_rules)

/* Teleport */
#define OW_F_tp_ask(F, T) F(T, U64, target, 0, _)
#define OW_F_tp_asked(F, T) F(T, U64, from, 0, _) F(T, STR, nickname, OW_NICK_MAX, _)
#define OW_F_tp_reply(F, T) F(T, U64, from, 0, _) F(T, BOOL, accept, 0, _)
#define OW_F_tp_result(F, T) F(T, U64, target, 0, _) F(T, U8, outcome, OW_TP_LAST, _)
#define OW_F_go_to(F, T) \
    F(T, U64, target, 0, _) F(T, U32, scene, 0, _) F(T, VEC3, pos, 0, _) F(T, U8, cause, OW_GO_LAST, _)
#define OW_F_summon(F, T) F(T, U8, scope, OW_SUMMON_LAST, _) F(T, U64, target, 0, _)

/* Host tools */
#define OW_F_mod_action(F, T) \
    F(T, U8, action, OW_MOD_LAST, _) F(T, U64, target, 0, _) F(T, U8, rank, OW_RANK_LAST, _) \
    F(T, U32, minutes, 0, _) F(T, STR, reason, OW_REASON_MAX, _)
#define OW_F_mod_done(F, T) \
    F(T, U8, action, OW_MOD_LAST, _) F(T, U64, target, 0, _) F(T, U8, outcome, OW_MOD_OUTCOME_LAST, _)
#define OW_F_announce(F, T) F(T, STR, text, OW_NOTICE_MAX, _)

/* Add-ons */
#define OW_F_duel_ask(F, T) F(T, U64, target, 0, _) F(T, U32, stake, 0, _)
#define OW_F_duel_asked(F, T) F(T, U64, from, 0, _) F(T, STR, nickname, OW_NICK_MAX, _) F(T, U32, stake, 0, _)
#define OW_F_duel_reply(F, T) F(T, U64, from, 0, _) F(T, BOOL, accept, 0, _)
#define OW_F_duel(F, T) \
    F(T, U32, duel, 0, _) F(T, U64, a, 0, _) F(T, U64, b, 0, _) F(T, U8, phase, OW_DUEL_LAST, _) \
    F(T, VEC3, ring, 0, _) F(T, F32, radius, 0, _) F(T, U32, countdown_ms, 0, _) F(T, U32, stake, 0, _) \
    F(T, U64, winner, 0, _) F(T, U8, cause, OW_DUEL_END_LAST, _)
#define OW_F_wallet_ask(F, T) F(T, U32, seq, 0, _) F(T, I32, delta, 0, _) F(T, U8, cause, OW_WALLET_LAST, _)
#define OW_F_wallet(F, T) F(T, U32, seq, 0, _) F(T, U32, rupees, 0, _) F(T, I32, granted, 0, _)
#define OW_F_ledger_note(F, T) F(T, U8, what, OW_LEDGER_LAST, _) F(T, U32, key, 0, _) F(T, U16, count, 0, _)
#define OW_F_ledger(F, T) \
    F(T, U32, lights_found, 0, _) F(T, U32, lights_spent, 0, _) F(T, U32, seeds_found, 0, _) \
    F(T, U32, seeds_spent, 0, _)
#define OW_F_companion(F, T) F(T, OBJ, id, 0, aid) F(T, U8, what, OW_COMPANION_LAST, _) F(T, BOOL, out, 0, _)
#define OW_F_sky(F, T) F(T, OBJ, info, 0, sky_info)
#define OW_F_sky_set(F, T) F(T, U64, leader, 0, _) F(T, OBJ, info, 0, sky_info)
#define OW_F_emote(F, T) F(T, STR, anim, OW_ANIM_NAME_MAX, _)
#define OW_F_emoted(F, T) F(T, U64, account, 0, _) F(T, STR, anim, OW_ANIM_NAME_MAX, _)

/* Components: C(kind number, name). Each body is one struct, OW_F_c_<name>, at most OW_COMP_BODY_MAX bytes. */
#define OW_COMPONENTS(C) \
    C(1, anim) C(2, life) C(3, gear) C(4, mind) C(5, ride) C(6, glide) C(7, aim) C(8, held) C(9, scale)

#define OW_F_c_anim(F, T) F(T, LIST, slots, 8, anim_slot)
#define OW_F_c_life(F, T) F(T, I32, life, 0, _) F(T, I32, life_max, 0, _)
#define OW_F_c_gear(F, T) \
    F(T, U32, weapon, 0, _) F(T, U32, bow, 0, _) F(T, U32, shield, 0, _) F(T, U32, weapon_fuse, 0, _) \
    F(T, U32, shield_fuse, 0, _) F(T, U32, arrow_fuse, 0, _) F(T, U8, drawn, OW_DRAWN_LAST, _)
#define OW_F_c_mind(F, T) F(T, U32, state, 0, _) F(T, U32, substate, 0, _) F(T, F32, timer, 0, _)
#define OW_F_c_ride(F, T) F(T, OBJ, mount, 0, aid) F(T, U8, seat, 0, _)
#define OW_F_c_glide(F, T) F(T, BOOL, on, 0, _) F(T, F32, lean, 0, _)
#define OW_F_c_aim(F, T) F(T, BOOL, on, 0, _) F(T, VEC3, dir, 0, _)
#define OW_F_c_held(F, T) F(T, U64, holder, 0, _) F(T, U8, what, OW_HOLD_LAST, _)
#define OW_F_c_scale(F, T) F(T, VEC3, scale, 0, _)

#endif
