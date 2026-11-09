// The puppets add-on in the client (net/net_puppets.h, net/net_client.h "puppets").
//
// Sending: the game hands its body to net_set_self each frame; the network thread takes it at most every
// NP_SEND_US (20 a second) through np_self_due, and at least every NP_KEEP_US while the body is still, so the others
// never take a still player for gone (NET_PUPPET_STALE_MS). Its time is the local clock plus the server-clock offset
// net_client.c measures from pings.
//
// Receiving: each relayed state goes into its player's smoother (net/net_smooth.c), timed by the sender's server-clock
// stamp, and its components (animation, glide) into a ring beside it. net_puppets evaluates every smoother at the
// local server-clock estimate. A puppet fades in over NP_FADE_US from its first state; past NET_PUPPET_STALE_MS
// without a state it fades out and goes, and at once when its player leaves or the roster shows it out of the world.
//
// One mutex, P.mx, held only for copies and the evaluation of at most OW_PLAYERS_MAX smoothers: the present thread
// (net_set_self, net_puppets) and the network thread (everything else) both take it.
#include "net/net_puppets.h"

#include <math.h>
#include <string.h>

#include "net/net_smooth.h"
#include "platform/platform.h"

#define NP_SEND_US 50000ull
#define NP_KEEP_US 200000ull
#define NP_STILL_MPS 0.05f
#define NP_FADE_US 250000.0f
#define NP_MAX OW_PLAYERS_MAX

typedef struct {
    int nanim;
    NetAnimSlot anim[NET_ANIM_SLOTS];
    uint8_t gliding;
    float glide_lean;
    uint64_t t;
} Extra;

typedef struct {
    uint64_t account;
    char name[NET_NAME_MAX];
    uint8_t zelda;
    NetSmooth sm;
    Extra ex[NS_RING];
    int next_tag;
    uint64_t first_us, last_us;     // local clock: first and latest state
    uint64_t render_t, delay;       // of the last net_puppets: the render time (server clock) and the delay
} Pup;

static struct {
    PlatMutex mx;
    int64_t offset;
    int have_offset;
    Pup p[NP_MAX];
    int n;
    struct { uint64_t account; char name[NET_NAME_MAX]; uint8_t zelda, in_world; } roster[OW_PLAYERS_MAX];
    int nroster;
    NetSelf self;
    int have_self;
    uint64_t sent_us;
    float sent_pos[3];
} P;

static uint64_t local_us(void) { return plat_now_ns() / 1000u; }

static int in_world_state(uint8_t s) { return s == OW_PLACE_PLAYING || s == OW_PLACE_PAUSED || s == OW_PLACE_DEAD; }

// Copies a name, cut at a UTF-8 character boundary when it does not fit.
static void name_copy(char *dst, size_t n, const char *src) {
    size_t k = 0;
    for (; src[k] && k + 1 < n; k++) dst[k] = src[k];
    if (((unsigned char)src[k] & 0xC0) == 0x80) {   // cut inside a character: drop the part that fitted
        while (k > 0 && ((unsigned char)dst[k - 1] & 0xC0) == 0x80) k--;
        if (k > 0) k--;
    }
    dst[k] = 0;
}

static void drop_at(int i) {
    P.p[i] = P.p[--P.n];
}

// ---- components

void np_comps_put(const NetSelf *in, ow_comp *comps, uint8_t *count) {
    uint8_t n = 0;
    ow_c_anim a;
    memset(&a, 0, sizeof a);
    a.slots_count = (uint8_t)(in->nanim < 0 ? 0 : in->nanim > NET_ANIM_SLOTS ? NET_ANIM_SLOTS : in->nanim);
    for (int i = 0; i < a.slots_count; i++) {
        a.slots[i].layer = in->anim[i].layer;
        a.slots[i].anim = in->anim[i].anim;
        a.slots[i].frame = isfinite(in->anim[i].frame) ? in->anim[i].frame : 0;
        a.slots[i].rate = isfinite(in->anim[i].rate) ? in->anim[i].rate : 0;
    }
    if (a.slots_count && !ow_comp_put(&comps[n], OW_C_anim, &a)) n++;
    if (in->gliding) {
        ow_c_glide g = {.on = 1, .lean = isfinite(in->glide_lean) ? in->glide_lean : 0};
        if (!ow_comp_put(&comps[n], OW_C_glide, &g)) n++;
    }
    *count = n;
}

