// Actor sync, the client side (net_actors.h). Pure: the game is reached only through NaGame, the server only through
// NaSend and na_on_message, so tests/net_actors_lab.c runs every rule here with invented games and a stub server.
//
// The life of an actor in one world:
//   LOCAL    seen, not shared: a class the room does not share, or nobody is online.
//   OFFERED  a placed actor this game runs and offered to own; it keeps running here until the server answers.
//   OWNED    the server named this game owner: its AI runs here and its state goes out every 100 to 250 ms.
//   COPY     another game owns it: its AI is off here (NaGame.puppet) and the owner's states drive it.
// An owner message moves an actor between OWNED and COPY at any time (a hand-over), carrying the last full state.
//
// Behaviour follows the old online version's rules (P13 docs online-native-m3b.md and m5.md, re-expressed):
// - Spawn rule. A run-time enemy that a placement manager made is one game's: a follower's game is refused the placement
//   in the leader's zone (na_allow_auto_place), and one that slipped through is removed. A run-time enemy without that
//   mark (a summons) waits for another game's copy, LEADER_WAIT_S on the leader and FOLLOWER_WAIT_S on a follower, so
//   two games that both made it do not both share it; a copy of its name within SUMMON_M then replaces it, else it is
//   shared. A shot is shared at once when the nearest foe within SHOT_M is one this game runs, and stays local when it
//   is a copy (the owner's game fires the real one).
// - Wildlife leader rule. While players stand together the server names one leader per cell (herd.leader). Within
//   ZONE_M of the leader's spot only the leader's game places animals; it sends them as herd rows each HERD_S, and
//   every other game shows copies of those rows. What a game had before it became a follower stays its own.
#include "net_actors.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define POSE_MOVING_US 100000   // state interval of a moving owned actor
#define POSE_STILL_US 250000    // ... and of a still one: the wire's floor (docs/online-wire.md, actor_send)
#define STILL_V 0.05f           // m/s under which an actor counts as still
#define OFFER_RETRY_US 3000000
#define COPY_FORGET_US 15000000 // a copy with no state for this long and no actor here is forgotten
#define EXTRAPOLATE_US 500000   // copies are moved on by their velocity for at most this long past a state
#define LEADER_WAIT_S 2.5
#define FOLLOWER_WAIT_S 5.0
#define SUMMON_M 25.0f
#define SHOT_M 30.0f
#define ZONE_M 90.0f
#define PRE_US 6000000          // first seen this soon after becoming a follower: it was there before
#define HERD_S 0.25
#define HERD_FORGET_US 3000000  // a herd row not named again for this long is gone
#define TABLE 2048              // power of two, at least 2 * NA_MAX

typedef struct {
    ow_aid id;
    uint8_t cls, role, auto_placed, seen, ours_made, herd;   // herd: a copy made for a herd row
    char name[OW_ACTOR_NAME_MAX + 1];
    uint64_t handle, owner;
    uint32_t epoch;
    ow_body body;            // last known state (own reading when OWNED, the owner's when COPY)
    int32_t life, life_max;
    uint64_t t_state, t_sent, t_offer, t_first, t_seen;
} Ent;

typedef struct {         // a run-time actor this game made, waiting for the spawn rule
    uint64_t handle;
    uint64_t t_first;
    int alive;
} Pending;

struct NaWorld {
    NaGame g;
    NaSend send;
    void *send_ctx;
    uint64_t self, now, counter;
    Ent e[NA_MAX];
    int n;
    int16_t by_id[TABLE], by_handle[TABLE];
    Pending pend[256];
    int npend;
    NaSeen seen[NA_SEEN_MAX];
    // wildlife leader
    uint64_t leader;
    float leader_pos[3];
    int have_leader_pos;
    uint64_t t_follower, t_herd;
    uint32_t herd_cell;
    NaStats st;
};

// ---- helpers

static uint32_t hash_u64(uint64_t x) {
    x ^= x >> 33, x *= 0xff51afd7ed558ccdull, x ^= x >> 33, x *= 0xc4ceb9fe1a85ec53ull, x ^= x >> 33;
    return (uint32_t)x;
}
static uint32_t hash_aid(const ow_aid *a) { return hash_u64(a->origin * 0x9e3779b97f4a7c15ull ^ a->serial); }
static int aid_eq(const ow_aid *a, const ow_aid *b) { return a->origin == b->origin && a->serial == b->serial; }
static float dist2(const float a[3], const float b[3]) {
    float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return x * x + y * y + z * z;
}

