// Actor sync, the client side of the actors add-on (docs/online-wire.md 0x0400 to 0x040C, docs/online-actors.md).
//
// One body per actor on every screen: the game the server names owner runs it, every other game shows a copy that the
// owner's states drive, and a hit on a copy goes to the owner, whose game counts it. Life travels in the owner's states,
// so it is the same everywhere.
//
// This file knows no game memory. The game side (runtime/net_actors_game.c, to come) fills NaGame with what it can read
// and do in the recompiled game; tests/net_actors_lab.c fills it with invented games, so every rule here runs offline.
// A world (NaWorld) is one game's view; the lab runs several in one process.
//
// Threading: every call on a world comes from one thread (the game's frame step). Frames from the network thread are
// queued by the caller (net_client.c) and handed over with na_on_message on that thread.
#pragma once
#include <stdint.h>

#include "../../online/proto/ow_wire.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NA_MAX 1024          // actors a world follows at once
#define NA_SEEN_MAX 512      // actors one scan may report

// What the game sees of one of its own actors, each scan.
typedef struct {
    uint64_t handle;         // the game's own reference (its actor address), never 0
    uint64_t map_id;         // the map instance id of a placed actor, 0 for a run-time one
    uint8_t cls;             // OW_CLASS_*
    uint8_t auto_placed;     // a placement manager made it (wildlife and random monsters)
    uint8_t is_copy;         // this world made it as a copy (the game side remembers its own creations)
    uint8_t dead;            // life reached 0 or the game began removing it
    char name[OW_ACTOR_NAME_MAX + 1];
    ow_body body;
    int32_t life, life_max;  // life_max 0: the actor has no life
} NaSeen;

typedef struct NaGame {
    void *ctx;
    // Fills out with the actors resident now; returns the count.
    int (*scan)(void *ctx, NaSeen *out, int max);
    // Creates a copy that has no AI of its own; returns its handle, 0 when the game could not.
    uint64_t (*make_copy)(void *ctx, uint8_t cls, const char *name, const ow_body *body);
    // Moves a copy (or holds an actor this game does not own) to a state, with its life when life_max > 0.
    void (*drive)(void *ctx, uint64_t handle, const ow_body *body, int32_t life, int32_t life_max);
    // 1: the actor's own AI and physics stop (a copy); 0: they run (this game owns it).
    void (*puppet)(void *ctx, uint64_t handle, int on);
    // Hits this game's own actor for a hit another game landed on its copy.
    void (*hit)(void *ctx, uint64_t handle, const ow_hit_info *hit);
    // Takes an actor out through the game's own removal; cause is OW_GONE_* (killed: its death, with loot only on the owner).
    void (*remove)(void *ctx, uint64_t handle, uint8_t cause);
    // The actor name whose name hash (ow_name_hash) is hash, for herd rows; NULL when the game knows none. May be NULL.
    const char *(*name_of)(void *ctx, uint32_t hash);
} NaGame;

typedef struct {
    // what went out
    uint32_t offers, releases, poses, hits_out, spawns, gones_out, herds_out;
    // what came in and what it did
    uint32_t owned, copies, handovers, hits_in, spawned_in, gones_in, made, removed, refused_spawns, replaced;
    uint32_t stale;          // messages of an older epoch, dropped
} NaStats;

typedef struct NaWorld NaWorld;

// Sends one message (an ow_* struct of that type); returns 0 when queued.
typedef int (*NaSend)(void *ctx, uint16_t type, const void *msg);

NaWorld *na_new(const NaGame *game, NaSend send, void *send_ctx);
void na_free(NaWorld *w);
// The account this world plays as (0 until admitted) and the server's clock in microseconds, both from net_client.c.
void na_set_self(NaWorld *w, uint64_t account);
// The first serial of this game's run-time actors: the session's admit.server_time_us, so ids are never reused, not even
// across a reconnect (docs/online-wire.md 9.1, "Ids"). Each spawn takes the next.
void na_set_serial_base(NaWorld *w, uint64_t first);
// One frame step: scan, offer, send states, drive copies, apply the spawn and wildlife rules. now_us: server clock.
void na_step(NaWorld *w, uint64_t now_us);
// A frame of the actors add-on from the server (owner, actor_recv, hit_in, spawned, gone, herd, taken).
void na_on_message(NaWorld *w, uint16_t type, const void *msg);
// The game's damage path, for a hit on an actor of this game. Returns 1 when the hit must not count here (the actor is
// a copy: the hit went to its owner instead), 0 when the game counts it as usual.
int na_local_hit(NaWorld *w, uint64_t handle, const ow_hit_info *hit);
// The game asks to place an auto-placed animal or monster at pos: 0 refuses (another game leads this place), 1 allows.
int na_allow_auto_place(NaWorld *w, uint8_t cls, const float pos[3]);
// The map cell the local player stands in (herd.cell), from net_client.c's position report.
void na_set_place(NaWorld *w, uint32_t cell);
// The connection went away: copies are removed and this game takes back what it shows.
void na_offline(NaWorld *w);
const NaStats *na_stats(const NaWorld *w);

// What a world knows of one actor, for the lab and the log.
typedef enum { NA_LOCAL = 0, NA_OFFERED, NA_OWNED, NA_COPY } NaRole;
typedef struct {
    ow_aid id;
    NaRole role;
    uint32_t epoch;
    uint64_t owner, handle;
    int32_t life, life_max;
    uint8_t cls;
} NaInfo;
int na_info(const NaWorld *w, const ow_aid *id, NaInfo *out);   // 1 when the world follows it
int na_count(const NaWorld *w, NaRole role);

// The class of an actor by its name from the class table both ends compile (online/proto/classes.txt, ow_class_find);
// OW_CLASS_LAST+1 for a name no row shares.
uint8_t na_class_of(const char *name);

#ifdef __cplusplus
}
#endif
