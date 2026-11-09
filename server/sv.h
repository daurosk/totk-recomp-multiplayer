/* The release 1 multiplayer server: its configuration, its data folder and the room it runs.
 *
 * Described for hosts in docs/server.md; the protocol is docs/online-wire.md. One process, one thread for the poll
 * loop (sv_main.c) and one that only reads console lines (sv_os.c). Every client is treated as hostile: the wire
 * layer (online/proto) refuses any malformed frame, and the room (sv_room.c) checks every request against the
 * sender's rank, the room's state and per-player rate limits before it changes anything.
 */
#ifndef SV_H
#define SV_H

#include <stdint.h>
#include <stdio.h>

#include "../online/proto/ow_wire.h"
#include "sv_os.h"

#define SV_BUILD "TotK Recomp Multiplayer server 0.1.0"
#define SV_PORT 27120
/* Sockets the server keeps open at once, players and handshakes together. */
#define SV_MAX_CONNS 96
/* Handshakes in progress at once, over all addresses. */
#define SV_MAX_PENDING 32
/* Add-ons this build grants (docs/online-wire.md, "Features and add-ons"); a client's offer is answered with the
 * bits it offered of these. An add-on is plugged in by implementing its message types in sv_room.c's dispatch,
 * then adding its OW_FEAT_* bit here. Puppets: sv_puppets.c. Actors: sv_actors.c. */
#define SV_FEATURES_GRANTED ((uint32_t)(OW_FEAT_PUPPETS | OW_FEAT_ACTORS))

/* ---- configuration (sv_store.c) ---- */
typedef struct sv_config {
    char bind[64];
    uint16_t port;
    char name[OW_NICK_MAX + 1];
    char motd[OW_NOTICE_MAX + 1];
    int max_players;          /* 1 to OW_PLAYERS_MAX */
    char password[OW_CLAIM_MAX + 1]; /* join password new accounts give as their claim; "" for none */
    int open;                 /* 1: any new key may make an account; 0: only with a one-time claim code */
    char host[65];            /* hex Ed25519 key of the host; "" makes the first account the host */
    int hold_seconds;         /* how long a lost session waits for its player to reconnect */
    int handshake_seconds;    /* a connection has this long to finish the handshake */
    int idle_seconds;         /* a session without a frame for this long ends */
    int teleport;             /* teleport requests allowed */
    int positions_team_only;  /* players see only their team's positions (the host sees everyone) */
    int max_per_ip;           /* connections at once from one address */
    int max_pending_per_ip;   /* handshakes at once from one address */
    int connects_per_minute;  /* new connections from one address per minute (bursts of a quarter of it) */
    int new_accounts_per_hour; /* accounts one address may make per hour (all at once at most); throttles guessing too */
    int chat_burst;           /* chat lines a player may send at once */
    double chat_per_second;   /* and then this many each second */
    int action_burst;         /* team, teleport, marker and host requests at once */
    double action_per_second;
    int frame_burst;          /* frames of any kind at once; over this the client is cut off */
    double frames_per_second;
    int log_chat;             /* write chat lines to the log */
} sv_config;

void sv_config_defaults(sv_config *c);
/* Reads path into c, over the defaults. A missing file is written with the defaults and their explanations.
 * Returns 0 on success; on an error prints the line and returns -1. */
int sv_config_load(sv_config *c, const char *path);

/* ---- the data folder (sv_store.c) ---- */
typedef struct sv_account {
    uint64_t id;
    uint8_t key[32];
    char nick[OW_NICK_MAX + 1]; /* last name the room gave it */
} sv_account;

/* A ban or a mute: the key, when it ends (seconds since 1970, 0 for until lifted) and why. */
typedef struct sv_sanction {
    uint8_t key[32];
    int64_t until;
    char reason[OW_REASON_MAX + 1];
} sv_sanction;

typedef struct sv_sanctions {
    const char *file;
    sv_sanction *v;
    int n, cap;
} sv_sanctions;

typedef struct sv_store {
    char dir[512];
    ow_identity identity;
    sv_account *acc;
    int nacc, capacc;
    uint64_t next_id;
    sv_sanctions bans, mutes;
    uint8_t (*mods)[32];
    int nmods, capmods;
    int has_host, host_from_config;
    uint8_t host[32];
    char (*claims)[OW_CLAIM_MAX + 1];
    int nclaims, capclaims;
    FILE *log;
} sv_store;

extern sv_store g_store;

int sv_store_open(const char *dir, const sv_config *c);
void sv_store_close(void);
/* Re-reads bans, mutes and moderators after the host edited them. */
void sv_store_reload(void);
void sv_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

sv_account *sv_account_by_key(const uint8_t key[32]);
sv_account *sv_account_by_id(uint64_t id);
sv_account *sv_account_add(const uint8_t key[32], const char *nick);
void sv_accounts_save(void);

/* The active entry for key, or NULL. Entries that ended are dropped on the way. */
sv_sanction *sv_sanction_find(sv_sanctions *s, const uint8_t key[32]);
void sv_sanction_set(sv_sanctions *s, const uint8_t key[32], int64_t until, const char *reason);
int sv_sanction_remove(sv_sanctions *s, const uint8_t key[32]);

int sv_is_moderator(const uint8_t key[32]);
void sv_set_moderator(const uint8_t key[32], int on);
void sv_set_host(const uint8_t key[32]);
/* The rank a key has: host, moderator or player. */
int sv_rank_of(const uint8_t key[32]);

/* Takes a one-time claim code: 1 when it existed (it is then used up). */
int sv_claim_take(const char *code);
/* Whether a claim field holds the join password, compared in constant time. */
int sv_password_matches(const char *given, const char *password);
/* Makes a new one-time claim code into out (at least 16 bytes). */
void sv_claim_new(char *out);

void sv_hex(char *out, const uint8_t *in, size_t n);
/* Copies src into dst (max bytes plus the NUL), cut at a UTF-8 character boundary. */
void sv_copy_cut(char *dst, size_t max, const char *src);
int sv_unhex(uint8_t *out, size_t n, const char *in);

/* ---- the room (sv_room.c) ---- */
int sv_room_start(const sv_config *c);
/* One turn of the loop: waits at most timeout_ms for the network, then runs timers and sends what is due. */
void sv_room_poll(int timeout_ms);
/* Says goodbye to everyone and closes every socket. */
void sv_room_stop(void);
/* A console line from the person hosting (sv_room.c, "Console"). Returns 1 when the line asks the server to stop. */
int sv_console(const char *line);

/* What the add-ons may know of the player in a slot (0 to OW_PLAYERS_MAX - 1). */
typedef struct sv_peer {
    uint64_t account;
    ow_place area;
    int has_pos;           /* pos holds the latest 1 Hz position */
    float pos[3];
    int puppets;           /* the session was granted the puppets add-on */
    float interest_m;      /* the room's interest distance */
} sv_peer;
/* Fills out for a live player (connected, not held). Returns 0 for a free or held slot. */
int sv_room_peer(int slot, sv_peer *out);
/* Sends a message to the live player in a slot; nothing for a free or held one. */
void sv_room_send(int slot, uint16_t type, const void *msg);

/* ---- puppets (sv_puppets.c) ---- */
/* A self_state from the player in slot: relayed as player_state to the players that can see it. */
void sv_puppets_state(int slot, const ow_self_state *m, uint64_t now);
/* The player in slot reported a new area. */
void sv_puppets_area(int slot, const ow_place *area);
/* The slot was freed or given to a new session. */
void sv_puppets_drop(int slot);

#endif
