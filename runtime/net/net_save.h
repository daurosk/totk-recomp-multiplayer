// Saves and world state on the client (docs/online-saves.md): the contract between the GameData hooks, the network
// client and other online features (wallet, ledger, Blood Moon) that read or change the player's own values.
//
// Implemented by runtime/net/net_save.c. The GameData hooks that call the hook functions are not in yet
// (docs/online-saves.md, "Not done yet"): until they land the game neither sends its writes nor takes the server's.
//
// Values. A value is one GameData entry: key hash (u32), array index (0xFFFF for a scalar), an OW_GD_* kind and up to
// 256 little-endian bytes, exactly the wire's gd_entry. Every key is either the player's own (hearts, stamina, pouch,
// rupees, armour, chests, records: sent in save_write, kept in accounts/<id>/save.json) or shared by the room (quests,
// Koroks, map reveals, shrines, felled bosses: sent in world_write, the server applies the key's rule and sends the
// result to every game as world_set). net_save_shared() says which.
//
// Threads. Every function may be called from any thread; the module guards its own state. The hooks call
// net_save_game_wrote and net_save_take_incoming on the thread that runs the game's GameData commit.
#ifndef NET_SAVE_H
#define NET_SAVE_H

#include <stdint.h>

#define NET_SAVE_SCALAR 0xFFFFu
#define NET_SAVE_VALUE_MAX 256

typedef struct NetSaveValue {
    uint32_t key;
    uint16_t index;
    uint8_t kind;              // OW_GD_* (online/proto/ow_wire.h)
    uint16_t len;
    uint8_t v[NET_SAVE_VALUE_MAX];
} NetSaveValue;

// ---- for features (wallet, ledger, Blood Moon, ...)

// The player's own value as the server last confirmed it (save_part, save_fix) or this game last wrote it.
// Returns 1 and fills out when the key has one, 0 otherwise. Works offline too, from the game's own writes.
int net_save_get(uint32_t key, uint16_t index, NetSaveValue *out);
// Convenience for a scalar s32 such as the rupee count; 1 when found.
int net_save_get_s32(uint32_t key, int32_t *out);

// Sets one of the player's own values: the game takes it at its next GameData commit (as if the game had written it)
// and, while online, the server stores it for this account. Use for values a feature owns, not to fight the game:
// the game's next write of the key wins as usual. Returns 0 for a shared key (the server owns those).
int net_save_put(const NetSaveValue *v);
int net_save_put_s32(uint32_t key, int32_t value);

// Called once for each own value the server corrected (save_fix) or sent on join (save_part), after it is applied.
// One listener per feature, at most 8; fn runs on the network thread and must not block.
typedef void (*NetSaveListener)(const NetSaveValue *v, void *ctx);
int net_save_listen(NetSaveListener fn, void *ctx);

// Whether a key is shared by the room (1) or the player's own (0), from the key table (docs/online-saves.md).
int net_save_shared(uint32_t key);

// ---- for the GameData hooks

// The game queued a write (caught before its commit). Batched and sent every 250 ms while online; counters the game
// rewrites every frame are held back by the anti-spam list.
void net_save_game_wrote(const NetSaveValue *v);
// Values the game must take at this commit: the join snapshot (until net_save_snapshot_done), world_set, save_fix and
// net_save_put. Returns how many were copied into out (at most max); call again until it returns 0.
int net_save_take_incoming(NetSaveValue *out, int max);
// 1 once the whole join snapshot (every save_part and world_part) has arrived: the title waits for it before it hands
// over to the world, so the game starts from the account's save.
int net_save_snapshot_done(void);

// ---- for net_client.c (the network thread)

// A connection attempt starts: net_save_snapshot_done says 0 until the join's snapshot is in or the attempt ends.
void net_save_connecting(void);
// The server admitted this game; granted says whether it granted OW_FEAT_SAVES.
void net_save_admitted(int granted);
// One s2c message of the saves add-on (save_part, world_part, world_set, save_fix); 1 when it was one of them.
int net_save_on_msg(uint16_t type, const void *msg);
// join_ready arrived: the snapshot is complete.
void net_save_joined(void);
// While live, on every pass of the client's loop: sends the due save_write / world_write batch through send.
void net_save_pump(uint64_t now_us, int (*send)(uint16_t type, const void *msg));
// The connection ended or was stopped. Writes not yet sent are dropped: the next join's snapshot decides.
void net_save_offline(void);

#endif
