// Offline lab for actor sync (runtime/net/net_actors.c): two or three invented games and a stub server in one process.
// Every message goes through the real codec (ow_encode, ow_decode) both ways. The stub server's owner policy is the
// simplest that the wire allows (first offer wins, a release frees the actor, the creator owns a spawn at epoch 1); the
// real policy is the server's (branch owner-server), and the cases here only rely on what any policy must do.
//
//     python tests/net_actors_check.py      (builds this with MSYS2 Clang and runs it)
//
// Each case prints ok or FAIL; the exit code is the number of failures.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../runtime/net/net_actors.h"

static int fails;
#define CHECK(c, ...)                                   \
    do {                                                \
        int ok_ = (c);                                  \
        printf("%s ", ok_ ? "ok  " : "FAIL");           \
        printf(__VA_ARGS__);                            \
        printf("\n");                                   \
        fails += !ok_;                                  \
    } while (0)

// ---- an invented game

typedef struct {
    uint64_t handle, map_id;
    uint8_t cls, auto_placed, is_copy, dead, ai, live;
    char name[65];
    ow_body body;
    int32_t life, life_max;
} Act;

typedef struct {
    Act a[256];
    int n;
    uint64_t next_handle;
    NaWorld *w;
    uint64_t account;
    int removed_killed, removed_other;
} Game;

static Act *g_add(Game *g, uint64_t map_id, const char *name, float x, float z, int life) {
    Act *a = &g->a[g->n++];
    memset(a, 0, sizeof *a);
    a->handle = (g->next_handle += 0x40);
    a->map_id = map_id, a->cls = na_class_of(name), a->ai = 1, a->live = 1;
    snprintf(a->name, sizeof a->name, "%s", name);
    a->body.pos[0] = x, a->body.pos[2] = z, a->body.rot[3] = 1;
    a->life = a->life_max = life;
    return a;
}
static Act *g_find(Game *g, uint64_t h) {
    for (int i = 0; i < g->n; i++)
        if (g->a[i].live && g->a[i].handle == h) return &g->a[i];
    return NULL;
}
static int g_count(Game *g, const char *name) {
    int c = 0;
    for (int i = 0; i < g->n; i++) c += g->a[i].live && !strcmp(g->a[i].name, name);
    return c;
}

static int scan(void *ctx, NaSeen *out, int max) {
    Game *g = ctx;
    int k = 0;
    for (int i = 0; i < g->n && k < max; i++) {
        Act *a = &g->a[i];
        if (!a->live) continue;
        NaSeen *s = &out[k++];
        memset(s, 0, sizeof *s);
        s->handle = a->handle, s->map_id = a->map_id, s->cls = a->cls, s->auto_placed = a->auto_placed;
        s->is_copy = a->is_copy, s->dead = a->dead, s->body = a->body, s->life = a->life, s->life_max = a->life_max;
        snprintf(s->name, sizeof s->name, "%s", a->name);
    }
    return k;
}
static uint64_t make_copy(void *ctx, uint8_t cls, const char *name, const ow_body *b) {
    Game *g = ctx;
    Act *a = g_add(g, 0, name, b->pos[0], b->pos[2], 0);
    a->cls = cls, a->is_copy = 1, a->body = *b;
    return a->handle;
}
static void drive(void *ctx, uint64_t h, const ow_body *b, int32_t life, int32_t life_max) {
    Act *a = g_find(ctx, h);
    if (!a) return;
    a->body = *b;
    if (life_max > 0) a->life = life, a->life_max = life_max;
}
static void puppet(void *ctx, uint64_t h, int on) {
    Act *a = g_find(ctx, h);
    if (a) a->ai = !on;
}
static void hit(void *ctx, uint64_t h, const ow_hit_info *i) {
    Act *a = g_find(ctx, h);
    if (!a) return;
    a->life -= i->amount;
    if (a->life <= 0) a->life = 0, a->dead = 1;
}
static void removef(void *ctx, uint64_t h, uint8_t cause) {
    Game *g = ctx;
    Act *a = g_find(g, h);
    if (!a) return;
    a->live = 0;
    if (cause == OW_GONE_KILLED) g->removed_killed++;
    else g->removed_other++;
}
static uint32_t fnv(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}
static const char *name_of(void *ctx, uint32_t h) {
    (void)ctx;
    static const char *names[] = {"Animal_Fox_A", "Animal_Boar_A", "Animal_Insect_A"};
    for (int i = 0; i < 3; i++)
        if (fnv(names[i]) == h) return names[i];
    return NULL;
}

