// The link between net_client.c (the network thread) and the actor sync core (runtime/net/net_actors.c), which runs on
// the game's side. docs/online-actors.md, "The link".
//
// The network thread hands each 0x04xx frame from the server to nal_from_server as it arrives and sends the core's
// messages with nal_send_out; the game side calls nal_tick once per frame, which feeds the frames to the core and
// steps it. Two byte queues under one mutex carry encoded payloads between the threads, so neither waits on the other
// for more than a copy. The add-on is offered only when the game side has an adapter (nal_offer).
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "net/net_actors.h"

#ifdef __cplusplus
extern "C" {
#endif

// The game side's adapter (runtime/net_actors_game.c); NULL while it cannot make, drive and remove copies.
const NaGame *na_game(void);
// The read-only scan of the resident actors (runtime/net_actors_game.c).
int na_game_scan(NaSeen *out, int max);

// The OW_FEAT_ACTORS bit to offer in the sign-in, 0 without an adapter. Network thread.
uint32_t nal_offer(void);
// The session was admitted: the account, the server's clock at admit and the add-ons it granted. Network thread.
void nal_admitted(uint64_t account, uint64_t server_time_us, uint32_t features);
// The server's clock offset got better (server time minus plat_now_ns / 1000). Network thread.
void nal_clock(int64_t offset_us);
// 1 for a message type of the actors add-on that the server sends.
int nal_is_from_server(uint16_t type);
// Queues one such frame's payload for the game side. Network thread.
void nal_from_server(uint16_t type, const uint8_t *payload, size_t len);
// Sends what the core queued, oldest first, through send (an ow_* struct of that type); stops at the first send that
// fails and returns its error, 0 when the queue is empty. Network thread.
int nal_send_out(int (*send)(uint16_t type, const void *msg));
// The connection ended (lost or closed): the game side drops its copies on its next tick. Network thread.
void nal_lost(void);
// Once per game frame: hands over what arrived and steps the core. player_pos (NULL when unknown) gives the herd cell
// the local player stands in (na_set_place). Game side.
void nal_tick(const float *player_pos);
// Tests: the adapter nal_tick uses instead of na_game() (NULL: back to na_game()).
void nal_use(const NaGame *game);

typedef struct {
    uint32_t in, out, in_dropped, out_dropped, bad;   // frames through each queue, dropped for space, failed to decode
    int live;                                         // the core is stepping
} NalStats;
void nal_stats(NalStats *out);

#ifdef __cplusplus
}
#endif