void np_comps_get(const ow_comp *comps, uint8_t count, NetPuppet *out) {
    out->nanim = 0;
    out->gliding = 0;
    out->glide_lean = 0;
    for (int i = 0; i < count; i++) {
        if (comps[i].kind == OW_C_anim) {
            ow_c_anim a;
            if (ow_comp_get(&comps[i], &a)) continue;
            out->nanim = a.slots_count;
            for (int k = 0; k < a.slots_count; k++) {
                out->anim[k].layer = a.slots[k].layer;
                out->anim[k].anim = a.slots[k].anim;
                out->anim[k].frame = a.slots[k].frame;
                out->anim[k].rate = a.slots[k].rate;
            }
        } else if (comps[i].kind == OW_C_glide) {
            ow_c_glide g;
            if (ow_comp_get(&comps[i], &g)) continue;
            out->gliding = g.on;
            out->glide_lean = g.lean;
        }
    }
}

// ---- network thread

void np_reset(void) {
    plat_mutex_lock(&P.mx);
    P.n = 0;
    P.nroster = 0;
    P.have_offset = 0;
    plat_mutex_unlock(&P.mx);
}

void np_clock(int64_t offset_us) {
    plat_mutex_lock(&P.mx);
    P.offset = offset_us;
    P.have_offset = 1;
    plat_mutex_unlock(&P.mx);
}

static int find(uint64_t account) {
    for (int i = 0; i < P.n; i++)
        if (P.p[i].account == account) return i;
    return -1;
}

void np_state(const ow_player_state *m, uint64_t now) {
    plat_mutex_lock(&P.mx);
    int r = -1;
    for (int k = 0; k < P.nroster; k++)
        if (P.roster[k].account == m->account) r = k;
    int i = find(m->account);
    if (i < 0 && (r < 0 || !P.roster[r].in_world || P.n == NP_MAX || !P.have_offset)) {
        plat_mutex_unlock(&P.mx);
        return;
    }
    if (i < 0) {
        i = P.n++;
        Pup *q = &P.p[i];
        memset(q, 0, sizeof *q);
        q->account = m->account;
        ns_init(&q->sm);
        q->first_us = now;
    }
    Pup *q = &P.p[i];
    if (r >= 0) {
        name_copy(q->name, sizeof q->name, P.roster[r].name);
        q->zelda = P.roster[r].zelda;
    }
    NsSample s;
    memset(&s, 0, sizeof s);
    s.t = m->time_us;
    memcpy(s.p, m->body.pos, sizeof s.p);
    memcpy(s.v, m->body.vel, sizeof s.v);
    memcpy(s.q, m->body.rot, sizeof s.q);
    float qn = s.q[0] * s.q[0] + s.q[1] * s.q[1] + s.q[2] * s.q[2] + s.q[3] * s.q[3];
    if (!(qn > 1e-6f)) s.q[0] = s.q[1] = s.q[2] = 0, s.q[3] = 1;
    s.tag = q->next_tag;
    if (ns_push(&q->sm, &s, now + (uint64_t)P.offset)) {
        NetPuppet tmp;
        np_comps_get(m->comps, m->comps_count, &tmp);
        Extra *e = &q->ex[q->next_tag];
        e->nanim = tmp.nanim;
        memcpy(e->anim, tmp.anim, sizeof e->anim);
        e->gliding = tmp.gliding;
        e->glide_lean = tmp.glide_lean;
        e->t = m->time_us;
        q->next_tag = (q->next_tag + 1) % NS_RING;
        q->last_us = now;
    }
    plat_mutex_unlock(&P.mx);
}

void np_depart(uint64_t account) {
    plat_mutex_lock(&P.mx);
    int i = find(account);
    if (i >= 0) drop_at(i);
    plat_mutex_unlock(&P.mx);
}

void np_roster(const ow_roster_row *rows, int n, uint64_t self) {
    plat_mutex_lock(&P.mx);
    P.nroster = 0;
    for (int k = 0; k < n && P.nroster < OW_PLAYERS_MAX; k++) {
        if (rows[k].account == self) continue;
        int j = P.nroster++;
        P.roster[j].account = rows[k].account;
        name_copy(P.roster[j].name, sizeof P.roster[j].name, rows[k].nickname);
        P.roster[j].zelda = rows[k].character == OW_CHAR_ZELDA;
        P.roster[j].in_world = !rows[k].afk && in_world_state(rows[k].area.state);
    }
    for (int i = P.n - 1; i >= 0; i--) {
        int keep = 0;
        for (int k = 0; k < P.nroster; k++)
            if (P.roster[k].account == P.p[i].account) {
                keep = P.roster[k].in_world;
                name_copy(P.p[i].name, sizeof P.p[i].name, P.roster[k].name);
                P.p[i].zelda = P.roster[k].zelda;
            }
        if (!keep) drop_at(i);
    }
    plat_mutex_unlock(&P.mx);
}

