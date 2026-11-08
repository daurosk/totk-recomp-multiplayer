/* The online wire: constants, message structs, the codec, framing, sealing and the handshake.
 *
 * Described in docs/online-wire.md. The same files compile into the game's client, the server and the offline
 * tests, so both ends share every layout and limit. Messages and components are defined once, in ow_messages.h.
 */
#ifndef OW_WIRE_H
#define OW_WIRE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Protocol version. A client of version N talks only to a server of version N (docs/online-wire.md, "Versions"). */
#define OW_VERSION 1
/* greet.magic: the bytes "TKRW" read as a little-endian u32. */
#define OW_MAGIC 0x57524B54u

/* Frame layout: u32 length, u16 type, then `length` bytes (the payload, followed by a 16-byte tag once sealed). */
#define OW_HEADER 6
#define OW_TAG 16
#define OW_PAYLOAD_MAX 65000
#define OW_FRAME_MAX (OW_HEADER + OW_PAYLOAD_MAX + OW_TAG)

/* Field limits named by the tables. */
#define OW_BUILD_MAX 64
#define OW_DETAIL_MAX 240
#define OW_NICK_MAX 32
#define OW_CLAIM_MAX 32
#define OW_TEXT_MAX 256
#define OW_NOTICE_MAX 480
#define OW_ACTOR_NAME_MAX 64
#define OW_ANIM_NAME_MAX 64
#define OW_GD_BATCH 200
#define OW_PLAYERS_MAX 32
#define OW_POSE_BATCH 48
#define OW_GROUP_MAX 64
#define OW_HERD_MAX 64
#define OW_COMPS_MAX 8
#define OW_COMP_BODY_MAX 256
#define OW_TEAM_NAME_MAX 24
#define OW_TEAMS_MAX 16
#define OW_REASON_MAX 120

/* Features: add-ons a client offers in proof.features and the server grants in admit.features. A type of a feature
 * travels only on a link that has it (ow_link_set_features). Bits never change meaning; new add-ons take new bits. */
enum { OW_FEAT_CORE = 0, OW_FEAT_PUPPETS = 1, OW_FEAT_SAVES = 2, OW_FEAT_ACTORS = 4, OW_FEAT_DUELS = 8,
       OW_FEAT_ECONOMY = 16, OW_FEAT_COMPANIONS = 32, OW_FEAT_SKY = 64, OW_FEAT_EMOTES = 128, OW_FEAT_ALL = 255 };

/* Value sets. Each *_LAST is the largest value a field of that set may carry. */
enum { OW_GD_BOOL, OW_GD_S32, OW_GD_F32, OW_GD_STR, OW_GD_VEC3, OW_GD_U32, OW_GD_S64, OW_GD_U64, OW_GD_BIN,
       OW_GD_KIND_LAST = OW_GD_BIN };
enum { OW_PLACE_TITLE, OW_PLACE_PLAYING, OW_PLACE_LOADING, OW_PLACE_PAUSED, OW_PLACE_DEAD,
       OW_PLACE_LAST = OW_PLACE_DEAD };
enum { OW_CHAR_LINK, OW_CHAR_ZELDA, OW_CHAR_LAST = OW_CHAR_ZELDA };
enum { OW_RANK_PLAYER, OW_RANK_MODERATOR, OW_RANK_HOST, OW_RANK_LAST = OW_RANK_HOST };
enum { OW_POSE_STILL = 1, OW_POSE_ASLEEP = 2, OW_POSE_HELD = 4, OW_POSE_ALL = 7 };
enum { OW_RULE_SHARED_DROPS = 1, OW_RULE_TELEPORT = 2, OW_RULE_PLAYER_HITS = 4 };
enum { OW_HIT_WEAPON, OW_HIT_ARROW, OW_HIT_BOMB, OW_HIT_THROWN, OW_HIT_ELEMENT, OW_HIT_OTHER,
       OW_HIT_LAST = OW_HIT_OTHER };
enum { OW_ELEMENT_NONE, OW_ELEMENT_FIRE, OW_ELEMENT_ICE, OW_ELEMENT_SHOCK, OW_ELEMENT_WATER, OW_ELEMENT_WIND,
       OW_ELEMENT_LIGHT, OW_ELEMENT_GLOOM, OW_ELEMENT_LAST = OW_ELEMENT_GLOOM };
