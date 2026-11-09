/* The actors add-on on the server: one record per shared actor, interest, the owner rules applied, and the relays of
 * states, hits, spawns, gone and take-once.
 *
 * docs/online-wire.md section 9.1 is what both ends do; sv_owner.h holds the rules' numbers. The library keeps no
 * socket and no clock of its own: sv_room.c (and tests/actor_lab.c on a virtual clock) tells it who is in the room,
 * where each player is, every actor message and the time, and it sends through the io callbacks. Single-threaded,
 * like the room.
 */
#ifndef SV_ACTORS_H
#define SV_ACTORS_H

#include <stdint.h>

#include "../online/proto/ow_wire.h"
#include "sv_owner.h"

/* Records at once over the whole room; offers and spawns past it are ignored and counted. */
#define SVA_RECORDS_MAX 16384
#define SVA_GROUPS_MAX 1024
/* Actor frames a game may send at once and then each second (they do not count against the room's frame budget,
 * since loading into a busy place offers hundreds of actors at once). Past it the game is cut off as a flood. */
#define SVA_FRAME_BURST 2000.0
#define SVA_FRAME_RATE 400.0
/* Hits from one game: 120 in 5 s. */
#define SVA_HIT_BURST 120.0
#define SVA_HIT_RATE 24.0
/* How long a taken thing's id is remembered, so a late take learns who got it. */
#define SVA_TAKEN_KEEP (600 * SVO_SEC)
/* Interest: a game leaves an actor's interest this much beyond rules.interest_m. */
#define SVA_INTEREST_SLACK_M 20.0f

typedef struct sva_io {
    void *ctx;
    /* Sends one message to the game of account. */
    void (*send)(void *ctx, uint64_t account, uint16_t type, const void *msg);
    /* A line for the server's log. */
    void (*log)(void *ctx, const char *line);
    /* Told of every owner change (may be NULL): the replay lab counts them. */
    void (*changed)(void *ctx, const ow_aid *id, uint32_t scene, const char *name, uint64_t from, uint64_t to,
                    uint8_t cause, uint32_t epoch, uint64_t now);
} sva_io;

typedef struct sva_stats {
    uint64_t changes[OW_OWNER_LAST + 1];  /* owner changes by cause, to an owner or to none */
    uint64_t kept[SVO_KEPT_COUNT];        /* evaluations that asked for a change and were held back, by rule */
    uint64_t records, records_peak;
    uint64_t offers, offers_unshared, offers_refused, full;
    uint64_t states, states_dropped, relayed;
    uint64_t hits, hits_passed, hits_dropped;
    uint64_t spawns, spawns_refused, gone, takes, takes_won;
    uint64_t holds, holds_refused;
    uint64_t silent; /* actors taken from an owner that stopped sending their states (rule 9) */
} sva_stats;

/* Starts an empty room. interest_m is rules.interest_m. */
void sva_init(const sva_io *io, float interest_m);
/* Frees every record. */
void sva_shutdown(void);

/* An admitted session (new, or resumed after a lost connection). */
void sva_player_join(uint64_t account, uint64_t now);
/* Its connection was lost and the session is held: its actors are free for rule 1 (rule 9). */
void sva_player_away(uint64_t account, uint64_t now);
/* The session ended for good. */
void sva_player_leave(uint64_t account, uint64_t now);
void sva_player_area(uint64_t account, const ow_place *where, uint64_t now);
void sva_player_pos(uint64_t account, uint32_t scene, const float pos[3], uint64_t now);

/* Whether the game of account may send one more actor frame now (SVA_FRAME_BURST, SVA_FRAME_RATE). */
int sva_frame_ok(uint64_t account, uint64_t now);
/* One decoded actor message (types 0x0400 to 0x040E) from the game of account. */
void sva_message(uint64_t account, uint16_t type, const void *msg, uint64_t now);
/* Call at least every 100 ms: once a second it runs interest, followers, clusters and the rules for every record,
 * and once a minute it logs the counters. */
void sva_tick(uint64_t now);

const sva_stats *sva_get_stats(void);
/* The record of a placed actor (origin 0) in scene, or of a run-time one (scene ignored): 1 with its owner and epoch,
 * 0 when there is none. */
int sva_owner_of(uint32_t scene, const ow_aid *id, uint64_t *owner, uint32_t *epoch);

#endif