// FNV-1a over the name's bytes: the herd rows' name hash (docs/online-actors.md, "Open questions").
static uint32_t name_hash(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}

static void reindex(NaWorld *w) {
    memset(w->by_id, -1, sizeof w->by_id);
    memset(w->by_handle, -1, sizeof w->by_handle);
    for (int i = 0; i < w->n; i++) {
        uint32_t h = hash_aid(&w->e[i].id) & (TABLE - 1);
        while (w->by_id[h] >= 0) h = (h + 1) & (TABLE - 1);
        w->by_id[h] = (int16_t)i;
        if (w->e[i].handle) {
            h = hash_u64(w->e[i].handle) & (TABLE - 1);
            while (w->by_handle[h] >= 0) h = (h + 1) & (TABLE - 1);
            w->by_handle[h] = (int16_t)i;
        }
    }
}

static Ent *find_id(NaWorld *w, const ow_aid *id) {
    for (uint32_t h = hash_aid(id) & (TABLE - 1); w->by_id[h] >= 0; h = (h + 1) & (TABLE - 1))
        if (aid_eq(&w->e[w->by_id[h]].id, id)) return &w->e[w->by_id[h]];
    return NULL;
}
static const Ent *find_id_c(const NaWorld *w, const ow_aid *id) { return find_id((NaWorld *)w, id); }

static Ent *find_handle(NaWorld *w, uint64_t handle) {
    if (!handle) return NULL;
    for (uint32_t h = hash_u64(handle) & (TABLE - 1); w->by_handle[h] >= 0; h = (h + 1) & (TABLE - 1))
        if (w->e[w->by_handle[h]].handle == handle) return &w->e[w->by_handle[h]];
    return NULL;
}

static void set_handle(NaWorld *w, Ent *e, uint64_t handle) {
    e->handle = handle;
    if (!handle) return reindex(w);   // a cleared handle leaves a stale slot: lay the table out again
    uint32_t h = hash_u64(handle) & (TABLE - 1);
    while (w->by_handle[h] >= 0) h = (h + 1) & (TABLE - 1);
    w->by_handle[h] = (int16_t)(e - w->e);
}

static Ent *add(NaWorld *w, const ow_aid *id, uint8_t cls, const char *name) {
    if (w->n >= NA_MAX) return NULL;
    Ent *e = &w->e[w->n];
    memset(e, 0, sizeof *e);
    e->id = *id, e->cls = cls;
    strncpy(e->name, name ? name : "", OW_ACTOR_NAME_MAX);
    e->t_first = e->t_seen = w->now;
    uint32_t h = hash_aid(id) & (TABLE - 1);
    while (w->by_id[h] >= 0) h = (h + 1) & (TABLE - 1);
    w->by_id[h] = (int16_t)w->n++;
    return e;
}

static void drop(NaWorld *w, Ent *e) {
    *e = w->e[--w->n];
    reindex(w);
}

static int shared_class(uint8_t cls) {
    return cls <= OW_CLASS_TOWNSPERSON || cls == OW_CLASS_PROJECTILE || cls == OW_CLASS_DEVICE;
}

static void put_life(ow_comp *comps, uint8_t *count, int32_t life, int32_t life_max) {
    if (life_max <= 0 || *count >= OW_COMPS_MAX) return;
    ow_c_life l = {.life = life, .life_max = life_max};
    if (!ow_comp_put(&comps[*count], OW_C_life, &l)) (*count)++;
}

static int get_life(const ow_comp *comps, unsigned count, int32_t *life, int32_t *life_max) {
    for (unsigned i = 0; i < count; i++)
        if (comps[i].kind == OW_C_life) {
            ow_c_life l;
            if (ow_comp_get(&comps[i], &l)) return 0;
            *life = l.life, *life_max = l.life_max;
            return 1;
        }
    return 0;
}

static int still(const ow_body *b) {
    return fabsf(b->vel[0]) < STILL_V && fabsf(b->vel[1]) < STILL_V && fabsf(b->vel[2]) < STILL_V;
}

// ---- roles

static void become_copy(NaWorld *w, Ent *e) {
    if (e->role != NA_COPY) w->st.copies++;
    e->role = NA_COPY;
    if (e->handle) w->g.puppet(w->g.ctx, e->handle, 1);
}