// The game's own frame: AI-run actors move by their velocity.
static void g_frame(Game *g, float dt) {
    for (int i = 0; i < g->n; i++) {
        Act *a = &g->a[i];
        if (!a->live || !a->ai || a->is_copy) continue;
        for (int k = 0; k < 3; k++) a->body.pos[k] += a->body.vel[k] * dt;
    }
}

// A player of this game hits an actor: the game's damage path asks net_actors first.
static void g_player_hits(Game *g, Act *a, int amount) {
    ow_hit_info i = {.amount = amount, .source = OW_HIT_WEAPON};
    if (na_local_hit(g->w, a->handle, &i)) return;
    hit(g, a->handle, &i);
}

// ---- the stub server

#define GAMES 3
static Game G[GAMES];
static int ngames;

typedef struct { int from; uint16_t type; size_t len; uint8_t *bytes; } Q;
static Q q[20000];
static int nq;
static _Alignas(16) uint8_t outbuf[1 << 20];

static int send_c2s(void *ctx, uint16_t type, const void *msg) {
    size_t n;
    int e = ow_encode(type, msg, outbuf, sizeof outbuf, &n);
    if (e) {
        printf("FAIL encode %#x: %s\n", type, ow_strerror(e));
        fails++;
        return e;
    }
    if (nq >= (int)(sizeof q / sizeof *q)) return -1;
    q[nq].from = (int)(intptr_t)ctx, q[nq].type = type, q[nq].len = n;
    q[nq].bytes = malloc(n ? n : 1);
    memcpy(q[nq].bytes, outbuf, n);
    nq++;
    return 0;
}

static void to_game(int gi, uint16_t type, const void *msg) {
    size_t n;
    static _Alignas(16) uint8_t back[1 << 20];
    if (ow_encode(type, msg, outbuf, sizeof outbuf, &n) || ow_decode(type, outbuf, n, back)) {
        printf("FAIL server encode %#x\n", type);
        fails++;
        return;
    }
    na_on_message(G[gi].w, type, back);
}

typedef struct { ow_aid id; uint64_t owner; uint32_t epoch; uint8_t cls; char name[65]; ow_body body; int used; } SvActor;
static SvActor sv[2048];
static uint64_t herd_leader;
static int sv_drop_poses;   // test switch: the server keeps states back

static SvActor *sv_find(const ow_aid *id, int make) {
    for (int i = 0; i < 2048; i++)
        if (sv[i].used && sv[i].id.origin == id->origin && sv[i].id.serial == id->serial) return &sv[i];
    if (!make) return NULL;
    for (int i = 0; i < 2048; i++)
        if (!sv[i].used) {
            memset(&sv[i], 0, sizeof sv[i]);
            sv[i].used = 1, sv[i].id = *id;
            return &sv[i];
        }
    return NULL;
}
static int game_of(uint64_t account) {
    for (int i = 0; i < ngames; i++)
        if (G[i].account == account) return i;
    return -1;
}
static void sv_owner_all(SvActor *a, uint8_t cause) {
    static ow_owner m;
    memset(&m, 0, sizeof m);
    m.id = a->id, m.class_ = a->cls, m.epoch = a->epoch, m.owner = a->owner, m.cause = cause, m.state_time_us = 1;
    snprintf(m.name, sizeof m.name, "%s", a->name);
    m.body = a->body;
    for (int i = 0; i < ngames; i++) to_game(i, OW_T_owner, &m);
}