enum { OW_CLASS_MAP_OBJECT, OW_CLASS_WORLD_OBJECT, OW_CLASS_ENEMY, OW_CLASS_BOSS, OW_CLASS_ANIMAL,
       OW_CLASS_TOWNSPERSON, OW_CLASS_ITEM, OW_CLASS_PROJECTILE, OW_CLASS_PUPPET, OW_CLASS_DEVICE,
       OW_CLASS_LAST = OW_CLASS_DEVICE };
enum { OW_OWNER_OFFER, OW_OWNER_FAR, OW_OWNER_CLOSER, OW_OWNER_PICKUP, OW_OWNER_LEFT, OW_OWNER_HOLD,
       OW_OWNER_HOST, OW_OWNER_LAST = OW_OWNER_HOST };
enum { OW_HOLD_NONE, OW_HOLD_ULTRAHAND, OW_HOLD_CARRY, OW_HOLD_RIDE, OW_HOLD_LAST = OW_HOLD_RIDE };
enum { OW_GONE_REMOVED, OW_GONE_KILLED, OW_GONE_TAKEN, OW_GONE_UNLOADED, OW_GONE_LAST = OW_GONE_UNLOADED };
enum { OW_SAY_ALL, OW_SAY_TEAM, OW_SAY_WHISPER, OW_SAY_ME, OW_SAY_LAST = OW_SAY_ME };
enum { OW_CHAT_ALL, OW_CHAT_TEAM, OW_CHAT_WHISPER, OW_CHAT_ME, OW_CHAT_SYSTEM, OW_CHAT_LAST = OW_CHAT_SYSTEM };
enum { OW_NOTICE_MOTD, OW_NOTICE_TOAST, OW_NOTICE_WARNING, OW_NOTICE_ANNOUNCE, OW_NOTICE_LAST = OW_NOTICE_ANNOUNCE };
enum { OW_TP_ACCEPTED, OW_TP_REFUSED, OW_TP_TIMEOUT, OW_TP_NOT_ALLOWED, OW_TP_GONE, OW_TP_BUSY,
       OW_TP_LAST = OW_TP_BUSY };
enum { OW_GO_REQUEST, OW_GO_SUMMON, OW_GO_LAST = OW_GO_SUMMON };
enum { OW_SUMMON_ONE, OW_SUMMON_TEAM, OW_SUMMON_ALL, OW_SUMMON_LAST = OW_SUMMON_ALL };
enum { OW_MOD_KICK, OW_MOD_BAN, OW_MOD_UNBAN, OW_MOD_MUTE, OW_MOD_UNMUTE, OW_MOD_SET_RANK,
       OW_MOD_LAST = OW_MOD_SET_RANK };
enum { OW_MOD_DONE, OW_MOD_NOT_ALLOWED, OW_MOD_UNKNOWN, OW_MOD_ALREADY, OW_MOD_OUTCOME_LAST = OW_MOD_ALREADY };
enum { OW_MARK_LAST = 3 };
enum { OW_DUEL_ASKED, OW_DUEL_COUNTDOWN, OW_DUEL_LIVE, OW_DUEL_ENDED, OW_DUEL_LAST = OW_DUEL_ENDED };
enum { OW_DUEL_END_NONE, OW_DUEL_END_WON, OW_DUEL_END_RING, OW_DUEL_END_LOADING, OW_DUEL_END_TIME,
       OW_DUEL_END_DECLINED, OW_DUEL_END_CANCELLED, OW_DUEL_END_LAST = OW_DUEL_END_CANCELLED };
enum { OW_WALLET_SPEND, OW_WALLET_EARN, OW_WALLET_TRADE, OW_WALLET_STAKE, OW_WALLET_LAST = OW_WALLET_STAKE };
enum { OW_LEDGER_LIGHT_FOUND, OW_LEDGER_LIGHT_SPENT, OW_LEDGER_SEED_FOUND, OW_LEDGER_SEED_SPENT,
       OW_LEDGER_LAST = OW_LEDGER_SEED_SPENT };
enum { OW_COMPANION_HORSE, OW_COMPANION_SAGE, OW_COMPANION_OTHER, OW_COMPANION_LAST = OW_COMPANION_OTHER };
enum { OW_DRAWN_NONE, OW_DRAWN_WEAPON, OW_DRAWN_BOW, OW_DRAWN_SHIELD, OW_DRAWN_LAST = OW_DRAWN_SHIELD };

