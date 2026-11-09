/* The owner rules of the actors add-on: who runs a shared actor, decided from facts the caller gathers.
 *
 * docs/online-wire.md section 9.1 states the rules; this file names their numbers and sv_owner.c applies them. Pure:
 * no records, no clock, no I/O, so tests/actor_lab.c can drive every rule on a virtual clock. The numbers are the
 * ones the old port's rounds settled (its SyncFix S1 rules), re-expressed in our own terms.
 *
 * sv_actors.c runs the parts that need records (holds over a whole group, followers, the leaving owner's actors) and
 * calls svo_decide for one actor, or for a group as one actor at its centre.
 */
#ifndef SV_OWNER_H
#define SV_OWNER_H

#include <stdint.h>

#include "../online/proto/ow_classes.h"
#include "../online/proto/ow_wire.h"

#define SVO_SEC 1000000ull

/* Rule 1, no owner: the candidate within NEAR_M with no other present player ORPHAN_MARGIN_M nearer; a nearer player
 * whose game has not offered holds it back for ORPHAN_WAIT at most. */
#define SVO_NEAR_M 120.0f
#define SVO_ORPHAN_MARGIN_M 5.0f
#define SVO_ORPHAN_WAIT (3 * SVO_SEC)
/* Rule 2, owner far: the owner's player beyond NEAR_M, a candidate within NEAR_M and FAR_MARGIN_M nearer, FAR_HOLD
 * after the last change. */
#define SVO_FAR_MARGIN_M 10.0f
#define SVO_FAR_HOLD (5 * SVO_SEC)
/* Rule 3, clearly closer: at most half the owner's distance and CLOSER_MARGIN_M nearer, CLOSER_HOLD after the last
 * change. */
#define SVO_CLOSER_MARGIN_M 15.0f
#define SVO_CLOSER_HOLD (20 * SVO_SEC)
/* Rule 4, pick-up: within PICKUP_M; with both players within PICKUP_BOTH_M it waits CLOSER_HOLD. */
#define SVO_PICKUP_M 3.0f
#define SVO_PICKUP_BOTH_M 20.0f
/* Rule 5, still things (every class): not moved STILL_MOVE_M in STILL_AFTER. */
#define SVO_STILL_MOVE_M 1.0f
#define SVO_STILL_AFTER (5 * SVO_SEC)
/* Rule 7, fights: hit within FIGHT_HIT; a boss while a present player is within NEAR_M; owner far only beyond
 * FIGHT_FAR_M; an owner paused over PAUSE_AWAY counts as away (rule 9, for every actor). */
#define SVO_FIGHT_HIT (10 * SVO_SEC)
#define SVO_FIGHT_FAR_M 160.0f
#define SVO_PAUSE_AWAY (6 * SVO_SEC)
/* Rule 9: an owner that sent no state of an actor for OWNER_SILENT has stopped running it. */
#define SVO_OWNER_SILENT (2 * SVO_SEC)
/* Rule 8, followers. */
#define SVO_PART_NEAR_M 60.0f
#define SVO_HELD_NEAR_M 12.0f
/* Rule 10, holds: after a hold ends the actor stays HOLD_AFTER with the holder. */
#define SVO_HOLD_AFTER (5 * SVO_SEC)
/* Rule 11, clusters: parts within CLUSTER_LINK_M, chained; a vehicle when one is a core row or two moved
 * CLUSTER_MOVED_M since first seen. */
#define SVO_CLUSTER_LINK_M 6.0f
#define SVO_CLUSTER_MOVED_M 1.0f
/* Hits pass only from a player within HIT_NEAR_M of the actor. */
#define SVO_HIT_NEAR_M 80.0f
/* A distance for "not in this scene" or "position unknown". */
#define SVO_FAR_AWAY 1e30f

/* One game as the rules see it, for one actor. */
typedef struct svo_game {
    uint64_t account;
    uint8_t present;   /* connected and playing */
    uint8_t away;      /* loading, dead, on the title, its connection lost and held, or paused over PAUSE_AWAY */
    uint8_t candidate; /* offered the actor and has not unloaded it */
    uint8_t gave_up;   /* released or unloaded it since its last offer: it will not offer again soon */
    float dist;        /* its player to the actor; SVO_FAR_AWAY when in another scene or unknown */
} svo_game;

/* One actor (or a group as one) as the rules see it. */
typedef struct svo_actor {
    uint8_t class_;          /* OW_CLASS_* */
    uint32_t rules;          /* OW_CR_* of its row (for a group, see sv_actors.c group_facts) */
    uint64_t owner;          /* account, 0 for none */
    uint64_t holder;         /* account holding it, 0 for none */
    uint64_t hold_until;     /* the end of the stay after a hold */
    uint64_t last_change;    /* time of the last owner change, the first owner included */
    uint64_t unowned_since;  /* when it last became ownerless (or was first seen) */
    uint64_t moved_at;       /* the last time it moved STILL_MOVE_M from where it was */
    uint64_t last_hit;       /* the last hit passed to it (or to one of its parts), 0 for none */
    uint8_t asleep;          /* the owner's last state had OW_POSE_ASLEEP */
    uint8_t left_by_owner;   /* its last owner left or released it: a new owner comes by OW_OWNER_LEFT */
} svo_actor;

/* What the rules held back, for the server's counters and the replay lab. */
enum {
    SVO_KEPT_NONE,       /* nothing to do: no rule asked for a change */
    SVO_KEPT_FIXED,
    SVO_KEPT_HELD,       /* a hold, or the stay after one */
    SVO_KEPT_QUEST,
    SVO_KEPT_FIGHT,
    SVO_KEPT_ASLEEP,
    SVO_KEPT_STILL,
    SVO_KEPT_RECENT,     /* far, closer or pick-up asked too soon after the last change */
    SVO_KEPT_FAR_MARGIN, /* owner far, but no candidate near enough or enough nearer */
    SVO_KEPT_ORPHAN_NEARER, /* no owner: a nearer player's game may still offer */
    SVO_KEPT_ORPHAN_FAR,    /* no owner: no candidate within NEAR_M */
    SVO_KEPT_PAUSED,     /* the owner paused, not yet long */
    SVO_KEPT_COUNT
};

typedef struct svo_result {
    int change;       /* 1: give the actor to `to` (0 for none) by `cause` */
    uint64_t to;
    uint8_t cause;    /* OW_OWNER_* */
    uint32_t hold_ms; /* the owner message's hold_ms */
    int kept;         /* SVO_KEPT_*: why not, when change is 0 */
} svo_result;

/* Decides one actor at time now. games lists every game in the room (the owner among them if it is still in the
 * room; an owner missing from the list has left). */
void svo_decide(const svo_actor *a, const svo_game *games, int n, uint64_t now, svo_result *out);

/* Whether rule 7 holds the actor for a fight: a boss with a present player within NEAR_M, or an enemy hit lately. */
int svo_in_fight(const svo_actor *a, const svo_game *games, int n, uint64_t now);

/* The hold_ms an owner message carries after a change by cause for an actor of this class and rules: how long no rule
 * but the owner leaving or a hold moves it. */
uint32_t svo_hold_ms(uint8_t class_, uint32_t rules, uint8_t cause);

const char *svo_kept_name(int kept);
const char *svo_cause_name(uint8_t cause);

#endif