static void become_owner(NaWorld *w, Ent *e) {
    if (e->role != NA_OWNED) w->st.owned++;
    int was_copy = e->role == NA_COPY;
    e->role = NA_OWNED;
    e->owner = w->self;
    e->t_sent = 0;
    if (!e->handle) return;
    if (was_copy) w->g.drive(w->g.ctx, e->handle, &e->body, e->life, e->life_max);   // start from the last state
    w->g.puppet(w->g.ctx, e->handle, 0);
}

// ---- the world

NaWorld *na_new(const NaGame *game, NaSend send, void *send_ctx) {
    NaWorld *w = calloc(1, sizeof *w);
    if (!w) return NULL;
    w->g = *game, w->send = send, w->send_ctx = send_ctx;
    reindex(w);
    return w;
}

void na_free(NaWorld *w) { free(w); }
void na_set_self(NaWorld *w, uint64_t account) { w->self = account; }
const NaStats *na_stats(const NaWorld *w) { return &w->st; }

static int is_follower(const NaWorld *w, const float pos[3]) {
    return w->leader && w->leader != w->self && w->have_leader_pos && dist2(pos, w->leader_pos) < ZONE_M * ZONE_M;
}
static int is_leader(const NaWorld *w) { return !w->leader || w->leader == w->self; }

int na_allow_auto_place(NaWorld *w, uint8_t cls, const float pos[3]) {
    if (!w->self || (cls != OW_CLASS_ANIMAL && cls != OW_CLASS_ENEMY)) return 1;
    if (!is_follower(w, pos)) return 1;
    w->st.refused_spawns++;
    return 0;
}

static void offer(NaWorld *w, Ent *e) {
    ow_offer m = {.id = e->id, .class_ = e->cls};
    strncpy(m.name, e->name, OW_ACTOR_NAME_MAX);
    memcpy(m.pos, e->body.pos, sizeof m.pos);
    if (!w->send(w->send_ctx, OW_T_offer, &m)) w->st.offers++;
    e->t_offer = w->now;
}

static void send_gone(NaWorld *w, Ent *e, uint8_t cause) {
    ow_gone m = {.id = e->id, .epoch = e->epoch, .cause = cause};
    if (!w->send(w->send_ctx, OW_T_gone, &m)) w->st.gones_out++;
}

// The nearest followed foe (enemy or boss) within r of pos, or NULL.
static const Ent *nearest_foe(const NaWorld *w, const float pos[3], float r) {
    const Ent *best = NULL;
    float bd = r * r;
    for (int i = 0; i < w->n; i++) {
        const Ent *e = &w->e[i];
        if (e->cls != OW_CLASS_ENEMY && e->cls != OW_CLASS_BOSS) continue;
        float d = dist2(e->body.pos, pos);
        if (d < bd) bd = d, best = e;
    }
    return best;
}

static void share_spawn(NaWorld *w, const NaSeen *s) {
    ow_spawn m = {0};
    m.info.id.origin = w->self, m.info.id.serial = ++w->counter;
    m.info.class_ = s->cls;
    strncpy(m.info.name, s->name, OW_ACTOR_NAME_MAX);
    m.info.body = s->body;
    put_life(m.info.comps, &m.info.comps_count, s->life, s->life_max);
    Ent *e = add(w, &m.info.id, s->cls, s->name);
    if (!e) return;
    if (w->send(w->send_ctx, OW_T_spawn, &m)) {
        drop(w, e);
        return;
    }
    w->st.spawns++;
    e->seen = 1, e->body = s->body, e->life = s->life, e->life_max = s->life_max, e->epoch = 1, e->t_state = w->now;
    set_handle(w, e, s->handle);
    become_owner(w, e);
}