/* Reasons carried by refuse, bye and depart. Numbers never change meaning; new ones are added at the end. */
enum {
    OW_WHY_NONE = 0,      /* an ordinary goodbye */
    OW_WHY_VERSION = 1,   /* protocol versions differ; refuse.version and refuse.build say the server's */
    OW_WHY_PROTOCOL = 2,  /* a frame broke the rules of this document; detail names the error */
    OW_WHY_AUTH = 3,      /* a handshake signature did not check */
    OW_WHY_UNKNOWN_KEY = 4, /* the key has no account and no claim code was given */
    OW_WHY_BAD_CLAIM = 5, /* the claim code is unknown or used */
    OW_WHY_FULL = 6,
    OW_WHY_BANNED = 7,
    OW_WHY_SHUTDOWN = 8,
    OW_WHY_TIMEOUT = 9,   /* 10 s without a frame */
    OW_WHY_DUPLICATE = 10, /* the same account connected again; the older session ends */
    OW_WHY_KICKED = 11,
};

/* Errors. Every one of them ends the connection when it comes from a peer. */
enum {
    OW_OK = 0,
    OW_E_SHORT = -1,      /* the payload ends inside a field */
    OW_E_TRAILING = -2,   /* bytes left after the last field */
    OW_E_RANGE = -3,      /* a value outside its set, a bool other than 0 or 1, a component kind 0 */
    OW_E_FLOAT = -4,      /* NaN or infinity */
    OW_E_UTF8 = -5,       /* a string that is not UTF-8 or holds a control character */
    OW_E_LIMIT = -6,      /* a count or length over the field's limit */
    OW_E_TOO_BIG = -7,    /* a frame over its type's limit */
    OW_E_TOO_SMALL = -8,  /* a frame under its type's smallest encoding */
    OW_E_TYPE = -9,       /* an unknown message type */
    OW_E_DIRECTION = -10, /* a type this side never receives */
    OW_E_STAGE = -11,     /* a handshake type after sealing, a sealed type before it */
    OW_E_SEAL = -12,      /* the tag did not check: changed, replayed, reordered or forged */
    OW_E_COUNTER = -13,   /* the frame counter ran out */
    OW_E_SPACE = -14,     /* the caller's output buffer is too small */
    OW_E_COMPONENT = -15, /* a component kind this build does not know (skip it) */
    OW_E_VERSION = -16,
    OW_E_MAGIC = -17,
    OW_E_AUTH = -18,      /* a handshake signature did not check */
    OW_E_KEY = -19,       /* the peer's exchange key gives no usable secret */
    OW_E_STEP = -20,      /* a handshake call out of order */
    OW_E_PIN = -21,       /* the server's identity is not the one the client expected */
    OW_E_FEATURE = -22,   /* a type of a feature the link was not granted */
};
const char *ow_strerror(int err);

/* Field kinds of a descriptor. */
enum { OW_K_U8, OW_K_U16, OW_K_U32, OW_K_U64, OW_K_I16, OW_K_I32, OW_K_F32, OW_K_BOOL, OW_K_VEC3, OW_K_QUAT,
       OW_K_B32, OW_K_B64, OW_K_STR, OW_K_BLOB, OW_K_OBJ, OW_K_LIST, OW_K_COMPS };
enum { OW_DIR_C2S = 1, OW_DIR_S2C = 2, OW_DIR_BOTH = 3 };
enum { OW_STAGE_PLAIN = 1, OW_STAGE_SEALED = 2, OW_STAGE_ANY = 3 };

/* One component as carried: kind, body length and body. The body's layout belongs to the kind (ow_comp_get). */
typedef struct ow_comp {
    uint8_t kind;
    uint16_t len;
    uint8_t data[OW_COMP_BODY_MAX];
} ow_comp;

#include "ow_messages.h"

/* Structs, one per sub-struct, message and component body, with fields in wire order. A STR is a NUL-terminated
 * char array of max + 1; a BLOB has <name>_len; a LIST and COMPS have <name>_count. */
