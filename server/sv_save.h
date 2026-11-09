/* Saves and world state on the server (docs/online-saves.md): each account's own GameData values and the room's
 * shared ones, the rule each shared key follows, and the files they live in.
 *
 * Kept apart from sv_room.c so the room only calls in at four points: a player joins (the snapshot), a save_write or
 * world_write arrives, the loop's timer (flush at most every 5 s) and shutdown. Nothing here touches sockets: the
 * caller hands in a send function, which keeps the rules testable without a network (tests/sv_save_check.c). */
#ifndef SV_SAVE_H
#define SV_SAVE_H

#include <stdint.h>

#include "../online/proto/ow_wire.h"

/* The rule a shared key follows (docs/online-saves.md, "Rules"). */
enum {
    SV_RULE_LATEST = 0,  /* the last write wins */
    SV_RULE_FORWARD = 1, /* never back: true stays true, numbers only grow (signed or unsigned by kind) */
    SV_RULE_STEPS = 2,   /* never back for enums and text: a value the key held before is not taken again */
    SV_RULE_OWNER = 3,   /* only the world owner's game writes it: clock, weather, townspeople schedules */
    SV_RULE_PLAYER = 4,  /* per player; a world_write of it is refused and the writer keeps its own copy */
};

/* What sv_save sends: to one account, or to every live account but `except` when to is 0. */
typedef void (*sv_save_send_fn)(uint64_t to, uint64_t except, uint16_t type, const void *msg);

/* Opens <dir>/world.json, <dir>/world-keys.tsv (optional) and makes <dir>/accounts. 0 on success. */
int sv_save_open(const char *dir, sv_save_send_fn send);
/* Writes everything that changed and frees it all. */
void sv_save_close(void);
/* Writes the files that changed since the last flush, at most every 5 s unless force. */
void sv_save_flush(uint64_t now_us, int force);

/* The world owner: the host's game while it is connected, else the first live game that wrote an owner key, sticky
 * while it stays connected. sv_room.c reports who is live and who is the host. */
void sv_save_live(uint64_t account, int live, int is_host);

/* On join, after admit and before join_ready: the account's save as save_part messages, then the world as
 * world_part messages. */
void sv_save_snapshot(uint64_t account);

void sv_save_on_save_write(uint64_t account, const ow_save_write *m);
void sv_save_on_world_write(uint64_t account, const ow_world_write *m);

/* For server features (wallet, ledger, Blood Moon): read and write one account's value, or the world's.
 * get copies at most cap bytes and returns the value's length, or -1 when the key has none. A put to an account
 * also sends that account a save_fix so its game takes the value; a put to the world sends world_set to everyone. */
int sv_save_get(uint64_t account, uint32_t key, uint16_t index, uint8_t *kind, uint8_t *out, int cap);
void sv_save_put(uint64_t account, uint32_t key, uint16_t index, uint8_t kind, const uint8_t *v, int len);
int sv_world_get(uint32_t key, uint16_t index, uint8_t *kind, uint8_t *out, int cap);
void sv_world_put(uint32_t key, uint16_t index, uint8_t kind, const uint8_t *v, int len);

/* The rule of a shared key (SV_RULE_LATEST when world-keys.tsv has no row). */
int sv_save_rule(uint32_t key);

#endif