// The spawn rule for one run-time actor of this game. Returns 1 when it is decided (shared, removed or kept local).
static int spawn_rule(NaWorld *w, const NaSeen *s, uint64_t t_first) {
    double age = (double)(w->now - t_first) * 1e-6;
    if (s->cls == OW_CLASS_PROJECTILE) {
        const Ent *foe = nearest_foe(w, s->body.pos, SHOT_M);
        if (!foe || foe->role == NA_OWNED || foe->role == NA_LOCAL) share_spawn(w, s);
        return 1;   // fired by a copy: the owner's game shares the real one, this one stays here
    }
    if (s->cls != OW_CLASS_ENEMY && s->cls != OW_CLASS_BOSS && s->cls != OW_CLASS_ANIMAL) return 1;
    if (s->auto_placed) {
        if (is_follower(w, s->body.pos)) {
            if (t_first >= w->t_follower + PRE_US) {   // slipped past the refusal: the leader's game has it
                w->g.remove(w->g.ctx, s->handle, OW_GONE_REMOVED);
                w->st.removed++;
            }
            return 1;   // there before the players met: stays this game's own, unshared
        }
        if (s->cls == OW_CLASS_ANIMAL) return 1;   // the leader's animals go as herd rows
        share_spawn(w, s);
        return 1;
    }
    // a summons: wait for another game's copy
    for (int i = 0; i < w->n; i++) {
        Ent *c = &w->e[i];
        if (c->role == NA_COPY && !strcmp(c->name, s->name) && dist2(c->body.pos, s->body.pos) < SUMMON_M * SUMMON_M &&
            c->t_first + 1000000 >= t_first) {   // a copy that appeared around the same time: the same summons
            w->g.remove(w->g.ctx, s->handle, OW_GONE_REMOVED);
            w->st.replaced++;
            return 1;
        }
    }
    if (age < (is_leader(w) ? LEADER_WAIT_S : FOLLOWER_WAIT_S)) return 0;
    share_spawn(w, s);
    return 1;
}

static Pending *pending_of(NaWorld *w, uint64_t handle) {
    for (int i = 0; i < w->npend; i++)
        if (w->pend[i].handle == handle) return &w->pend[i];
    if (w->npend >= (int)(sizeof w->pend / sizeof *w->pend)) return NULL;
    Pending *p = &w->pend[w->npend++];
    p->handle = handle, p->t_first = w->now, p->alive = 1;
    return p;
}

static void send_poses(NaWorld *w) {
    static ow_actor_send m;
    m.time_us = w->now, m.items_count = 0;
    for (int i = 0; i < w->n; i++) {
        Ent *e = &w->e[i];
        if (e->role != NA_OWNED || !e->handle) continue;
        uint64_t every = still(&e->body) ? POSE_STILL_US : POSE_MOVING_US;
        if (e->t_sent && w->now < e->t_sent + every) continue;
        ow_actor_pose *p = &m.items[m.items_count++];
        memset(p, 0, sizeof *p);
        p->id = e->id, p->epoch = e->epoch, p->body = e->body, p->flags = still(&e->body) ? OW_POSE_STILL : 0;
        put_life(p->comps, &p->comps_count, e->life, e->life_max);
        e->t_sent = w->now;
        if (m.items_count == OW_POSE_BATCH) {
            if (!w->send(w->send_ctx, OW_T_actor_send, &m)) w->st.poses += m.items_count;
            m.items_count = 0;
        }
    }
    if (m.items_count && !w->send(w->send_ctx, OW_T_actor_send, &m)) w->st.poses += m.items_count;
}

static void drive_copies(NaWorld *w) {
    for (int i = 0; i < w->n; i++) {
        Ent *e = &w->e[i];
        if (e->role != NA_COPY || !e->handle || !e->t_state) continue;
        ow_body b = e->body;
        uint64_t dt = w->now > e->t_state ? w->now - e->t_state : 0;
        if (dt > EXTRAPOLATE_US) dt = EXTRAPOLATE_US;
        for (int k = 0; k < 3; k++) b.pos[k] += b.vel[k] * (float)dt * 1e-6f;
        w->g.drive(w->g.ctx, e->handle, &b, e->life, e->life_max);
    }
}

// The leader's animals, as herd rows of the cell it stands in.
static void send_herd(NaWorld *w) {
    if (!w->self || w->leader != w->self || (double)(w->now - w->t_herd) * 1e-6 < HERD_S) return;
    w->t_herd = w->now;
    static ow_herd m;
    memset(&m, 0, sizeof m);
    m.cell = w->herd_cell;
    for (int i = 0; i < NA_SEEN_MAX && m.beasts_count < OW_HERD_MAX; i++) {
        const NaSeen *s = &w->seen[i];
        if (!s->handle) break;
        if (s->cls != OW_CLASS_ANIMAL || !s->auto_placed || s->is_copy || s->dead) continue;
        if (w->have_leader_pos && dist2(s->body.pos, w->leader_pos) > ZONE_M * ZONE_M) continue;
        ow_herd_row *r = &m.beasts[m.beasts_count++];
        r->local = (uint32_t)(s->handle >> 4);   // stable while the animal lives
        r->name_hash = name_hash(s->name);
        memcpy(r->pos, s->body.pos, 12);
        memcpy(r->vel, s->body.vel, 12);
        r->yaw = 2.0f * atan2f(s->body.rot[1], s->body.rot[3]);
    }
    if (!w->send(w->send_ctx, OW_T_herd, &m)) w->st.herds_out++;
}