int np_self_due(uint64_t now, ow_self_state *m, int *in_world) {
    plat_mutex_lock(&P.mx);
    *in_world = P.have_self ? P.self.in_world != 0 : -1;
    int due = 0;
    if (*in_world == 1 && P.have_offset && now >= P.sent_us + NP_SEND_US) {
        const NetSelf *s = &P.self;
        float sp = sqrtf(s->vel[0] * s->vel[0] + s->vel[1] * s->vel[1] + s->vel[2] * s->vel[2]);
        float dx = s->pos[0] - P.sent_pos[0], dy = s->pos[1] - P.sent_pos[1], dz = s->pos[2] - P.sent_pos[2];
        int still = sp < NP_STILL_MPS && dx * dx + dy * dy + dz * dz < 1e-4f && s->nanim == 0;
        due = !still || now >= P.sent_us + NP_KEEP_US;
    }
    if (due) {
        memset(m, 0, sizeof *m);
        m->time_us = (uint64_t)((int64_t)now + P.offset);
        memcpy(m->body.pos, P.self.pos, sizeof m->body.pos);
        memcpy(m->body.vel, P.self.vel, sizeof m->body.vel);
        memcpy(m->body.rot, P.self.rot, sizeof m->body.rot);
        np_comps_put(&P.self, m->comps, &m->comps_count);
        P.sent_us = now;
        memcpy(P.sent_pos, P.self.pos, sizeof P.sent_pos);
    }
    plat_mutex_unlock(&P.mx);
    return due;
}

// ---- the game's side (net_client.h)

static int finite_all(const float *v, int n) {
    for (int i = 0; i < n; i++)
        if (!isfinite(v[i])) return 0;
    return 1;
}

void np_set_self(const NetSelf *self) {
    if (!self) return;
    NetSelf s = *self;
    if (!finite_all(s.pos, 3) || !finite_all(s.vel, 3) || !finite_all(s.rot, 4)) s.in_world = 0;
    if (s.nanim < 0) s.nanim = 0;
    if (s.nanim > NET_ANIM_SLOTS) s.nanim = NET_ANIM_SLOTS;
    plat_mutex_lock(&P.mx);
    P.self = s;
    P.have_self = 1;
    plat_mutex_unlock(&P.mx);
}

int np_puppets(NetPuppet *out, int max) {
    uint64_t now = local_us();
    int n = 0;
    plat_mutex_lock(&P.mx);
    if (!P.have_offset || !P.have_self || !P.self.in_world) {
        plat_mutex_unlock(&P.mx);
        return 0;
    }
    uint64_t server_now = (uint64_t)((int64_t)now + P.offset);
    for (int i = P.n - 1; i >= 0; i--) {
        Pup *q = &P.p[i];
        double silent = (double)(now - q->last_us);
        double stale = NET_PUPPET_STALE_MS * 1000.0;
        if (silent > stale + NP_FADE_US) {
            drop_at(i);
            continue;
        }
        if (n >= max) continue;
        NsOut o;
        if (!ns_eval(&q->sm, server_now, &o)) continue;
        q->render_t = o.render_t;
        q->delay = ns_delay(&q->sm);
        NetPuppet *r = &out[n++];
        memset(r, 0, sizeof *r);
        r->id = (uint32_t)q->account;
        memcpy(r->name, q->name, sizeof r->name);
        r->zelda = q->zelda;
        memcpy(r->pos, o.p, sizeof r->pos);
        memcpy(r->vel, o.v, sizeof r->vel);
        memcpy(r->rot, o.q, sizeof r->rot);
        float x = o.q[0], y = o.q[1], z = o.q[2], w = o.q[3];
        r->yaw = atan2f(2 * (w * y + x * z), 1 - 2 * (x * x + y * y));
        float in = (float)(now - q->first_us) / NP_FADE_US;
        float outf = silent > stale ? 1.0f - (float)((silent - stale) / NP_FADE_US) : 1.0f;
        r->alpha = in < outf ? in : outf;
        if (r->alpha > 1) r->alpha = 1;
        if (r->alpha < 0) r->alpha = 0;
        const Extra *e = &q->ex[o.tag];
        r->nanim = e->nanim;
        float adv = o.render_t > e->t ? (float)((double)(o.render_t - e->t) / 1e6) * 30.0f : 0;   // frames at 30 a second
        for (int k = 0; k < e->nanim; k++) {
            r->anim[k] = e->anim[k];
            r->anim[k].frame += adv * e->anim[k].rate;
        }
        r->gliding = e->gliding;
        r->glide_lean = e->glide_lean;
    }
    plat_mutex_unlock(&P.mx);
    return n;
}

// Tests (tests/net_client_main.c): the render time and the delay of a puppet's last net_puppets, 0 when unknown.
uint64_t net_test_puppet_render(uint32_t id, uint64_t *delay_us) {
    uint64_t t = 0;
    plat_mutex_lock(&P.mx);
    int i = find(id);
    if (i >= 0) {
        t = P.p[i].render_t;
        if (delay_us) *delay_us = P.p[i].delay;
    }
    plat_mutex_unlock(&P.mx);
    return t;
}