static void server(void) {
    static _Alignas(16) uint8_t m[1 << 20];
    for (int qi = 0; qi < nq; qi++) {
        Q *x = &q[qi];
        if (ow_decode(x->type, x->bytes, x->len, m)) {
            printf("FAIL decode %#x\n", x->type);
            fails++;
            continue;
        }
        uint64_t from = G[x->from].account;
        switch (x->type) {
        case OW_T_offer: {
            ow_offer *o = (ow_offer *)m;
            SvActor *a = sv_find(&o->id, 1);
            if (a->owner) break;   // owned: the offer is only a sign the game has it
            a->owner = from, a->epoch++, a->cls = o->class_;
            snprintf(a->name, sizeof a->name, "%s", o->name);
            memcpy(a->body.pos, o->pos, 12);
            sv_owner_all(a, OW_OWNER_OFFER);
            break;
        }
        case OW_T_release: {
            ow_release *r = (ow_release *)m;
            SvActor *a = sv_find(&r->id, 0);
            if (!a || a->owner != from || a->epoch != r->epoch) break;
            a->owner = 0, a->epoch++;
            sv_owner_all(a, OW_OWNER_LEFT);
            break;
        }
        case OW_T_actor_send: {
            ow_actor_send *s = (ow_actor_send *)m;
            if (sv_drop_poses) break;
            static ow_actor_recv r;
            r.source = from, r.time_us = s->time_us, r.items_count = s->items_count;
            memcpy(r.items, s->items, s->items_count * sizeof *s->items);
            for (unsigned i = 0; i < s->items_count; i++) {
                SvActor *a = sv_find(&s->items[i].id, 0);
                if (a && a->owner == from) a->body = s->items[i].body;
            }
            for (int i = 0; i < ngames; i++)
                if (i != x->from) to_game(i, OW_T_actor_recv, &r);
            break;
        }
        case OW_T_hit: {
            ow_hit *h = (ow_hit *)m;
            SvActor *a = sv_find(&h->info.target, 0);
            if (!a || !a->owner || a->epoch != h->info.epoch) break;
            ow_hit_in in = {.attacker = from, .info = h->info};
            int gi = game_of(a->owner);
            if (gi >= 0) to_game(gi, OW_T_hit_in, &in);
            break;
        }
        case OW_T_spawn: {
            ow_spawn *s = (ow_spawn *)m;
            SvActor *a = sv_find(&s->info.id, 1);
            a->owner = from, a->epoch = 1, a->cls = s->info.class_, a->body = s->info.body;
            snprintf(a->name, sizeof a->name, "%s", s->info.name);
            static ow_spawned sp;
            sp.creator = from, sp.info = s->info;
            for (int i = 0; i < ngames; i++)
                if (i != x->from) to_game(i, OW_T_spawned, &sp);
            break;
        }
        case OW_T_gone: {
            ow_gone *g = (ow_gone *)m;
            SvActor *a = sv_find(&g->id, 0);
            if (!a || a->owner != from) break;
            a->used = 0;
            for (int i = 0; i < ngames; i++)
                if (i != x->from) to_game(i, OW_T_gone, g);
            break;
        }
        case OW_T_herd: {
            ow_herd *h = (ow_herd *)m;
            if (from != herd_leader) break;
            h->leader = from;
            for (int i = 0; i < ngames; i++)
                if (i != x->from) to_game(i, OW_T_herd, h);
            break;
        }
        default: break;
        }
        free(x->bytes);
    }
    nq = 0;
}

static NaGame api(Game *g) {
    return (NaGame){g, scan, make_copy, drive, puppet, hit, removef, name_of};
}