void na_step(NaWorld *w, uint64_t now_us) {
    w->now = now_us;
    int ns = w->g.scan(w->g.ctx, w->seen, NA_SEEN_MAX);
    if (ns < NA_SEEN_MAX) w->seen[ns].handle = 0;
    for (int i = 0; i < w->n; i++) w->e[i].seen = 0;
    for (int i = 0; i < w->npend; i++) w->pend[i].alive = 0;

    for (int i = 0; i < ns; i++) {
        const NaSeen *s = &w->seen[i];
        Ent *e = find_handle(w, s->handle);
        if (!e && s->map_id) {
            ow_aid id = {0, s->map_id};
            e = find_id(w, &id);
            if (e && !e->handle) {   // a placed actor this world knew of before it loaded here
                set_handle(w, e, s->handle);
                if (e->role == NA_COPY) become_copy(w, e);   // stops its AI: the owner runs it
            }
        }
        if (e) {
            e->seen = 1, e->t_seen = w->now;
            if (e->role == NA_COPY) continue;   // the owner's states are what counts
            e->body = s->body, e->life = s->life, e->life_max = s->life_max;
            if (s->dead && e->role == NA_OWNED) {
                send_gone(w, e, OW_GONE_KILLED);
                drop(w, e);
            } else if (e->role == NA_OFFERED && w->now >= e->t_offer + OFFER_RETRY_US && w->self) {
                offer(w, e);
            } else if (e->role == NA_LOCAL && w->self && shared_class(e->cls)) {
                e->role = NA_OFFERED;
                offer(w, e);
            }
            continue;
        }
        if (s->is_copy || s->dead || !shared_class(s->cls) || !w->self) continue;
        if (s->map_id) {
            ow_aid id = {0, s->map_id};
            e = add(w, &id, s->cls, s->name);
            if (!e) continue;
            set_handle(w, e, s->handle);
            e->seen = 1, e->body = s->body, e->life = s->life, e->life_max = s->life_max, e->auto_placed = s->auto_placed;
            e->role = NA_OFFERED;
            offer(w, e);
            continue;
        }
        Pending *p = pending_of(w, s->handle);
        if (!p) continue;
        p->alive = 1;
        if (spawn_rule(w, s, p->t_first)) p->handle = 0;   // decided: forget it
    }

    // what this scan did not see
    for (int i = 0; i < w->n; i++) {
        Ent *e = &w->e[i];
        if (e->seen) continue;
        if (e->handle && (e->role == NA_OWNED || e->role == NA_OFFERED)) {   // unloaded or gone in this game
            if (e->role == NA_OWNED) {
                if (e->id.origin) {
                    send_gone(w, e, OW_GONE_UNLOADED);
                } else {
                    ow_release m = {.id = e->id, .epoch = e->epoch};
                    if (!w->send(w->send_ctx, OW_T_release, &m)) w->st.releases++;
                }
            }
            drop(w, e), i--;
        } else if (e->role == NA_COPY && e->handle) {   // our copy went (the game unloaded it): make it again later
            set_handle(w, e, 0);
        } else if (e->role == NA_COPY && w->now > e->t_state + COPY_FORGET_US) {
            drop(w, e), i--;
        }
    }
    for (int i = 0; i < w->npend; i++)
        if (!w->pend[i].alive || !w->pend[i].handle) w->pend[i--] = w->pend[--w->npend];

    // copies that should stand here and do not yet
    for (int i = 0; i < w->n; i++) {
        Ent *e = &w->e[i];
        if (e->role != NA_COPY || e->handle || !e->t_state) continue;
        if (e->id.origin == 0) continue;   // placed actors come with their map: the game loads them
        if (e->herd && w->now > e->t_state + HERD_FORGET_US) continue;
        uint64_t h = w->g.make_copy(w->g.ctx, e->cls, e->name, &e->body);
        if (h) {
            set_handle(w, e, h);
            w->g.puppet(w->g.ctx, h, 1);
            w->st.made++;
        }
    }
    send_poses(w);
    drive_copies(w);
    send_herd(w);
}