#define OW_DECL(T, K, n, m, s) OW_DECL_##K(n, m, s)
#define OW_DECL_U8(n, m, s) uint8_t n;
#define OW_DECL_BOOL(n, m, s) uint8_t n;
#define OW_DECL_U16(n, m, s) uint16_t n;
#define OW_DECL_U32(n, m, s) uint32_t n;
#define OW_DECL_U64(n, m, s) uint64_t n;
#define OW_DECL_I16(n, m, s) int16_t n;
#define OW_DECL_I32(n, m, s) int32_t n;
#define OW_DECL_F32(n, m, s) float n;
#define OW_DECL_VEC3(n, m, s) float n[3];
#define OW_DECL_QUAT(n, m, s) float n[4];
#define OW_DECL_B32(n, m, s) uint8_t n[32];
#define OW_DECL_B64(n, m, s) uint8_t n[64];
#define OW_DECL_STR(n, m, s) char n[(m) + 1];
#define OW_DECL_BLOB(n, m, s) uint16_t n##_len; uint8_t n[m];
#define OW_DECL_OBJ(n, m, s) ow_##s n;
#define OW_DECL_LIST(n, m, s) uint16_t n##_count; ow_##s n[m];
#define OW_DECL_COMPS(n, m, s) uint8_t n##_count; ow_comp n[m];

#define OW_DEF_SUB(name) typedef struct ow_##name { OW_F_##name(OW_DECL, ow_##name) } ow_##name;
#define OW_DEF_MSG(id, name, dir, stage, cap, feat) typedef struct ow_##name { OW_F_##name(OW_DECL, ow_##name) } ow_##name;
#define OW_DEF_COMP(kind, name) typedef struct ow_c_##name { OW_F_c_##name(OW_DECL, ow_c_##name) } ow_c_##name;
OW_SUBSTRUCTS(OW_DEF_SUB)
OW_MESSAGES(OW_DEF_MSG)
OW_COMPONENTS(OW_DEF_COMP)

#define OW_ENUM_MSG(id, name, dir, stage, cap, feat) OW_T_##name = id,
enum ow_type { OW_MESSAGES(OW_ENUM_MSG) };
#define OW_ENUM_COMP(kind, name) OW_C_##name = kind,
enum ow_comp_kind { OW_COMPONENTS(OW_ENUM_COMP) };

/* Descriptors: what the codec walks. */
typedef struct ow_struct ow_struct;
typedef struct ow_field {
    uint8_t kind;
    uint16_t max;    /* limit of a STR, BLOB, LIST or COMPS; largest value of a U8 or U16 (0: any) */
    uint32_t off;    /* offset of the field in its struct */
    uint32_t aux;    /* offset of a BLOB's length or a LIST's or COMPS' count */
    const ow_struct *sub;
    const char *name;
} ow_field;
struct ow_struct {
    const char *name;
    const ow_field *fields;
    uint16_t nfields;
    uint32_t size;   /* sizeof the C struct */
};
typedef struct ow_msg_info {
    uint16_t type;
    const char *name;
    uint8_t dir, stage;
    uint32_t cap;
    uint32_t feature; /* OW_FEAT_* bit, 0 for core */
    const ow_struct *desc;
} ow_msg_info;

/* Message table. */
int ow_msg_count(void);
const ow_msg_info *ow_msg_at(int i);
const ow_msg_info *ow_msg(uint16_t type); /* NULL when unknown */
uint32_t ow_msg_limit(const ow_msg_info *m); /* payload limit: cap, or the largest encoding when cap is 0 */
uint32_t ow_msg_min(const ow_msg_info *m);   /* smallest encoding */
void ow_struct_bounds(const ow_struct *d, uint32_t *min, uint32_t *max);

/* Codec. Encoding refuses a struct the decoder would refuse; decoding is canonical: a payload that decodes encodes
 * back to the same bytes. Decode clears the whole struct first. */
int ow_struct_encode(const ow_struct *d, const void *s, uint8_t *out, size_t cap, size_t *len);
int ow_struct_decode(const ow_struct *d, const uint8_t *in, size_t len, void *s);
int ow_encode(uint16_t type, const void *msg, uint8_t *out, size_t cap, size_t *len);
int ow_decode(uint16_t type, const uint8_t *in, size_t len, void *msg);

/* Components. */
int ow_comp_count(void);
uint8_t ow_comp_kind_at(int i);
const ow_struct *ow_comp_desc(uint8_t kind); /* NULL when unknown */
int ow_comp_put(ow_comp *c, uint8_t kind, const void *body);
int ow_comp_get(const ow_comp *c, void *body); /* OW_E_COMPONENT for an unknown kind: skip it, keep the rest */

