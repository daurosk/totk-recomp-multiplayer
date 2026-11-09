/* The actors add-on's records, interest and relays (sv_actors.h), applying the owner rules of sv_owner.c.
 *
 * Every game is treated as hostile here as in the room: a message about an actor counts only when the sender may
 * send it (the owner for states and gone, the creator for spawns, a nearby player for hits), under the record's
 * current epoch, for a name the class table shares. Anything else is dropped and counted, never answered with a
 * change.
 */
#include "sv_actors.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NPLAYERS OW_PLAYERS_MAX
#define HASH_SIZE 32768 /* a power of two, twice the records */
#define TAKEN_MAX 4096
#define EVAL_EVERY SVO_SEC
#define LOG_EVERY (60 * SVO_SEC)

typedef struct bucket {
    double tokens;
    uint64_t at;
} bucket;

static int bucket_take(bucket *b, double burst, double rate, uint64_t now) {
    if (!b->at) b->tokens = burst, b->at = now;
    b->tokens += (double)(now - b->at) / 1e6 * rate;
    b->at = now;
    if (b->tokens > burst) b->tokens = burst;
    if (b->tokens < 1.0) return 0;
    b->tokens -= 1.0;
    return 1;
}

typedef struct pl {
    int used, connected, has_pos;
    uint64_t account;
    ow_place place;
    uint32_t scene;
    float pos[3];
    uint64_t paused_since;
    bucket frames, hits;
} pl;

typedef struct rec {
    int used, next; /* next: the hash chain, or the free list */
    ow_aid id;
    uint32_t scene;
    const ow_class_row *row;
    uint8_t class_;
    char name[OW_ACTOR_NAME_MAX + 1];
    uint64_t owner;
    uint32_t epoch;
    uint8_t last_cause, left_by_owner, asleep, moved_once;
    uint64_t last_change, free_since, moved_at, last_hit, hold_until;
    uint64_t state_at; /* server time of the owner's last state, or of the change that made it owner */
    uint64_t holder;
    float pos[3], still_ref[3], first_pos[3];
    ow_body body;
    uint8_t flags, ncomps;
    uint64_t state_time;
    ow_comp *comps;
    uint32_t loaded, interest; /* bits over player slots */
    uint32_t gave_up;          /* games that released or unloaded it since their last offer */
    ow_aid parent;             /* a spawn's parent, zero for none */
    int leader;                /* rule 8: the record it follows, -1 for none */
    int group;                 /* rule 11: its group, -1 for none */
} rec;

typedef struct grp {
    int used, explicit_;
    ow_aid id;
    uint32_t scene;
    int n;
    int members[OW_GROUP_MAX];
} grp;

typedef struct taken_entry {
    ow_aid id;
    uint32_t scene;
    uint64_t taker, at;
} taken_entry;

static struct {
    sva_io io;
    float interest_m;
    pl p[NPLAYERS];
    rec *r;
    int heads[HASH_SIZE];
    int free_head, nrec;
    grp g[SVA_GROUPS_MAX];
    taken_entry taken[TAKEN_MAX];
    int taken_next;
    uint64_t next_eval, next_log;
    /* Epochs come from one counter for the room, so an id's epoch never goes back, even when its record is dropped
     * and made again; a spawn starts at 1, below every counted one. */
    uint32_t epochs;
    sva_stats st, st_logged;
} A;

static ow_actor_recv g_recv;
static ow_owner g_owner;

/* ---- helpers ---- */

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...) {
    if (!A.io.log) return;
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    A.io.log(A.io.ctx, line);
}

static void send(uint64_t account, uint16_t type, const void *msg) {
    if (A.io.send) A.io.send(A.io.ctx, account, type, msg);
}

static int slot_of(uint64_t account) {
    if (!account) return -1;
    for (int i = 0; i < NPLAYERS; i++)
        if (A.p[i].used && A.p[i].account == account) return i;
    return -1;
}