// ---- from the server

static void on_owner(NaWorld *w, const ow_owner *m) {
    Ent *e = find_id(w, &m->id);
    if (e && m->epoch < e->epoch) {
        w->st.stale++;
        return;
    }
    if (!e) e = add(w, &m->id, m->class_, m->name);
    if (!e) return;
    if (e->role == NA_OWNED || e->role == NA_COPY) w->st.handovers++;
    e->epoch = m->epoch, e->owner = m->owner;
    if (m->state_time_us) {
        e->body = m->body, e->t_state = m->state_time_us;
        get_life(m->comps, m->comps_count, &e->life, &e->life_max);
    }
    if (m->owner && m->owner == w->self) become_owner(w, e);
    else if (m->owner) become_copy(w, e);
    else if (e->handle) {   // nobody owns it: the game that has it loaded runs it until someone offers again
        e->role = NA_LOCAL;
        w->g.puppet(w->g.ctx, e->handle, 0);
    }
}

static void on_recv(NaWorld *w, const ow_actor_recv *m) {
    for (unsigned i = 0; i < m->items_count; i++) {
        const ow_actor_pose *p = &m->items[i];
        Ent *e = find_id(w, &p->id);
        if (!e || e->role != NA_COPY) continue;   // an actor this game does not follow, or a late state after a hand-over
        if (p->epoch != e->epoch) {
            w->st.stale++;
            continue;
        }
        e->body = p->body, e->t_state = m->time_us ? m->time_us : w->now;
        get_life(p->comps, p->comps_count, &e->life, &e->life_max);
    }
}

static void on_spawned(NaWorld *w, const ow_spawned *m) {
    if (m->info.id.origin == w->self) return;
    Ent *e = find_id(w, &m->info.id);
    if (e) return;
    e = add(w, &m->info.id, m->info.class_, m->info.name);
    if (!e) return;
    w->st.spawned_in++;
    e->owner = m->creator, e->epoch = 1, e->body = m->info.body, e->t_state = w->now;
    get_life(m->info.comps, m->info.comps_count, &e->life, &e->life_max);
    e->role = NA_COPY;
    w->st.copies++;
}

static void on_gone(NaWorld *w, const ow_gone *m) {
    Ent *e = find_id(w, &m->id);
    if (!e) return;
    if (m->epoch < e->epoch) {
        w->st.stale++;
        return;
    }
    w->st.gones_in++;
    // a copy here goes the way the owner's did: killed things die (their loot is the owner's), the rest is removed
    if (e->handle && m->cause != OW_GONE_UNLOADED) {
        w->g.remove(w->g.ctx, e->handle, m->cause);
        w->st.removed++;
    } else if (e->handle && e->id.origin) {
        w->g.remove(w->g.ctx, e->handle, OW_GONE_UNLOADED);
    } else if (e->handle) {
        e->role = NA_LOCAL;   // a placed actor nobody runs now: this game takes it back at its next offer
        w->g.puppet(w->g.ctx, e->handle, 0);
        return;
    }
    drop(w, e);
}

static void on_hit_in(NaWorld *w, const ow_hit_in *m) {
    Ent *e = find_id(w, &m->info.target);
    if (!e || e->role != NA_OWNED || !e->handle) return;   // no longer ours: the hit is lost, as a late one would be
    w->st.hits_in++;
    w->g.hit(w->g.ctx, e->handle, &m->info);
}

