/* The puppets add-on on the server: each game's own state (self_state) relayed to the games that can see it, as
 * player_state (docs/online-wire.md, "Features and add-ons", bit 1).
 *
 * A state goes to every other live player whose game is in the same scene, both games in the world (playing,
 * paused or dead, not on the title screen or loading), within the room's interest distance of the sender. The
 * distance uses the viewer's latest self_state position, or its 1 Hz position when it has sent no state yet. A
 * sender is relayed at most SV_PUPPET_HZ times a second; extra states are dropped, not queued, since a newer one
 * follows. A state with a position or velocity that is not a finite number is dropped too.
 *
 * The room (sv_room.c) owns the players; it calls in here with a player's slot index and answers sv_room_peer for
 * the viewers. Single-threaded like the room.
 */
#include "sv.h"

#include <math.h>
#include <string.h>

#define SV_PUPPET_HZ 20.0
#define SV_PUPPET_BURST 4.0

static struct {
    int has;               /* a state arrived since the player joined or entered the world */
    float pos[3];
    double tokens;
    uint64_t at;
} S[OW_PLAYERS_MAX];

static int finite3(const float *v) { return isfinite(v[0]) && isfinite(v[1]) && isfinite(v[2]); }

static int in_world(uint8_t state) {
    return state == OW_PLACE_PLAYING || state == OW_PLACE_PAUSED || state == OW_PLACE_DEAD;
}

void sv_puppets_drop(int slot) {
    if (slot >= 0 && slot < OW_PLAYERS_MAX) memset(&S[slot], 0, sizeof S[slot]);
}

void sv_puppets_area(int slot, const ow_place *area) {
    if (slot < 0 || slot >= OW_PLAYERS_MAX) return;
    /* Leaving the world forgets the last position, so a stale one never counts for interest. The rate budget
     * stays, so going in and out of a loading screen cannot refill it. */
    if (!in_world(area->state)) S[slot].has = 0;
}

void sv_puppets_state(int slot, const ow_self_state *m, uint64_t now) {
    if (slot < 0 || slot >= OW_PLAYERS_MAX) return;
    if (!finite3(m->body.pos) || !finite3(m->body.vel)) return;
    if (!S[slot].at) {
        S[slot].tokens = SV_PUPPET_BURST;
        S[slot].at = now;
    }
    S[slot].tokens += (double)(now - S[slot].at) / 1e6 * SV_PUPPET_HZ;
    S[slot].at = now;
    if (S[slot].tokens > SV_PUPPET_BURST) S[slot].tokens = SV_PUPPET_BURST;
    if (S[slot].tokens < 1.0) return;
    S[slot].tokens -= 1.0;

    sv_peer me;
    if (!sv_room_peer(slot, &me) || !in_world(me.area.state)) return;
    memcpy(S[slot].pos, m->body.pos, sizeof S[slot].pos);
    S[slot].has = 1;

    static ow_player_state out;
    out.account = me.account;
    out.time_us = m->time_us;
    out.body = m->body;
    out.comps_count = m->comps_count;
    memcpy(out.comps, m->comps, sizeof out.comps[0] * m->comps_count);

    const float r2 = me.interest_m * me.interest_m;
    for (int i = 0; i < OW_PLAYERS_MAX; i++) {
        if (i == slot) continue;
        sv_peer v;
        if (!sv_room_peer(i, &v) || !v.puppets || !in_world(v.area.state) || v.area.scene != me.area.scene)
            continue;
        const float *vp = S[i].has ? S[i].pos : v.has_pos ? v.pos : NULL;
        if (!vp) continue;
        float dx = vp[0] - m->body.pos[0], dy = vp[1] - m->body.pos[1], dz = vp[2] - m->body.pos[2];
        if (dx * dx + dy * dy + dz * dz > r2) continue;
        sv_room_send(i, OW_T_player_state, &out);
    }
}