static uint64_t now_us;
static void start(int n) {
    for (int i = 0; i < ngames; i++) na_free(G[i].w);
    memset(G, 0, sizeof G);
    memset(sv, 0, sizeof sv);
    nq = 0, ngames = n, now_us = 1000000, herd_leader = 0, sv_drop_poses = 0;
    for (int i = 0; i < n; i++) {
        G[i].next_handle = 0x100000ull * (i + 1);
        NaGame a = api(&G[i]);
        G[i].w = na_new(&a, send_c2s, (void *)(intptr_t)i);
        G[i].account = 11 + (uint64_t)i;
        na_set_self(G[i].w, G[i].account);
    }
}
// Runs the games frame by frame (60 Hz) for secs: game frame, sync step, then the server's turn.
static void run(double secs) {
    for (int f = 0; f < (int)(secs * 60 + 0.5); f++) {
        now_us += 16667;
        for (int i = 0; i < ngames; i++) g_frame(&G[i], 1.0f / 60);
        for (int i = 0; i < ngames; i++) na_step(G[i].w, now_us);
        server();
    }
}
static NaRole role(int gi, uint64_t origin, uint64_t serial) {
    NaInfo i;
    ow_aid id = {origin, serial};
    return na_info(G[gi].w, &id, &i) ? i.role : (NaRole)-1;
}

// ---- cases

static void case_objects(void) {
    start(2);
    Act *ta = g_add(&G[0], 100, "Obj_Tree_A", 10, 10, 30), *tb = g_add(&G[1], 100, "Obj_Tree_A", 10, 10, 30);
    g_add(&G[0], 101, "Obj_Rock_B", 20, 10, 40), g_add(&G[1], 101, "Obj_Rock_B", 20, 10, 40);
    run(1);
    int owners = (role(0, 0, 100) == NA_OWNED) + (role(1, 0, 100) == NA_OWNED);
    CHECK(owners == 1, "objects: a tree loaded in two games has one owner (%d)", owners);
    int ia = role(0, 0, 100) == NA_OWNED ? 0 : 1, ib = 1 - ia;
    Act *own = ia == 0 ? ta : tb, *cp = ia == 0 ? tb : ta;
    CHECK(own->ai && !cp->ai, "objects: the owner runs it, the other shows a copy");
    for (int k = 0; k < 4; k++) g_player_hits(&G[ib], cp, 5), run(0.1);
    run(0.5);
    CHECK(own->life == 10 && cp->life == 10, "objects: hits on the copy reach the owner and count (life %d and %d)",
          own->life, cp->life);
    g_player_hits(&G[ia], own, 5), run(0.5);
    CHECK(own->life == 5 && cp->life == 5, "objects: the owner's own hit shows on the copy (%d, %d)", own->life, cp->life);
    g_player_hits(&G[ib], cp, 10), run(0.5);
    CHECK(own->dead && !cp->live && G[ib].removed_killed == 1, "objects: felled on the owner, gone by its death on the copy");
    CHECK(role(ib, 0, 100) == (NaRole)-1 && role(ia, 0, 100) == (NaRole)-1, "objects: both games forget the felled tree");
    CHECK(na_count(G[0].w, NA_OWNED) + na_count(G[1].w, NA_OWNED) == 1, "objects: the rock still has one owner");
}