/* Links: one per connection, holding the sealing state of each direction. */
enum { OW_ROLE_CLIENT = 1, OW_ROLE_SERVER = 2 };
typedef struct ow_seal_dir {
    uint8_t key[32];
    uint8_t prefix[16];
    uint64_t counter;
} ow_seal_dir;
typedef struct ow_link {
    uint8_t role;
    uint8_t sealed;
    uint32_t features; /* granted add-ons; 0 until admit */
    ow_seal_dir tx, rx;
} ow_link;
void ow_link_init(ow_link *l, int role);
/* After admit: the client sets admit.features, the server what it granted. */
void ow_link_set_features(ow_link *l, uint32_t features);
void ow_link_wipe(ow_link *l);

/* Writes one frame of `type` into out. Before sealing only PLAIN and ANY types go; after it only SEALED and ANY,
 * each sealed. A side writes only the types of its direction, and of a feature only when the link has it. */
int ow_frame_write(ow_link *l, uint16_t type, const void *msg, uint8_t *out, size_t cap, size_t *len);

/* Stream reader. Takes bytes from *data (advancing it) until one frame is whole, checking the header against the
 * type's direction, stage, feature and limits as soon as it is in. Returns 1 with *f set (valid until the next call), 0 when
 * the input ran out, or an error, which sticks. It never takes bytes past the frame it returns, so the caller can
 * change the link (seal it) between frames. */
typedef struct ow_frame {
    uint16_t type;
    uint32_t len;
    const uint8_t *payload;
} ow_frame;
typedef struct ow_reader {
    uint32_t have;
    uint32_t want;
    int err;
    uint8_t buf[OW_FRAME_MAX];
} ow_reader;
void ow_reader_init(ow_reader *r);
int ow_reader_take(ow_link *l, ow_reader *r, const uint8_t **data, size_t *n, ow_frame *f);

/* Identity: an Ed25519 key pair. */
typedef struct ow_identity {
    uint8_t secret[64];
    uint8_t pub[32];
} ow_identity;
void ow_identity_from_seed(ow_identity *id, const uint8_t seed[32]);
void ow_identity_wipe(ow_identity *id);

/* Handshake (docs/online-wire.md, "Handshake"). The caller moves the frames; these calls build and check them. */
typedef struct ow_handshake {
    uint8_t role, step;
    uint8_t exchange_secret[32];
    uint8_t transcript[64];
    uint8_t greet[OW_HEADER + 1024];
    uint32_t greet_len;
    uint8_t keys[96]; /* c2s key, c2s prefix, s2c key, s2c prefix */
    uint8_t peer_identity[32];
} ow_handshake;
/* random: 64 bytes from ow_random (nonce, then exchange secret). */
int ow_hs_client_greet(ow_handshake *h, const uint8_t random[64], const char *build, ow_greet *out);
/* pin: the server identity the client expects, or NULL to accept any (the caller then records ch->identity). */
/* features: the OW_FEAT_* add-ons the client offers; admit.features says which the server granted. */
int ow_hs_client_challenge(ow_handshake *h, const ow_challenge *ch, const uint8_t *pin, const ow_identity *me,
                           const char *nickname, uint8_t character, const char *claim, uint32_t features,
                           ow_proof *out);
int ow_hs_server_greet(ow_handshake *h, const ow_identity *server, const uint8_t random[64], const ow_greet *g,
                       ow_challenge *out);
int ow_hs_server_proof(ow_handshake *h, const ow_proof *p);
/* Puts the derived keys into the link and seals it: the client right after the challenge checks, the server right
 * after writing the challenge. Wipes the handshake's secrets. */
int ow_hs_seal(ow_handshake *h, ow_link *l);
void ow_hs_wipe(ow_handshake *h);
/* Reads magic and version from a greet payload of any version, so a server can refuse a client of another version
 * with a reason instead of a decode error. */
int ow_greet_peek(const uint8_t *payload, size_t len, uint32_t *magic, uint16_t *version);
/* A refuse for the given reason, with this build's version. */
void ow_refuse_make(ow_refuse *r, uint16_t reason, const char *build, const char *detail);

/* Clock: from a ping sent at local t0, a pong whose reply_us is the peer's clock, received at local t1. */
void ow_clock_sample(uint64_t t0, uint64_t peer_us, uint64_t t1, int64_t *offset_us, uint64_t *rtt_us);

/* Random bytes from the operating system. Returns 0 on success. */
int ow_random(void *buf, size_t n);

#ifdef __cplusplus
}
#endif
#endif