static void on_herd(NaWorld *w, const ow_herd *m) {
    if (m->leader != w->leader) w->t_follower = w->now;
    w->leader = m->leader;
    if (m->leader == w->self || !m->leader) return;
    // the leader's spot: the middle of its animals (a herd always stands round its player)
    float c[3] = {0};
    for (unsigned i = 0; i < m->beasts_count; i++)
        for (int k = 0; k < 3; k++) c[k] += m->beasts[i].pos[k] / (float)m->beasts_count;
    if (m->beasts_count) memcpy(w->leader_pos, c, sizeof c), w->have_leader_pos = 1;
    for (unsigned i = 0; i < m->beasts_count; i++) {
        const ow_herd_row *r = &m->beasts[i];
        ow_aid id = {m->leader, 0x8000000000000000ull | r->local};   // herd ids never meet a spawn counter
        Ent *e = find_id(w, &id);
        if (!e) {
            const char *name = w->g.name_of ? w->g.name_of(w->g.ctx, r->name_hash) : NULL;
            if (!name) continue;   // an animal this game cannot name
            e = add(w, &id, OW_CLASS_ANIMAL, name);
            if (!e) return;
            e->role = NA_COPY, e->herd = 1, e->owner = m->leader;
            w->st.copies++;
        }
        memcpy(e->body.pos, r->pos, 12);
        memcpy(e->body.vel, r->vel, 12);
        e->body.rot[0] = e->body.rot[2] = 0, e->body.rot[1] = sinf(r->yaw * 0.5f), e->body.rot[3] = cosf(r->yaw * 0.5f);
        e->t_state = w->now;
    }
}

void na_set_place(NaWorld *w, uint32_t cell) { w->herd_cell = cell; }

void na_on_message(NaWorld *w, uint16_t type, const void *msg) {
    switch (type) {
    case OW_T_owner: on_owner(w, msg); break;
    case OW_T_actor_recv: on_recv(w, msg); break;
    case OW_T_spawned: on_spawned(w, msg); break;
    case OW_T_gone: on_gone(w, msg); break;
    case OW_T_hit_in: on_hit_in(w, msg); break;
    case OW_T_herd: on_herd(w, msg); break;
    default: break;
    }
}

int na_local_hit(NaWorld *w, uint64_t handle, const ow_hit_info *hit) {
    Ent *e = find_handle(w, handle);
    if (!e || e->role != NA_COPY) return 0;
    ow_hit m = {.info = *hit};
    m.info.target = e->id, m.info.epoch = e->epoch;
    if (!w->send(w->send_ctx, OW_T_hit, &m)) w->st.hits_out++;
    return 1;
}

void na_offline(NaWorld *w) {
    for (int i = 0; i < w->n; i++) {
        Ent *e = &w->e[i];
        if (!e->handle) continue;
        if (e->role == NA_COPY && e->id.origin) w->g.remove(w->g.ctx, e->handle, OW_GONE_UNLOADED);
        else w->g.puppet(w->g.ctx, e->handle, 0);
    }
    w->n = 0, w->npend = 0, w->leader = 0, w->have_leader_pos = 0;
    reindex(w);
}

int na_info(const NaWorld *w, const ow_aid *id, NaInfo *out) {
    const Ent *e = find_id_c(w, id);
    if (!e) return 0;
    *out = (NaInfo){e->id, (NaRole)e->role, e->epoch, e->owner, e->handle, e->life, e->life_max, e->cls};
    return 1;
}

int na_count(const NaWorld *w, NaRole role) {
    int c = 0;
    for (int i = 0; i < w->n; i++) c += w->e[i].role == role;
    return c;
}

// ---- classes by name

uint8_t na_class_of(const char *n) {
    static const struct { const char *prefix; uint8_t cls; } rows[] = {
        {"Enemy_Giant", OW_CLASS_BOSS},      {"Enemy_Lynel", OW_CLASS_BOSS},   {"Enemy_Golem", OW_CLASS_BOSS},
        {"Enemy_Drake", OW_CLASS_BOSS},      {"Enemy_Mogurudo", OW_CLASS_BOSS},{"Enemy_SandWorm", OW_CLASS_BOSS},
        {"Enemy_", OW_CLASS_ENEMY},          {"Animal_", OW_CLASS_ANIMAL},     {"Npc_", OW_CLASS_TOWNSPERSON},
        {"Obj_Tree", OW_CLASS_MAP_OBJECT},   {"Obj_Rock", OW_CLASS_MAP_OBJECT},{"Obj_Ore", OW_CLASS_MAP_OBJECT},
        {"Obj_", OW_CLASS_WORLD_OBJECT},     {"Item_", OW_CLASS_ITEM},         {"Weapon_", OW_CLASS_ITEM},
        {"Bullet_", OW_CLASS_PROJECTILE},    {"Arrow_", OW_CLASS_PROJECTILE},
    };
    for (size_t i = 0; i < sizeof rows / sizeof *rows; i++)
        if (!strncmp(n, rows[i].prefix, strlen(rows[i].prefix))) return rows[i].cls;
    return OW_CLASS_LAST + 1;
}