static void case_camp(void) {
    start(2);
    Act *e[2][3];
    for (int i = 0; i < 2; i++)
        for (int k = 0; k < 3; k++) e[i][k] = g_add(&G[i], 200 + k, "Enemy_Bokoblin_Junior", 50 + 3 * k, 50, 13);
    run(0.5);
    for (int k = 0; k < 3; k++) {
        int o0 = role(0, 0, 200 + k) == NA_OWNED, o1 = role(1, 0, 200 + k) == NA_OWNED;
        CHECK(o0 + o1 == 1, "camp: member %d has one owner", k);
        Act *own = e[o0 ? 0 : 1][k];
        own->body.vel[0] = 2;   // it runs at the players
    }
    run(2);
    for (int k = 0; k < 3; k++) {
        int o = role(0, 0, 200 + k) == NA_OWNED ? 0 : 1;
        Act *own = e[o][k], *cp = e[1 - o][k];
        own->body.vel[0] = 0;
        float d = fabsf(own->body.pos[0] - cp->body.pos[0]);
        CHECK(d < 0.6f, "camp: member %d's copy follows its owner (%.2f m apart while running)", k, d);
    }
    for (int k = 0; k < 3; k++) {
        int o = role(0, 0, 200 + k) == NA_OWNED ? 0 : 1;
        g_player_hits(&G[1 - o], e[1 - o][k], 4), g_player_hits(&G[o], e[o][k], 4);
    }
    run(1);
    int same = 1;
    for (int k = 0; k < 3; k++) same &= e[0][k]->life == 5 && e[1][k]->life == 5;
    CHECK(same, "camp: hits from both screens count once each, life equal on both screens");
}

static void case_handover(void) {
    start(2);
    Act *a = g_add(&G[0], 300, "Enemy_Moriblin_Junior", 0, 0, 30);
    run(0.5);
    Act *b = g_add(&G[1], 300, "Enemy_Moriblin_Junior", 0, 0, 30);
    run(0.5);
    CHECK(role(0, 0, 300) == NA_OWNED && role(1, 0, 300) == NA_COPY && !b->ai, "hand-over: the first game owns, the second copies");
    g_player_hits(&G[1], b, 10), run(0.5);
    a->live = 0;   // game 0 walks away: the actor unloads there
    run(4);
    CHECK(role(1, 0, 300) == NA_OWNED && b->ai, "hand-over: the game that still has it takes it over and runs it");
    CHECK(b->life == 20, "hand-over: the life it had goes with it (%d)", b->life);
}

static void case_summons(void) {
    start(2);
    g_add(&G[0], 400, "Enemy_Giant_Boss", 0, 0, 800), g_add(&G[1], 400, "Enemy_Giant_Boss", 0, 0, 800);
    run(0.5);
    int o = role(0, 0, 400) == NA_OWNED ? 0 : 1;
    g_add(&G[o], 0, "Enemy_Keese", 3, 0, 1);   // the boss's summons, on its owner
    run(1);
    CHECK(g_count(&G[1 - o], "Enemy_Keese") == 0, "summons: not shared before the wait");
    run(2);
    CHECK(g_count(&G[1 - o], "Enemy_Keese") == 1 && g_count(&G[o], "Enemy_Keese") == 1,
          "summons: after the wait one body on each screen (%d and %d)", g_count(&G[o], "Enemy_Keese"),
          g_count(&G[1 - o], "Enemy_Keese"));
    // both games made the same summons: the later one waits longer and takes the other's copy instead
    start(2);
    na_on_message(G[1].w, OW_T_herd, &(ow_herd){.leader = G[0].account});   // game 0 leads: game 1 is a follower
    g_add(&G[0], 0, "Enemy_Lizalfos", 5, 5, 40);
    run(0.5);
    g_add(&G[1], 0, "Enemy_Lizalfos", 6, 5, 40);
    run(6);
    CHECK(g_count(&G[0], "Enemy_Lizalfos") == 1 && g_count(&G[1], "Enemy_Lizalfos") == 1,
          "summons made twice: one body each (%d and %d)", g_count(&G[0], "Enemy_Lizalfos"), g_count(&G[1], "Enemy_Lizalfos"));
}