static float dist3(const float a[3], const float b[3]) {
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

static int aid_eq(const ow_aid *a, const ow_aid *b) { return a->origin == b->origin && a->serial == b->serial; }
static int aid_zero(const ow_aid *a) { return !a->origin && !a->serial; }

/* A placed actor's map id is unique only within its scene, so its key holds the scene; a run-time id is unique. */
static uint32_t key_scene(const ow_aid *id, uint32_t scene) { return id->origin ? 0 : scene; }

static uint32_t hash_of(const ow_aid *id, uint32_t scene) {
    uint64_t h = id->origin * 0x9e3779b97f4a7c15ull ^ id->serial * 0xc2b2ae3d27d4eb4full ^ (uint64_t)scene * 0x165667b1ull;
    h ^= h >> 29;
    return (uint32_t)(h * 0xbf58476d1ce4e5b9ull >> 40) & (HASH_SIZE - 1);
}

static int find(const ow_aid *id, uint32_t scene) {
    uint32_t ks = key_scene(id, scene);
    for (int i = A.heads[hash_of(id, ks)]; i >= 0; i = A.r[i].next)
        if (aid_eq(&A.r[i].id, id) && key_scene(&A.r[i].id, A.r[i].scene) == ks) return i;
    return -1;
}

static int rec_new(const ow_aid *id, uint32_t scene) {
    if (A.free_head < 0) {
        A.st.full++;
        return -1;
    }
    int i = A.free_head;
    rec *r = &A.r[i];
    A.free_head = r->next;
    memset(r, 0, sizeof *r);
    r->used = 1;
    r->id = *id;
    r->scene = scene;
    r->leader = r->group = -1;
    uint32_t h = hash_of(id, key_scene(id, scene));
    r->next = A.heads[h];
    A.heads[h] = i;
    A.nrec++;
    A.st.records = (uint64_t)A.nrec;
    if (A.st.records > A.st.records_peak) A.st.records_peak = A.st.records;
    return i;
}

static void group_drop_member(int gi, int ri);

static void rec_free(int i) {
    rec *r = &A.r[i];
    if (!r->used) return;
    if (r->group >= 0) group_drop_member(r->group, i);
    uint32_t h = hash_of(&r->id, key_scene(&r->id, r->scene));
    for (int *pp = &A.heads[h]; *pp >= 0; pp = &A.r[*pp].next)
        if (*pp == i) {
            *pp = r->next;
            break;
        }
    free(r->comps);
    for (int k = 0; k < SVA_RECORDS_MAX; k++)
        if (A.r[k].used && A.r[k].leader == i) A.r[k].leader = -1;
    memset(r, 0, sizeof *r);
    r->next = A.free_head;
    A.free_head = i;
    A.nrec--;
    A.st.records = (uint64_t)A.nrec;
}

/* ---- the rules' view of the room ---- */

static int place_away(const pl *p, uint64_t now) {
    if (!p->connected) return 1;
    switch (p->place.state) {
    case OW_PLACE_PLAYING: return 0;
    case OW_PLACE_PAUSED: return now - p->paused_since >= SVO_PAUSE_AWAY;
    default: return 1; /* title, loading, dead */
    }
}

static float player_dist(const pl *p, uint32_t scene, const float at[3]) {
    if (!p->has_pos || p->scene != scene) return SVO_FAR_AWAY;
    return dist3(p->pos, at);
}

/* The games as svo_decide sees them, for an actor in scene at `at` whose loaded and gave-up bits are given. */
static int games_for(uint32_t scene, const float at[3], uint32_t loaded, uint32_t gave_up, uint64_t now,
                     svo_game *out) {
    int n = 0;
    for (int i = 0; i < NPLAYERS; i++) {
        const pl *p = &A.p[i];
        if (!p->used) continue;
        svo_game *g = &out[n++];
        g->account = p->account;
        g->away = (uint8_t)place_away(p, now);
        g->present = (uint8_t)(p->connected && p->place.state == OW_PLACE_PLAYING);
        g->candidate = (uint8_t)((loaded >> i) & 1);
        g->gave_up = (uint8_t)((gave_up >> i) & 1);
        g->dist = player_dist(p, scene, at);
    }
    return n;
}

static void facts_of(const rec *r, svo_actor *a) {
    memset(a, 0, sizeof *a);
    a->class_ = r->class_;
    a->rules = r->row ? r->row->rules : 0;
    a->owner = r->owner;
    a->holder = r->holder;
    a->hold_until = r->hold_until;
    a->last_change = r->last_change;
    a->unowned_since = r->free_since;
    a->moved_at = r->moved_at;
    a->last_hit = r->last_hit;
    a->asleep = r->asleep;
    a->left_by_owner = r->left_by_owner;
}

/* ---- interest and owner messages ---- */

static void owner_msg_of(const rec *r, ow_owner *m) {
    memset(m, 0, sizeof *m);
    m->id = r->id;
    m->class_ = r->class_;
    snprintf(m->name, sizeof m->name, "%s", r->name);
    m->epoch = r->epoch;
    m->owner = r->owner;
    m->cause = r->last_cause;
    m->hold_ms = r->owner ? svo_hold_ms(r->class_, r->row ? r->row->rules : 0, r->last_cause) : 0;
    m->state_time_us = r->state_time;
    m->body = r->body;
    m->flags = r->flags;
    m->comps_count = r->ncomps;
    if (r->ncomps) memcpy(m->comps, r->comps, sizeof(ow_comp) * r->ncomps);
}

static void send_owner_to(const rec *r, uint64_t account) {
    owner_msg_of(r, &g_owner);
    send(account, OW_T_owner, &g_owner);
}

/* Whether player slot i is in r's interest now, with the slack for one already in it. Entering sends r's owner. */
static int interest_update(rec *r, int i, int announce) {
    const pl *p = &A.p[i];
    uint32_t bit = 1u << i;
    int was = (r->interest & bit) != 0, now_in = 0;
    if (p->used && p->connected && p->has_pos && p->scene == r->scene) {
        float lim = A.interest_m + (was ? SVA_INTEREST_SLACK_M : 0.0f);
        now_in = dist3(p->pos, r->pos) <= lim;
    }
    if (now_in) r->interest |= bit;
    else r->interest &= ~bit;
    if (now_in && !was && announce) send_owner_to(r, p->account);
    return now_in;
}

/* Sets r's owner and tells every game in interest, and the old and new owners. */
static void set_owner(int ri, uint64_t to, uint8_t cause, uint64_t now) {
    rec *r = &A.r[ri];
    uint64_t from = r->owner;
    r->owner = to;
    r->epoch = ++A.epochs;
    r->last_cause = cause;
    r->last_change = now;
    r->state_at = now;
    if (!to) r->free_since = now;
    r->left_by_owner = 0;
    A.st.changes[cause <= OW_OWNER_LAST ? cause : 0]++;
    if (A.io.changed) A.io.changed(A.io.ctx, &r->id, r->scene, r->name, from, to, cause, r->epoch, now);
    owner_msg_of(r, &g_owner);
    for (int i = 0; i < NPLAYERS; i++) {
        const pl *p = &A.p[i];
        if (!p->used || !p->connected) continue;
        int in = interest_update(r, i, 0);
        if (in || p->account == to || p->account == from) send(p->account, OW_T_owner, &g_owner);
    }
}

/* ---- groups (rule 11) ---- */

static void group_drop_member(int gi, int ri) {
    grp *g = &A.g[gi];
    for (int k = 0; k < g->n; k++)
        if (g->members[k] == ri) {
            g->members[k] = g->members[--g->n];
            break;
        }
    A.r[ri].group = -1;
    if (!g->n) g->used = 0;
}

static void group_free(int gi) {
    grp *g = &A.g[gi];
    for (int k = 0; k < g->n; k++) A.r[g->members[k]].group = -1;
    memset(g, 0, sizeof *g);
}

static int group_new(void) {
    for (int i = 0; i < SVA_GROUPS_MAX; i++)
        if (!A.g[i].used) {
            memset(&A.g[i], 0, sizeof A.g[i]);
            A.g[i].used = 1;
            return i;
        }
    return -1;
}

static int group_find(const ow_aid *id, uint32_t scene) {
    for (int i = 0; i < SVA_GROUPS_MAX; i++)
        if (A.g[i].used && A.g[i].explicit_ && aid_eq(&A.g[i].id, id) &&
            key_scene(id, A.g[i].scene) == key_scene(id, scene))
            return i;
    return -1;
}

/* A group's facts as one actor at its centre (sv_owner.h): the strictest of its members' rules, its newest changes,
 * the owner most of it has. */
static void group_facts(const grp *g, svo_actor *a, float centre[3], uint32_t *loaded) {
    memset(a, 0, sizeof *a);
    centre[0] = centre[1] = centre[2] = 0;
    *loaded = 0;
    uint32_t all_still = OW_CR_STILL;
    int asleep = 1, best_votes = 0;
    for (int k = 0; k < g->n; k++) {
        const rec *r = &A.r[g->members[k]];
        for (int j = 0; j < 3; j++) centre[j] += r->pos[j] / (float)g->n;
        uint32_t rules = r->row ? r->row->rules : 0;
        a->rules |= rules & ~OW_CR_STILL;
        all_still &= rules;
        asleep &= r->asleep;
        *loaded |= r->loaded;
        if (!k) a->class_ = r->class_;
        if (r->holder) a->holder = r->holder;
        if (r->hold_until > a->hold_until) a->hold_until = r->hold_until;
        if (r->last_change > a->last_change) a->last_change = r->last_change;
        if (r->free_since > a->unowned_since) a->unowned_since = r->free_since;
        if (r->moved_at > a->moved_at) a->moved_at = r->moved_at;
        if (r->last_hit > a->last_hit) a->last_hit = r->last_hit;
        a->left_by_owner |= r->left_by_owner;
        int votes = 0;
        for (int j = 0; j < g->n; j++) votes += A.r[g->members[j]].owner == r->owner;
        if (votes > best_votes) best_votes = votes, a->owner = r->owner;
    }
    a->rules |= all_still;
    a->asleep = (uint8_t)asleep;
}

/* Rebuilds the clusters: cluster rows outside explicit groups, chained within CLUSTER_LINK_M in one scene, kept
 * when one is a core row or two have moved since first seen. */
static void clusters_rebuild(void) {
    for (int i = 0; i < SVA_GROUPS_MAX; i++)
        if (A.g[i].used && !A.g[i].explicit_) group_free(i);
    static int idx[SVA_RECORDS_MAX], comp[SVA_RECORDS_MAX];
    int n = 0;
    for (int i = 0; i < SVA_RECORDS_MAX; i++) {
        rec *r = &A.r[i];
        if (r->used && r->group < 0 && r->row && (r->row->rules & OW_CR_CLUSTER)) idx[n++] = i;
    }
    for (int a = 0; a < n; a++) comp[a] = a;
    for (int a = 0; a < n; a++)
        for (int b = a + 1; b < n; b++) {
            const rec *x = &A.r[idx[a]], *y = &A.r[idx[b]];
            if (x->scene != y->scene || dist3(x->pos, y->pos) > SVO_CLUSTER_LINK_M) continue;
            int ra = a, rb = b;
            while (comp[ra] != ra) ra = comp[ra];
            while (comp[rb] != rb) rb = comp[rb];
            if (ra != rb) comp[rb] = ra;
        }
    for (int a = 0; a < n; a++) {
        int root = a;
        while (comp[root] != root) root = comp[root];
        comp[a] = root;
    }
    for (int a = 0; a < n; a++) {
        if (comp[a] != a) continue;
        int members = 0, core = 0, moved = 0;
        for (int b = 0; b < n; b++)
            if (comp[b] == a) {
                const rec *r = &A.r[idx[b]];
                members++;
                core |= (r->row->rules & OW_CR_CORE) != 0;
                moved += r->moved_once;
            }
        if (members < 2 || (!core && moved < 2)) continue;
        int gi = group_new();
        if (gi < 0) return;
        grp *g = &A.g[gi];
        g->scene = A.r[idx[a]].scene;
        for (int b = 0; b < n && g->n < OW_GROUP_MAX; b++)
            if (comp[b] == a) {
                g->members[g->n++] = idx[b];
                A.r[idx[b]].group = gi;
            }
    }
}

/* ---- followers (rule 8) ---- */

static int begins(const char *name, const char *prefix) { return !strncmp(name, prefix, strlen(prefix)); }

/* The record r follows now, or -1: a spawn's parent, a part's body, a held object's enemy. */
static int leader_of(const rec *r) {
    if (!aid_zero(&r->parent)) {
        int k = find(&r->parent, r->scene);
        if (k >= 0) return k;
    }
    if (!r->row) return -1;
    if (r->row->rules & OW_CR_PART) {
        int best = -1;
        float bd = SVO_PART_NEAR_M;
        for (int k = 0; k < SVA_RECORDS_MAX; k++) {
            const rec *b = &A.r[k];
            if (!b->used || b == r || b->scene != r->scene || !b->row || (b->row->rules & OW_CR_PART)) continue;
            int named = 0;
            for (int j = 0; j < r->row->nparts && !named; j++) named = begins(b->name, r->row->parts[j]);
            float d = named ? dist3(b->pos, r->pos) : SVO_FAR_AWAY;
            if (d <= bd) bd = d, best = k;
        }
        return best;
    }
    if ((r->row->rules & OW_CR_HELD) && !r->holder) {
        int best = -1;
        float bd = SVO_HELD_NEAR_M;
        for (int k = 0; k < SVA_RECORDS_MAX; k++) {
            const rec *b = &A.r[k];
            if (!b->used || b == r || b->scene != r->scene) continue;
            if (b->class_ != OW_CLASS_ENEMY && b->class_ != OW_CLASS_BOSS) continue;
            float d = dist3(b->pos, r->pos);
            if (d <= bd) bd = d, best = k;
        }
        return best;
    }
    return -1;
}

/* ---- deciding ---- */

static void count_kept(int kept) {
    if (kept > SVO_KEPT_NONE && kept < SVO_KEPT_COUNT) A.st.kept[kept]++;
}

/* Rule 8 for one record: 1 when a leader decided (moved it or kept it), 0 when the common rules apply. */
static int follow(int ri, uint64_t now) {
    rec *r = &A.r[ri];
    if (r->holder || r->leader < 0 || (r->row && (r->row->rules & OW_CR_FIXED))) return 0;
    const rec *l = &A.r[r->leader];
    if (!l->used || !l->owner) return 0;
    int s = slot_of(l->owner);
    if (s < 0 || place_away(&A.p[s], now)) return 0;
    if (r->owner == l->owner) return 1;
    /* The leader's owner runs it only when its game has it: a game never gets an actor it cannot run. */
    if (!((r->loaded >> s) & 1)) return 0;
    set_owner(ri, l->owner, l->last_cause, now);
    return 1;
}

static void decide_one(int ri, uint64_t now) {
    rec *r = &A.r[ri];
    if (!r->used || r->group >= 0) return;
    if (follow(ri, now)) return;
    svo_actor a;
    facts_of(r, &a);
    svo_game games[NPLAYERS];
    int n = games_for(r->scene, r->pos, r->loaded, r->gave_up, now, games);
    svo_result res;
    svo_decide(&a, games, n, now, &res);
    if (res.change && res.to != r->owner) {
        set_owner(ri, res.to, res.cause, now);
        /* Its followers move with it at once rather than a second later. */
        for (int k = 0; k < SVA_RECORDS_MAX; k++)
            if (A.r[k].used && A.r[k].leader == ri) follow(k, now);
    } else {
        count_kept(res.kept);
    }
}

static void decide_group(int gi, uint64_t now) {
    grp *g = &A.g[gi];
    if (!g->used || !g->n) return;
    svo_actor a;
    float centre[3];
    uint32_t loaded, gave_up = 0;
    group_facts(g, &a, centre, &loaded);
    for (int k = 0; k < g->n; k++) gave_up |= A.r[g->members[k]].gave_up;
    svo_game games[NPLAYERS];
    int n = games_for(g->scene, centre, loaded, gave_up, now, games);
    svo_result res;
    svo_decide(&a, games, n, now, &res);
    uint64_t to = res.change ? res.to : a.owner;
    uint8_t cause = res.change ? res.cause : OW_OWNER_OFFER;
    if (!res.change) count_kept(res.kept);
    /* Members change together: those not with the group's owner join it (a new member, a split vote). */
    if (!to && !res.change) return;
    for (int k = 0; k < g->n; k++) {
        rec *r = &A.r[g->members[k]];
        if (r->owner == to) continue;
        int s = slot_of(to);
        if (to && s >= 0 && !((r->loaded >> s) & 1)) continue; /* not loaded there yet */
        set_owner(g->members[k], to, res.change ? cause : r->last_cause, now);
    }
}

static void decide_rec(int ri, uint64_t now) {
    if (A.r[ri].group >= 0) decide_group(A.r[ri].group, now);
    else decide_one(ri, now);
}

/* ---- the room ---- */

void sva_init(const sva_io *io, float interest_m) {
    sva_shutdown();
    memset(&A, 0, sizeof A);
    A.io = *io;
    A.interest_m = interest_m > 0 ? interest_m : 250.0f;
    A.r = calloc(SVA_RECORDS_MAX, sizeof *A.r);
    for (int i = 0; i < HASH_SIZE; i++) A.heads[i] = -1;
    A.free_head = -1;
    A.epochs = 1;
    if (A.r)
        for (int i = SVA_RECORDS_MAX - 1; i >= 0; i--) A.r[i].next = A.free_head, A.free_head = i;
}

void sva_shutdown(void) {
    if (!A.r) return;
    for (int i = 0; i < SVA_RECORDS_MAX; i++) free(A.r[i].comps);
    free(A.r);
    A.r = NULL;
}

void sva_player_join(uint64_t account, uint64_t now) {
    (void)now;
    int s = slot_of(account);
    if (s < 0)
        for (int i = 0; i < NPLAYERS && s < 0; i++)
            if (!A.p[i].used) {
                s = i;
                memset(&A.p[i], 0, sizeof A.p[i]);
                A.p[i].used = 1;
                A.p[i].account = account;
            }
    if (s >= 0) A.p[s].connected = 1;
}

void sva_player_away(uint64_t account, uint64_t now) {
    int s = slot_of(account);
    if (s < 0 || !A.r) return;
    A.p[s].connected = 0;
    for (int i = 0; i < SVA_RECORDS_MAX; i++) {
        rec *r = &A.r[i];
        if (!r->used) continue;
        r->interest &= ~(1u << s);
        if (r->owner == account) {
            r->free_since = now;
            decide_rec(i, now);
        }
    }
}

void sva_player_leave(uint64_t account, uint64_t now) {
    int s = slot_of(account);
    if (s < 0 || !A.r) return;
    uint32_t bit = 1u << s;
    A.p[s].used = 0; /* out of the rules' view first, so its actors go to others or to nobody */
    for (int i = 0; i < SVA_RECORDS_MAX; i++) {
        rec *r = &A.r[i];
        if (!r->used) continue;
        r->loaded &= ~bit;
        r->interest &= ~bit;
        r->gave_up &= ~bit;
        if (r->holder == account) r->holder = 0;
        if (r->id.origin == account && !r->loaded) {
            /* Its creator's run-time actor that no other game has: it is gone for everyone. */
            ow_gone m = { .id = r->id, .epoch = r->epoch, .cause = OW_GONE_REMOVED };
            for (int k = 0; k < NPLAYERS; k++)
                if ((r->interest >> k) & 1) send(A.p[k].account, OW_T_gone, &m);
            rec_free(i);
            continue;
        }
        if (r->owner == account) {
            r->free_since = now;
            r->left_by_owner = 1;
            decide_rec(i, now);
            if (r->used && r->owner == account) set_owner(i, 0, OW_OWNER_LEFT, now);
            if (r->used && !r->owner) r->left_by_owner = 1; /* the next owner comes by "owner left" */
        }
        if (r->used && !r->owner && !r->loaded) rec_free(i);
    }
    memset(&A.p[s], 0, sizeof A.p[s]);
}

void sva_player_area(uint64_t account, const ow_place *where, uint64_t now) {
    int s = slot_of(account);
    if (s < 0) return;
    pl *p = &A.p[s];
    if (where->state == OW_PLACE_PAUSED && p->place.state != OW_PLACE_PAUSED) p->paused_since = now;
    int was_away = place_away(p, now);
    p->place = *where;
    p->scene = where->scene;
    if (!was_away && place_away(p, now) && A.r)
        for (int i = 0; i < SVA_RECORDS_MAX; i++)
            if (A.r[i].used && A.r[i].owner == account) {
                A.r[i].free_since = now;
                decide_rec(i, now);
            }
}

void sva_player_pos(uint64_t account, uint32_t scene, const float pos[3], uint64_t now) {
    (void)now;
    int s = slot_of(account);
    if (s < 0) return;
    A.p[s].has_pos = 1;
    A.p[s].scene = scene;
    memcpy(A.p[s].pos, pos, sizeof A.p[s].pos);
}

int sva_frame_ok(uint64_t account, uint64_t now) {
    int s = slot_of(account);
    return s < 0 || bucket_take(&A.p[s].frames, SVA_FRAME_BURST, SVA_FRAME_RATE, now);
}

/* ---- messages ---- */

static int id_allowed(const ow_class_row *row, const ow_aid *id) {
    if (!id->origin) return (row->ids & OW_IDS_MAP) != 0;
    return (row->ids & (OW_IDS_CREATOR | OW_IDS_LEADER)) != 0;
}

static void on_offer(uint64_t from, int s, const ow_offer *m, uint64_t now) {
    A.st.offers++;
    const ow_class_row *row = ow_class_find(m->name);
    if (!row || row->class_ == OW_CLASS_LOCAL) {
        A.st.offers_unshared++;
        return;
    }
    if (!id_allowed(row, &m->id)) {
        A.st.offers_refused++;
        return;
    }
    uint32_t scene = A.p[s].scene;
    int ri = find(&m->id, scene);
    if (ri < 0) {
        /* Someone else's run-time actor that the server does not know is gone already: a late copy. */
        if (m->id.origin && m->id.origin != from) {
            A.st.offers_refused++;
            return;
        }
        ri = rec_new(&m->id, scene);
        if (ri < 0) return;
        rec *r = &A.r[ri];
        r->row = row;
        r->class_ = row->class_;
        snprintf(r->name, sizeof r->name, "%s", m->name);
        memcpy(r->pos, m->pos, sizeof r->pos);
        memcpy(r->still_ref, m->pos, sizeof r->pos);
        memcpy(r->first_pos, m->pos, sizeof r->pos);
        memcpy(r->body.pos, m->pos, sizeof r->pos);
        r->body.rot[3] = 1.0f;
        r->free_since = r->moved_at = now;
    } else if (strcmp(A.r[ri].name, m->name)) {
        A.st.offers_refused++; /* one id, two names: not the same actor */
        return;
    }
    rec *r = &A.r[ri];
    r->loaded |= 1u << s;
    r->gave_up &= ~(1u << s);
    if (r->group < 0) r->leader = leader_of(r);
    uint32_t epoch = r->epoch;
    decide_rec(ri, now);
    /* When nothing changed, the offering game still learns the owner: once on coming into the actor's interest,
     * or now when it offers from outside it (an actor nobody runs included, so it shows it still). */
    if (r->used && r->epoch == epoch && !interest_update(r, s, 1)) send_owner_to(r, from);
}

/* Takes the actor from its owner when that owner gives it up (release, or unloading it). */
static void owner_gives_up(int ri, int s, uint64_t now) {
    rec *r = &A.r[ri];
    r->loaded &= ~(1u << s);
    r->gave_up |= 1u << s;
    r->left_by_owner = 1;
    r->free_since = now;
    uint64_t was = r->owner;
    r->owner = 0; /* for the rules: nobody runs it now */
    svo_actor a;
    facts_of(r, &a);
    svo_game games[NPLAYERS];
    int n = games_for(r->scene, r->pos, r->loaded, r->gave_up, now, games);
    svo_result res;
    svo_decide(&a, games, n, now, &res);
    r->owner = was;
    set_owner(ri, res.change ? res.to : 0, OW_OWNER_LEFT, now);
    r->left_by_owner = r->owner ? 0 : 1;
}

static void on_release(uint64_t from, int s, const ow_release *m, uint64_t now) {
    int ri = find(&m->id, A.p[s].scene);
    if (ri < 0 || A.r[ri].owner != from || A.r[ri].epoch != m->epoch) return;
    owner_gives_up(ri, s, now);
}

static void update_state(rec *r, const ow_actor_pose *it, uint64_t time_us, uint64_t now) {
    r->body = it->body;
    r->flags = it->flags;
    r->asleep = (it->flags & OW_POSE_ASLEEP) != 0;
    r->state_time = time_us;
    r->state_at = now;
    memcpy(r->pos, it->body.pos, sizeof r->pos);
    if (dist3(r->pos, r->still_ref) > SVO_STILL_MOVE_M) {
        memcpy(r->still_ref, r->pos, sizeof r->pos);
        r->moved_at = now;
    }
    if (dist3(r->pos, r->first_pos) >= SVO_CLUSTER_MOVED_M) r->moved_once = 1;
    if (it->comps_count != r->ncomps) {
        ow_comp *c = it->comps_count ? realloc(r->comps, sizeof(ow_comp) * it->comps_count) : NULL;
        if (it->comps_count && !c) return;
        if (!it->comps_count) free(r->comps);
        r->comps = c;
        r->ncomps = it->comps_count;
    }
    if (r->ncomps) memcpy(r->comps, it->comps, sizeof(ow_comp) * r->ncomps);
}

static void on_actor_send(uint64_t from, int s, const ow_actor_send *m, uint64_t now) {
    static int idx[OW_POSE_BATCH];
    uint32_t scene = A.p[s].scene;
    int n = m->items_count;
    for (int k = 0; k < n; k++) {
        const ow_actor_pose *it = &m->items[k];
        int ri = find(&it->id, scene);
        A.st.states++;
        if (ri < 0 || A.r[ri].owner != from || A.r[ri].epoch != it->epoch) {
            A.st.states_dropped++;
            idx[k] = -1;
            continue;
        }
        update_state(&A.r[ri], it, m->time_us, now);
        idx[k] = ri;
    }
    for (int i = 0; i < NPLAYERS; i++) {
        if (i == s || !A.p[i].used || !A.p[i].connected) continue;
        g_recv.source = from;
        g_recv.time_us = m->time_us;
        g_recv.items_count = 0;
        for (int k = 0; k < n; k++)
            if (idx[k] >= 0 && interest_update(&A.r[idx[k]], i, 1)) g_recv.items[g_recv.items_count++] = m->items[k];
        if (g_recv.items_count) {
            A.st.relayed += g_recv.items_count;
            send(A.p[i].account, OW_T_actor_recv, &g_recv);
        }
    }
}

static void set_hold(int ri, uint64_t holder, uint64_t now) {
    rec *r = &A.r[ri];
    r->holder = holder;
    if (!holder) r->hold_until = now + SVO_HOLD_AFTER;
}

static void on_hold(uint64_t from, int s, const ow_hold *m, uint64_t now) {
    uint32_t scene = A.p[s].scene;
    int gi = group_find(&m->id, scene), ri = gi < 0 ? find(&m->id, scene) : -1;
    if (gi < 0 && ri < 0) return;
    if (ri >= 0 && A.r[ri].group >= 0) gi = A.r[ri].group;
    int members[OW_GROUP_MAX], n = 0;
    if (gi >= 0)
        for (int k = 0; k < A.g[gi].n; k++) members[n++] = A.g[gi].members[k];
    else members[n++] = ri;
    A.st.holds++;
    int on = m->on && m->what != OW_HOLD_NONE;
    if (on) {
        for (int k = 0; k < n; k++) {
            uint64_t h = A.r[members[k]].holder;
            if (h && h != from && slot_of(h) >= 0) {
                A.st.holds_refused++;
                send_owner_to(&A.r[members[k]], from); /* the holder keeps it: the asker lets go */
                return;
            }
        }
        for (int k = 0; k < n; k++) set_hold(members[k], from, now);
    } else {
        for (int k = 0; k < n; k++)
            if (A.r[members[k]].holder == from) set_hold(members[k], 0, now);
    }
    decide_rec(members[0], now);
    if (gi < 0) return;
    for (int k = 1; k < n; k++) /* a group decides once; this only reaches members the vote left out */
        if (A.r[members[k]].used && A.r[members[k]].owner != A.r[members[0]].owner) decide_rec(members[k], now);
}

static void on_hit(uint64_t from, int s, const ow_hit *m, uint64_t now) {
    A.st.hits++;
    if (!bucket_take(&A.p[s].hits, SVA_HIT_BURST, SVA_HIT_RATE, now)) {
        A.st.hits_dropped++;
        return;
    }
    int ri = find(&m->info.target, A.p[s].scene);
    if (ri < 0) {
        A.st.hits_dropped++;
        return;
    }
    rec *r = &A.r[ri];
    if (r->epoch != m->info.epoch || !r->owner || r->owner == from ||
        player_dist(&A.p[s], r->scene, r->pos) > SVO_HIT_NEAR_M) {
        A.st.hits_dropped++;
        return;
    }
    r->last_hit = now;
    if (r->row && (r->row->rules & OW_CR_PART) && r->leader >= 0) A.r[r->leader].last_hit = now;
    ow_hit_in h = { .attacker = from, .info = m->info };
    A.st.hits_passed++;
    send(r->owner, OW_T_hit_in, &h);
}

static void on_spawn(uint64_t from, int s, const ow_spawn *m, uint64_t now) {
    const ow_spawn_info *in = &m->info;
    A.st.spawns++;
    const ow_class_row *row = ow_class_find(in->name);
    uint32_t scene = A.p[s].scene;
    int refuse = !row || row->class_ == OW_CLASS_LOCAL || in->id.origin != from || !(row->ids & OW_IDS_CREATOR) ||
                 find(&in->id, scene) >= 0;
    if (!refuse && !aid_zero(&in->parent)) {
        int pi = find(&in->parent, scene);
        refuse = pi >= 0 && A.r[pi].owner != from; /* a copy's summons or drop */
    }
    if (refuse) {
        A.st.spawns_refused++;
        if (row && row->class_ != OW_CLASS_LOCAL && in->id.origin == from && find(&in->id, scene) < 0) {
            ow_gone g = { .id = in->id, .epoch = 0, .cause = OW_GONE_REMOVED };
            send(from, OW_T_gone, &g);
        }
        return;
    }
    int ri = rec_new(&in->id, scene);
    if (ri < 0) {
        ow_gone g = { .id = in->id, .epoch = 0, .cause = OW_GONE_REMOVED };
        send(from, OW_T_gone, &g);
        return;
    }
    rec *r = &A.r[ri];
    r->row = row;
    r->class_ = row->class_;
    snprintf(r->name, sizeof r->name, "%s", in->name);
    r->parent = in->parent;
    r->owner = from;
    r->epoch = 1;
    r->last_cause = OW_OWNER_OFFER;
    r->last_change = r->state_at = now;
    r->free_since = r->moved_at = now;
    r->loaded = 1u << s;
    r->body = in->body;
    memcpy(r->pos, in->body.pos, sizeof r->pos);
    memcpy(r->still_ref, r->pos, sizeof r->pos);
    memcpy(r->first_pos, r->pos, sizeof r->pos);
    if (in->comps_count) {
        r->comps = malloc(sizeof(ow_comp) * in->comps_count);
        if (r->comps) {
            memcpy(r->comps, in->comps, sizeof(ow_comp) * in->comps_count);
            r->ncomps = in->comps_count;
        }
    }
    r->leader = leader_of(r);
    ow_spawned sp = { .creator = from, .info = *in };
    for (int i = 0; i < NPLAYERS; i++) {
        if (!A.p[i].used || !A.p[i].connected) continue;
        if (interest_update(r, i, 0) && i != s) send(A.p[i].account, OW_T_spawned, &sp);
    }
}

static void tell_gone(rec *r, uint8_t cause, int except) {
    ow_gone m = { .id = r->id, .epoch = r->epoch, .cause = cause };
    for (int i = 0; i < NPLAYERS; i++)
        if (i != except && ((r->interest >> i) & 1)) send(A.p[i].account, OW_T_gone, &m);
}

static void on_gone(uint64_t from, int s, const ow_gone *m, uint64_t now) {
    int ri = find(&m->id, A.p[s].scene);
    if (ri < 0) return;
    rec *r = &A.r[ri];
    A.st.gone++;
    if (m->cause == OW_GONE_UNLOADED) {
        if (r->owner == from) owner_gives_up(ri, s, now);
        else r->loaded &= ~(1u << s), r->gave_up |= 1u << s;
        if (!r->owner && !r->loaded) rec_free(ri);
        return;
    }
    if (r->owner != from || r->epoch != m->epoch) return; /* only the owner ends an actor */
    tell_gone(r, m->cause, s);
    rec_free(ri);
}

static void on_group(uint64_t from, int s, const ow_group *m, uint64_t now) {
    uint32_t scene = A.p[s].scene;
    int gi = group_find(&m->id, scene);
    if (gi >= 0) {
        /* Only the owner of a group's members changes it. */
        if (A.g[gi].n && A.r[A.g[gi].members[0]].owner != from) return;
        group_free(gi);
    }
    if (!m->members_count) return;
    gi = group_new();
    if (gi < 0) return;
    grp *g = &A.g[gi];
    g->explicit_ = 1;
    g->id = m->id;
    g->scene = scene;
    for (int k = 0; k < m->members_count && g->n < OW_GROUP_MAX; k++) {
        int ri = find(&m->members[k], scene);
        if (ri < 0 || A.r[ri].owner != from) continue;
        if (A.r[ri].group >= 0) group_drop_member(A.r[ri].group, ri);
        g->members[g->n++] = ri;
        A.r[ri].group = gi;
    }
    if (!g->n) {
        g->used = 0;
        return;
    }
    decide_group(gi, now);
}

static const taken_entry *taken_find(const ow_aid *id, uint32_t scene, uint64_t now) {
    for (int i = 0; i < TAKEN_MAX; i++) {
        const taken_entry *t = &A.taken[i];
        if (t->taker && aid_eq(&t->id, id) && key_scene(id, t->scene) == key_scene(id, scene) &&
            now - t->at < SVA_TAKEN_KEEP)
            return t;
    }
    return NULL;
}

static void on_take(uint64_t from, int s, const ow_take *m, uint64_t now) {
    uint32_t scene = A.p[s].scene;
    A.st.takes++;
    ow_taken t = { .id = m->id, .taker = from, .yours = 1 };
    const taken_entry *before = taken_find(&m->id, scene, now);
    if (before) {
        t.taker = before->taker;
        t.yours = before->taker == from;
        send(from, OW_T_taken, &t);
        return;
    }
    int ri = find(&m->id, scene);
    taken_entry *e = &A.taken[A.taken_next];
    A.taken_next = (A.taken_next + 1) % TAKEN_MAX;
    e->id = m->id;
    e->scene = scene;
    e->taker = from;
    e->at = now;
    A.st.takes_won++;
    send(from, OW_T_taken, &t);
    if (ri < 0) return; /* nobody else knows it */
    rec *r = &A.r[ri];
    t.yours = 0;
    for (int i = 0; i < NPLAYERS; i++)
        if (i != s && (((r->interest | r->loaded) >> i) & 1)) send(A.p[i].account, OW_T_taken, &t);
    rec_free(ri);
}

void sva_message(uint64_t account, uint16_t type, const void *msg, uint64_t now) {
    int s = slot_of(account);
    if (s < 0 || !A.r) return;
    switch (type) {
    case OW_T_offer: on_offer(account, s, msg, now); break;
    case OW_T_release: on_release(account, s, msg, now); break;
    case OW_T_actor_send: on_actor_send(account, s, msg, now); break;
    case OW_T_hold: on_hold(account, s, msg, now); break;
    case OW_T_hit: on_hit(account, s, msg, now); break;
    case OW_T_spawn: on_spawn(account, s, msg, now); break;
    case OW_T_gone: on_gone(account, s, msg, now); break;
    case OW_T_group: on_group(account, s, msg, now); break;
    case OW_T_take: on_take(account, s, msg, now); break;
    default: break; /* herd: the wildlife leader rule is not built yet */
    }
}

/* ---- the second's pass ---- */

static void log_minute(void) {
    sva_stats *a = &A.st, *b = &A.st_logged;
    uint64_t changes = 0, kept = 0;
    for (int c = 0; c <= OW_OWNER_LAST; c++) changes += a->changes[c] - b->changes[c];
    for (int k = 1; k < SVO_KEPT_COUNT; k++) kept += a->kept[k] - b->kept[k];
    if (changes || a->offers != b->offers || a->hits != b->hits) {
        char line[400];
        int n = snprintf(line, sizeof line, "actors: %llu records, %llu owner changes this minute",
                         (unsigned long long)a->records, (unsigned long long)changes);
        for (int c = 0; c <= OW_OWNER_LAST && n < (int)sizeof line; c++)
            if (a->changes[c] != b->changes[c])
                n += snprintf(line + n, sizeof line - (size_t)n, ", %s %llu", svo_cause_name((uint8_t)c),
                              (unsigned long long)(a->changes[c] - b->changes[c]));
        if (n < (int)sizeof line)
            snprintf(line + n, sizeof line - (size_t)n, "; held back %llu; hits %llu passed, %llu dropped",
                     (unsigned long long)kept, (unsigned long long)(a->hits_passed - b->hits_passed),
                     (unsigned long long)(a->hits_dropped - b->hits_dropped));
        logf_("%s", line);
    }
    *b = *a;
}

void sva_tick(uint64_t now) {
    if (!A.r) return;
    if (!A.next_log) A.next_log = now + LOG_EVERY;
    if (now >= A.next_log) {
        A.next_log = now + LOG_EVERY;
        log_minute();
    }
    if (now < A.next_eval) return;
    A.next_eval = now + EVAL_EVERY;
    for (int i = 0; i < SVA_RECORDS_MAX; i++) {
        rec *r = &A.r[i];
        if (!r->used) continue;
        for (int k = 0; k < NPLAYERS; k++)
            if (A.p[k].used) interest_update(r, k, 1);
        if (r->group < 0) r->leader = leader_of(r);
    }
    /* Rule 9: an owner that is playing but sends no state of an actor has stopped running it. */
    for (int i = 0; i < SVA_RECORDS_MAX; i++) {
        rec *r = &A.r[i];
        if (!r->used || !r->owner || now - r->state_at < SVO_OWNER_SILENT) continue;
        int s = slot_of(r->owner);
        if (s >= 0 && !place_away(&A.p[s], now) && A.p[s].place.state == OW_PLACE_PLAYING) {
            A.st.silent++;
            owner_gives_up(i, s, now);
            if (!r->owner && !r->loaded) rec_free(i);
        }
    }
    clusters_rebuild();
    for (int i = 0; i < SVA_GROUPS_MAX; i++)
        if (A.g[i].used) decide_group(i, now);
    for (int i = 0; i < SVA_RECORDS_MAX; i++)
        if (A.r[i].used && A.r[i].group < 0) decide_one(i, now);
}

const sva_stats *sva_get_stats(void) { return &A.st; }

int sva_owner_of(uint32_t scene, const ow_aid *id, uint64_t *owner, uint32_t *epoch) {
    if (!A.r) return 0;
    int ri = find(id, scene);
    if (ri < 0) return 0;
    if (owner) *owner = A.r[ri].owner;
    if (epoch) *epoch = A.r[ri].epoch;
    return 1;
}
