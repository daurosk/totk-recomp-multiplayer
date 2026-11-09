// The puppets add-on inside the client: what net_client.c's network thread hands to net_puppets.c, and what the
// thread takes from it to send. The game's side is net_set_self and net_puppets in net_client.h.
#pragma once
#include <stdint.h>

#include "net/net_client.h"
#include "ow_wire.h"

// ---- network thread
void np_reset(void);                                    // no room: every puppet goes
void np_clock(int64_t offset_us);                       // server clock = local clock + offset
void np_state(const ow_player_state *m, uint64_t local_us);   // a relayed state, with its arrival on the local clock
void np_depart(uint64_t account);
// Names, characters and places from the roster: a puppet whose player is not in the world goes at once.
void np_roster(const ow_roster_row *rows, int n, uint64_t self);
// The local body to send, if any: 1 with m filled (time_us on the server clock) when one is due at local time now.
// in_world: 1 in the world, 0 out of it, -1 when the game never called net_set_self.
int np_self_due(uint64_t local_us, ow_self_state *m, int *in_world);

// ---- present thread: net_set_self and net_puppets of the real client
void np_set_self(const NetSelf *self);
int np_puppets(NetPuppet *out, int max);

// ---- shared with tests: the comps of a state, both ways
void np_comps_put(const NetSelf *in, ow_comp *comps, uint8_t *count);
void np_comps_get(const ow_comp *comps, uint8_t count, NetPuppet *out);