static void case_projectiles(void) {
    start(2);
    g_add(&G[0], 500, "Enemy_Bokoblin_Bow", 0, 0, 13);
    run(0.3);
    g_add(&G[1], 500, "Enemy_Bokoblin_Bow", 0, 0, 13);
    run(0.5);
    g_add(&G[0], 0, "Arrow_Enemy", 1, 0, 0);   // the owner's archer shoots
    run(0.3);
    CHECK(g_count(&G[1], "Arrow_Enemy") == 1, "shots: the owner's shot shows on the other screen");
    g_add(&G[1], 0, "Arrow_Bomb", 1, 1, 0);   // a shot appears beside the copy (fired by it): it stays local
    run(0.3);
    CHECK(g_count(&G[0], "Arrow_Bomb") == 0, "shots: a shot beside a copy is not shared");
    g_add(&G[1], 0, "Arrow_Fire", 100, 100, 0);   // the player's own shot, no foe near
    run(0.3);
    CHECK(g_count(&G[0], "Arrow_Fire") == 1, "shots: a player's shot is shared");
}

static void case_wildlife(void) {
    start(2);
    herd_leader = G[0].account;
    na_on_message(G[0].w, OW_T_herd, &(ow_herd){.leader = G[0].account});
    Act *fox = g_add(&G[0], 0, "Animal_Fox_A", 30, 30, 10);
    fox->auto_placed = 1, fox->body.vel[2] = 1;
    run(1);
    CHECK(g_count(&G[1], "Animal_Fox_A") == 1, "wildlife: the leader's animal shows on the follower");
    float p[3] = {32, 0, 31};
    CHECK(!na_allow_auto_place(G[1].w, OW_CLASS_ANIMAL, p), "wildlife: the follower may not place animals near the leader");
    float far[3] = {900, 0, 900};
    CHECK(na_allow_auto_place(G[1].w, OW_CLASS_ANIMAL, far), "wildlife: ... but may far from it");
    CHECK(na_allow_auto_place(G[0].w, OW_CLASS_ANIMAL, p), "wildlife: the leader places as usual");
    run(6);   // what a follower placed in its first seconds as one was there before: wait that out
    Act *slip = g_add(&G[1], 0, "Animal_Boar_A", 31, 31, 10);
    slip->auto_placed = 1;
    run(1);
    CHECK(!slip->live, "wildlife: an animal that slipped past the refusal on the follower is removed");
    int n = 0;
    for (int i = 0; i < G[1].n; i++) n += G[1].a[i].live && G[1].a[i].is_copy && !strcmp(G[1].a[i].name, "Animal_Fox_A");
    CHECK(n == 1, "wildlife: the follower keeps one copy of the fox, not one per herd line (%d)", n);
}

static void case_stale_offline(void) {
    start(2);
    g_add(&G[0], 600, "Enemy_Chuchu", 0, 0, 5);
    Act *b = g_add(&G[1], 600, "Enemy_Chuchu", 0, 0, 5);
    run(0.5);
    static ow_actor_recv r;
    memset(&r, 0, sizeof r);
    r.source = G[0].account, r.items_count = 1;
    r.items[0].id = (ow_aid){0, 600}, r.items[0].epoch = 0, r.items[0].body.pos[0] = 99;
    uint32_t stale = na_stats(G[1].w)->stale;
    to_game(1, OW_T_actor_recv, &r);
    run(0.1);
    CHECK(na_stats(G[1].w)->stale == stale + 1 && b->body.pos[0] < 1, "epochs: a state of an older epoch is dropped");
    g_add(&G[0], 0, "Enemy_Keese_Fire", 40, 40, 1);
    run(3);
    CHECK(g_count(&G[1], "Enemy_Keese_Fire") == 1, "offline: a run-time copy exists before the link goes");
    na_offline(G[1].w);
    CHECK(g_count(&G[1], "Enemy_Keese_Fire") == 0 && b->ai, "offline: run-time copies go, placed ones run again here");
}

int main(void) {
    case_objects();
    case_camp();
    case_handover();
    case_summons();
    case_projectiles();
    case_wildlife();
    case_stale_offline();
    printf("%d failure(s)\n", fails);
    return fails;
}
