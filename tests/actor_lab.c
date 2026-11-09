/* The replay lab of the actors add-on: bot timelines on a virtual clock, against the server's own library.
 *
 *   actor_lab CASE.txt...
 *
 * Each case file (tests/actor_cases/) is a timeline of players and actors. The lab links server/sv_actors.c and
 * server/sv_owner.c as the server runs them and plays each player's game as a well-behaved client of
 * docs/online-wire.md 9.1 would: it reports its position every 250 ms, offers the shared actors near it and unloads
 * those that fall away, sends the states of the actors it owns under the epoch the server gave it, and applies the
 * owner, spawned, gone and taken messages it receives. Scripted events add what players do (holds, hits, spawns,
 * pauses, loading screens, leaving). Then the case's expectations are checked against the owner changes and the
 * messages each game received. tests/actor_lab.py builds the lab and runs every case.
 *
 * Case file lines (times in seconds, positions x,y,z in metres; # starts a comment):
 *   case NAME TEXT...                     the case and what it reproduces
 *   player P [scene=N] [load=M] [unload=M] [manual]
 *                                         a game; it auto-offers within load (150) and unloads beyond unload (200);
 *                                         manual: it offers and unloads only by script
 *   actor X ORIGIN:SERIAL NAME X,Y,Z [scene=N] [asleep] [late]
 *                                         an actor; late: it exists only from its spawn event
 *   walk P|X T0 T1 X,Y,Z X,Y,Z            a straight line between the times; before and after, the ends
 *   wander P|X T0 T1 X0,Z0 X1,Z1 SEED     walks at 3 m/s between random points of the box, resting 2 to 10 s at each
 *   follow P LEADER DX,DZ DELAY           P walks LEADER's way (its keyframes so far), DX,DZ off it and DELAY s behind
 *   grid PREFIX NAME X0,Z0 DX,DZ NX,NZ SERIAL [scene=N]
 *                                         NX by NZ placed actors PREFIX0, PREFIX1... of game name NAME, map ids from
 *                                         SERIAL up
 *   at T P join|away|leave                the session starts (playing, at its walk's place), its connection is
 *                                         lost (held), it ends
 *   at T P area playing|paused|loading|dead|title [scene=N]
 *   at T P offer X | unload X | release X | take X
 *   at T P hold X ultrahand|carry|ride on|off
 *   at T P hit X [stale] [n=N]
 *   at T P spawn X [parent=Y]             X (declared late) is created by P's game
 *   at T P gone X removed|killed|taken
 *   at T P group G ORIGIN:SERIAL X...     P's game glues the actors into group G (no actors: ends it)
 *   at T P stale X                        a state of X under an old epoch
 *   run T                                 the end of the timeline
 *   expect owner X at T is P|none
 *   expect changes X|all|class=C|flips[:class=C] [in T0..T1] OP N
 *                                         owner changes (OP is <=, >=, ==, <, >); flips: those by rules 2 to 4
 *   expect rate X|all|class=C|flips[:class=C] in T0..T1 OP R
 *                                         owner changes a minute over the window
 *   expect cause X CAUSE OP N                            changes of X by offer, far, closer, pickup, left, hold
 *   expect got P TYPE [X] OP N            messages P's game received: owner, recv (states), hit_in, spawned, gone,
 *                                         taken_yours, taken_not
 *   expect kept NAME OP N                 rules that held a change back (sv_owner.c svo_kept_name, as one word:
 *                                         fight, still, asleep, recent, far_margin, orphan_nearer, orphan_far, held)
 */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../server/sv_actors.h"

#define MAXP 8
#define MAXA 256
#define MAXK 256
#define MAXE 4096
#define MAXX 128
#define MAXC 8192
#define STEP_US 50000ull
#define GAME_US 250000ull
#define US(t) ((uint64_t)((t) * 1e6 + 0.5))

typedef struct key {
    double t;
    float p[3];
} key;

typedef struct track {
    key k[MAXK];
    int n;
} track;

typedef struct gview { /* one game's view of one actor */
    int loaded, known;
    uint64_t owner;
    uint32_t epoch;
    int got[8];
} gview;

enum { G_OWNER, G_RECV, G_HIT_IN, G_SPAWNED, G_GONE, G_TAKEN_YOURS, G_TAKEN_NOT, G_TYPES };
static const char *const GOT_NAMES[G_TYPES] = {"owner", "recv", "hit_in", "spawned", "gone", "taken_yours", "taken_not"};

typedef struct player {
    char name[16];
    uint64_t account;
    uint32_t scene;
    float load, unload;
    int manual, joined, connected;
    track tr;
} player;

typedef struct actor {
    char name[16], game_name[OW_ACTOR_NAME_MAX + 1];
    ow_aid id;
    uint32_t scene;
    int asleep, late, exists;
    uint64_t spawned_by, spawned_at; /* a run-time actor starts owned by its creator, with no owner change */
    track tr;
    gview v[MAXP];
} actor;

typedef struct event {
    double t;
    int line;
    char words[16][80];
    int n;
} event;

typedef struct change {
    uint64_t t;
    int actor;
    uint64_t from, to;
    uint8_t cause;
} change;

static struct {
    char file[256], name[64];
    player p[MAXP];
    int np;
    actor a[MAXA];
    int na;
    event e[MAXE];
    int ne;
    char x[MAXX][16][80];
    int xn[MAXX], xline[MAXX], nx;
    double end;
    change c[MAXC];
    int nc;
    uint64_t now;
    int fails, errors;
} L;

static void fail(int line, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void fail(int line, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("FAIL %s line %d: ", L.name[0] ? L.name : L.file, line);
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    L.fails++;
}

static void bad(int line, const char *what) {
    printf("ERROR %s line %d: %s\n", L.file, line, what);
    L.errors++;
}

/* ---- tracks ---- */

static void track_add(track *tr, double t, const float p[3]) {
    if (tr->n >= MAXK) return;
    int i = tr->n++;
    while (i > 0 && tr->k[i - 1].t > t) tr->k[i] = tr->k[i - 1], i--;
    tr->k[i].t = t;
    memcpy(tr->k[i].p, p, sizeof tr->k[i].p);
}

static void track_at(const track *tr, double t, float out[3]) {
    if (!tr->n) {
        out[0] = out[1] = out[2] = 0;
        return;
    }
    int last = -1;
    for (int i = 0; i < tr->n; i++)
        if (tr->k[i].t <= t) last = i;
    if (last < 0) {
        memcpy(out, tr->k[0].p, sizeof(float) * 3);
        return;
    }
    if (last + 1 >= tr->n || tr->k[last + 1].t == tr->k[last].t) {
        memcpy(out, tr->k[last].p, sizeof(float) * 3);
        return;
    }
    const key *a = &tr->k[last], *b = &tr->k[last + 1];
    float f = (float)((t - a->t) / (b->t - a->t));
    for (int j = 0; j < 3; j++) out[j] = a->p[j] + (b->p[j] - a->p[j]) * f;
}

static float dist3(const float a[3], const float b[3]) {
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

/* ---- lookups ---- */

static int player_of(const char *n) {
    for (int i = 0; i < L.np; i++)
        if (!strcmp(L.p[i].name, n)) return i;
    return -1;
}

static int player_of_account(uint64_t a) {
    for (int i = 0; i < L.np; i++)
        if (L.p[i].account == a) return i;
    return -1;
}

static int actor_of(const char *n) {
    for (int i = 0; i < L.na; i++)
        if (!strcmp(L.a[i].name, n)) return i;
    return -1;
}

/* A placed actor's id is unique within its scene only, as on the server. */
static int actor_of_id(const ow_aid *id, uint32_t scene) {
    for (int i = 0; i < L.na; i++)
        if (L.a[i].id.origin == id->origin && L.a[i].id.serial == id->serial && (id->origin || L.a[i].scene == scene))
            return i;
    return -1;
}

static const char *who(uint64_t account) {
    int p = player_of_account(account);
    return p >= 0 ? L.p[p].name : account ? "?" : "none";
}

static int parse_vec(const char *s, float out[3]) { return sscanf(s, "%f,%f,%f", &out[0], &out[1], &out[2]) == 3; }

static int parse_aid(const char *s, ow_aid *id) {
    unsigned long long o, n;
    if (sscanf(s, "%llu:%llu", &o, &n) != 2) return 0;
    id->origin = o, id->serial = n;
    return 1;
}

/* ---- the server's side, as the lab sees it ---- */

static void io_send(void *ctx, uint64_t account, uint16_t type, const void *msg) {
    (void)ctx;
    int pi = player_of_account(account);
    if (pi < 0) return;
    int ai;
    gview *v;
    switch (type) {
    case OW_T_owner: {
        const ow_owner *m = msg;
        if ((ai = actor_of_id(&m->id, L.p[pi].scene)) < 0) return;
        v = &L.a[ai].v[pi];
        v->got[G_OWNER]++;
        if (m->epoch >= v->epoch) v->epoch = m->epoch, v->owner = m->owner;
        v->known = 1;
        break;
    }
    case OW_T_actor_recv: {
        const ow_actor_recv *m = msg;
        for (int k = 0; k < m->items_count; k++)
            if ((ai = actor_of_id(&m->items[k].id, L.p[pi].scene)) >= 0) L.a[ai].v[pi].got[G_RECV]++;
        break;
    }
    case OW_T_hit_in: {
        const ow_hit_in *m = msg;
        if ((ai = actor_of_id(&m->info.target, L.p[pi].scene)) >= 0) L.a[ai].v[pi].got[G_HIT_IN]++;
        break;
    }
    case OW_T_spawned: {
        const ow_spawned *m = msg;
        if ((ai = actor_of_id(&m->info.id, L.p[pi].scene)) < 0) return;
        v = &L.a[ai].v[pi];
        v->got[G_SPAWNED]++;
        v->known = 1;
        v->owner = m->creator;
        v->epoch = 1;
        break;
    }
    case OW_T_gone: {
        const ow_gone *m = msg;
        if ((ai = actor_of_id(&m->id, L.p[pi].scene)) < 0) return;
        v = &L.a[ai].v[pi];
        v->got[G_GONE]++;
        v->known = v->loaded = 0;
        break;
    }
    case OW_T_taken: {
        const ow_taken *m = msg;
        if ((ai = actor_of_id(&m->id, L.p[pi].scene)) < 0) return;
        v = &L.a[ai].v[pi];
        v->got[m->yours ? G_TAKEN_YOURS : G_TAKEN_NOT]++;
        if (!m->yours) v->known = v->loaded = 0;
        else L.a[ai].exists = 0; /* in the taker's pouch: no game offers it again */
        break;
    }
    default: break;
    }
}

static void io_log(void *ctx, const char *line) {
    (void)ctx;
    (void)line;
}

static void io_changed(void *ctx, const ow_aid *id, uint32_t scene, const char *name, uint64_t from, uint64_t to,
                       uint8_t cause, uint32_t epoch, uint64_t now) {
    (void)ctx;
    (void)name;
    (void)epoch;
    if (L.nc >= MAXC) return;
    L.c[L.nc++] = (change){ now, actor_of_id(id, scene), from, to, cause };
}

/* ---- the games ---- */

static void send_from(int pi, uint16_t type, const void *msg) {
    if (!L.p[pi].connected) return;
    if (!sva_frame_ok(L.p[pi].account, L.now)) {
        fail(0, "%s's game was cut off as a flood", L.p[pi].name);
        return;
    }
    sva_message(L.p[pi].account, type, msg, L.now);
}

static void game_offer(int pi, int ai) {
    actor *a = &L.a[ai];
    float at[3];
    track_at(&a->tr, (double)L.now / 1e6, at);
    ow_offer m;
    memset(&m, 0, sizeof m);
    m.id = a->id;
    const ow_class_row *row = ow_class_find(a->game_name);
    m.class_ = row && row->class_ != OW_CLASS_LOCAL ? row->class_ : 0;
    snprintf(m.name, sizeof m.name, "%s", a->game_name);
    memcpy(m.pos, at, sizeof m.pos);
    a->v[pi].loaded = 1;
    send_from(pi, OW_T_offer, &m);
}

static void game_unload(int pi, int ai) {
    gview *v = &L.a[ai].v[pi];
    ow_gone m = { .id = L.a[ai].id, .epoch = v->epoch, .cause = OW_GONE_UNLOADED };
    v->loaded = 0;
    send_from(pi, OW_T_gone, &m);
}

static ow_actor_send g_send;

static void game_step(int pi) {
    player *p = &L.p[pi];
    if (!p->connected) return;
    double t = (double)L.now / 1e6;
    float me[3];
    track_at(&p->tr, t, me);
    sva_player_pos(p->account, p->scene, me, L.now);
    g_send.time_us = L.now;
    g_send.items_count = 0;
    for (int ai = 0; ai < L.na; ai++) {
        actor *a = &L.a[ai];
        gview *v = &a->v[pi];
        if (!a->exists) continue;
        float at[3];
        track_at(&a->tr, t, at);
        float d = a->scene == p->scene ? dist3(me, at) : 1e30f;
        int may_load = !a->id.origin || v->known || a->id.origin == p->account;
        if (!p->manual && !v->loaded && may_load && d <= p->load) game_offer(pi, ai);
        else if (!p->manual && v->loaded && d > p->unload) game_unload(pi, ai);
        if (v->loaded && v->owner == p->account && v->epoch) {
            ow_actor_pose *it = &g_send.items[g_send.items_count++];
            memset(it, 0, sizeof *it);
            it->id = a->id;
            it->epoch = v->epoch;
            memcpy(it->body.pos, at, sizeof it->body.pos);
            it->body.rot[3] = 1.0f;
            it->flags = a->asleep ? OW_POSE_ASLEEP : 0;
            if (g_send.items_count == OW_POSE_BATCH) {
                send_from(pi, OW_T_actor_send, &g_send);
                g_send.items_count = 0;
            }
        }
    }
    if (g_send.items_count) send_from(pi, OW_T_actor_send, &g_send);
}

/* ---- events ---- */

static uint8_t place_state(const char *w) {
    static const char *const names[] = {"title", "playing", "loading", "paused", "dead"};
    for (int i = 0; i < 5; i++)
        if (!strcmp(w, names[i])) return (uint8_t)i;
    return 255;
}

static void run_event(const event *e) {
    int pi = player_of(e->words[2]);
    if (pi < 0) {
        bad(e->line, "unknown player");
        return;
    }
    player *p = &L.p[pi];
    const char *what = e->words[3];
    int ai = e->n > 4 ? actor_of(e->words[4]) : -1;
    if (!strcmp(what, "join")) {
        p->joined = p->connected = 1;
        sva_player_join(p->account, L.now);
        float me[3];
        track_at(&p->tr, (double)L.now / 1e6, me);
        sva_player_pos(p->account, p->scene, me, L.now);
        ow_place pl = { .scene = p->scene, .state = OW_PLACE_PLAYING };
        sva_player_area(p->account, &pl, L.now);
    } else if (!strcmp(what, "away")) {
        p->connected = 0;
        sva_player_away(p->account, L.now);
    } else if (!strcmp(what, "leave")) {
        p->connected = 0;
        sva_player_leave(p->account, L.now);
        for (int k = 0; k < L.na; k++) memset(&L.a[k].v[pi], 0, sizeof L.a[k].v[pi]);
    } else if (!strcmp(what, "area")) {
        ow_place pl;
        memset(&pl, 0, sizeof pl);
        pl.state = place_state(e->words[4]);
        for (int k = 5; k < e->n; k++)
            if (!strncmp(e->words[k], "scene=", 6)) p->scene = (uint32_t)atoi(e->words[k] + 6);
        pl.scene = p->scene;
        if (pl.state == 255) bad(e->line, "unknown place state");
        else sva_player_area(p->account, &pl, L.now);
    } else if (!strcmp(what, "group")) {
        ow_group m;
        memset(&m, 0, sizeof m);
        if (e->n < 6 || !parse_aid(e->words[5], &m.id)) {
            bad(e->line, "group needs NAME ORIGIN:SERIAL");
            return;
        }
        for (int k = 6; k < e->n && m.members_count < OW_GROUP_MAX; k++) {
            int mi = actor_of(e->words[k]);
            if (mi >= 0) m.members[m.members_count++] = L.a[mi].id;
        }
        send_from(pi, OW_T_group, &m);
    } else if (ai < 0) {
        bad(e->line, "unknown actor");
    } else if (!strcmp(what, "offer")) {
        game_offer(pi, ai);
    } else if (!strcmp(what, "unload")) {
        game_unload(pi, ai);
    } else if (!strcmp(what, "release")) {
        ow_release m = { .id = L.a[ai].id, .epoch = L.a[ai].v[pi].epoch };
        send_from(pi, OW_T_release, &m);
        L.a[ai].v[pi].loaded = 0;
    } else if (!strcmp(what, "take")) {
        ow_take m = { .id = L.a[ai].id, .epoch = L.a[ai].v[pi].epoch };
        send_from(pi, OW_T_take, &m);
    } else if (!strcmp(what, "hold")) {
        static const char *const kinds[] = {"none", "ultrahand", "carry", "ride"};
        ow_hold m = { .id = L.a[ai].id, .what = 0, .on = e->n > 6 && !strcmp(e->words[6], "on") };
        for (uint8_t k = 0; k < 4; k++)
            if (e->n > 5 && !strcmp(e->words[5], kinds[k])) m.what = k;
        send_from(pi, OW_T_hold, &m);
    } else if (!strcmp(what, "hit")) {
        int n = 1, stale = 0;
        for (int k = 5; k < e->n; k++) {
            if (!strncmp(e->words[k], "n=", 2)) n = atoi(e->words[k] + 2);
            if (!strcmp(e->words[k], "stale")) stale = 1;
        }
        float at[3];
        track_at(&L.a[ai].tr, (double)L.now / 1e6, at);
        for (int k = 0; k < n; k++) {
            ow_hit m;
            memset(&m, 0, sizeof m);
            m.info.target = L.a[ai].id;
            m.info.epoch = L.a[ai].v[pi].epoch - (uint32_t)stale;
            m.info.amount = 5;
            memcpy(m.info.at, at, sizeof at);
            send_from(pi, OW_T_hit, &m);
        }
    } else if (!strcmp(what, "spawn")) {
        actor *a = &L.a[ai];
        a->exists = 1;
        ow_spawn m;
        memset(&m, 0, sizeof m);
        m.info.id = a->id;
        for (int k = 5; k < e->n; k++)
            if (!strncmp(e->words[k], "parent=", 7)) {
                int par = actor_of(e->words[k] + 7);
                if (par >= 0) m.info.parent = L.a[par].id;
            }
        const ow_class_row *row = ow_class_find(a->game_name);
        m.info.class_ = row && row->class_ != OW_CLASS_LOCAL ? row->class_ : 0;
        snprintf(m.info.name, sizeof m.info.name, "%s", a->game_name);
        track_at(&a->tr, (double)L.now / 1e6, m.info.body.pos);
        m.info.body.rot[3] = 1.0f;
        a->v[pi] = (gview){ .loaded = 1, .known = 1, .owner = p->account, .epoch = 1 };
        a->spawned_by = p->account;
        a->spawned_at = L.now;
        send_from(pi, OW_T_spawn, &m);
    } else if (!strcmp(what, "gone")) {
        uint8_t cause = !strcmp(e->n > 5 ? e->words[5] : "", "killed") ? OW_GONE_KILLED
                      : !strcmp(e->n > 5 ? e->words[5] : "", "taken") ? OW_GONE_TAKEN : OW_GONE_REMOVED;
        ow_gone m = { .id = L.a[ai].id, .epoch = L.a[ai].v[pi].epoch, .cause = cause };
        send_from(pi, OW_T_gone, &m);
        if (L.a[ai].v[pi].owner == p->account) L.a[ai].exists = 0;
    } else if (!strcmp(what, "stale")) {
        ow_actor_send m;
        memset(&m, 0, sizeof m);
        m.time_us = L.now;
        m.items_count = 1;
        m.items[0].id = L.a[ai].id;
        m.items[0].epoch = L.a[ai].v[pi].epoch - 1;
        m.items[0].body.rot[3] = 1.0f;
        send_from(pi, OW_T_actor_send, &m);
    } else {
        bad(e->line, "unknown event");
    }
    (void)ai;
}

/* ---- reading a case ---- */

static int split(char *line, char words[16][80]) {
    int n = 0;
    char *save = NULL;
    for (char *t = strtok_r(line, " \t\r\n", &save); t && n < 16; t = strtok_r(NULL, " \t\r\n", &save))
        snprintf(words[n++], 80, "%s", t);
    return n;
}

static int load_case(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        printf("ERROR cannot open %s\n", path);
        return -1;
    }
    char line[1024];
    int lineno = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        char w[16][80];
        int n = split(line, w);
        if (!n) continue;
        if (!strcmp(w[0], "case") && n >= 2) {
            snprintf(L.name, sizeof L.name, "%s", w[1]);
        } else if (!strcmp(w[0], "player") && n >= 2 && L.np < MAXP) {
            player *p = &L.p[L.np];
            snprintf(p->name, sizeof p->name, "%s", w[1]);
            p->account = 1001 + (uint64_t)L.np;
            p->scene = 1;
            p->load = 150, p->unload = 200;
            for (int k = 2; k < n; k++) {
                if (!strncmp(w[k], "scene=", 6)) p->scene = (uint32_t)atoi(w[k] + 6);
                else if (!strncmp(w[k], "load=", 5)) p->load = (float)atof(w[k] + 5);
                else if (!strncmp(w[k], "unload=", 7)) p->unload = (float)atof(w[k] + 7);
                else if (!strcmp(w[k], "manual")) p->manual = 1;
                else bad(lineno, "unknown player option");
            }
            L.np++;
        } else if (!strcmp(w[0], "actor") && n >= 5 && L.na < MAXA) {
            actor *a = &L.a[L.na];
            snprintf(a->name, sizeof a->name, "%s", w[1]);
            snprintf(a->game_name, sizeof a->game_name, "%s", w[3]);
            float at[3];
            if (!parse_aid(w[2], &a->id) || !parse_vec(w[4], at)) bad(lineno, "actor NAME ORIGIN:SERIAL NAME X,Y,Z");
            /* An origin written as a player's number (1, 2...) is that player's account. */
            if (a->id.origin && a->id.origin <= MAXP) a->id.origin = 1000 + a->id.origin;
            track_add(&a->tr, 0, at);
            a->scene = 1;
            a->exists = 1;
            for (int k = 5; k < n; k++) {
                if (!strncmp(w[k], "scene=", 6)) a->scene = (uint32_t)atoi(w[k] + 6);
                else if (!strcmp(w[k], "asleep")) a->asleep = 1;
                else if (!strcmp(w[k], "late")) a->late = 1, a->exists = 0;
                else bad(lineno, "unknown actor option");
            }
            L.na++;
        } else if (!strcmp(w[0], "walk") && n == 6) {
            int pi = player_of(w[1]), ai = actor_of(w[1]);
            track *tr = pi >= 0 ? &L.p[pi].tr : ai >= 0 ? &L.a[ai].tr : NULL;
            float a[3], b[3];
            if (!tr || !parse_vec(w[4], a) || !parse_vec(w[5], b)) {
                bad(lineno, "walk WHO T0 T1 FROM TO");
                continue;
            }
            /* An actor's first walk replaces where it was declared. */
            if (ai >= 0 && tr->n == 1 && tr->k[0].t == 0) tr->n = 0;
            track_add(tr, atof(w[2]), a);
            track_add(tr, atof(w[3]), b);
        } else if (!strcmp(w[0], "wander") && n == 7 && (player_of(w[1]) >= 0 || actor_of(w[1]) >= 0)) {
            track *tr = player_of(w[1]) >= 0 ? &L.p[player_of(w[1])].tr : &L.a[actor_of(w[1])].tr;
            float x0, z0, x1, z1;
            double t = atof(w[2]), t1 = atof(w[3]);
            if (sscanf(w[4], "%f,%f", &x0, &z0) != 2 || sscanf(w[5], "%f,%f", &x1, &z1) != 2) {
                bad(lineno, "wander P T0 T1 X0,Z0 X1,Z1 SEED");
                continue;
            }
            uint64_t seed = (uint64_t)atoll(w[6]) * 2654435761u + 1;
            float at[3];
            track_at(tr, t, at);
            track_add(tr, t, at);
            while (t < t1 && tr->n + 2 < MAXK) {
                float to[3];
                seed = seed * 6364136223846793005ull + 1442695040888963407ull;
                to[0] = x0 + (x1 - x0) * (float)((seed >> 33) % 10000) / 10000.0f;
                seed = seed * 6364136223846793005ull + 1442695040888963407ull;
                to[2] = z0 + (z1 - z0) * (float)((seed >> 33) % 10000) / 10000.0f;
                to[1] = 0;
                t += dist3(at, to) / 3.0;
                track_add(tr, t, to);
                seed = seed * 6364136223846793005ull + 1442695040888963407ull;
                t += 2.0 + (double)((seed >> 33) % 8000) / 1000.0;
                track_add(tr, t, to);
                memcpy(at, to, sizeof at);
            }
        } else if (!strcmp(w[0], "follow") && n == 5 && player_of(w[1]) >= 0 &&
                   (player_of(w[2]) >= 0 || actor_of(w[2]) >= 0)) {
            const track *src = player_of(w[2]) >= 0 ? &L.p[player_of(w[2])].tr : &L.a[actor_of(w[2])].tr;
            track *tr = &L.p[player_of(w[1])].tr;
            float dx, dz;
            double delay = atof(w[4]);
            if (sscanf(w[3], "%f,%f", &dx, &dz) != 2) {
                bad(lineno, "follow P LEADER DX,DZ DELAY");
                continue;
            }
            tr->n = 0;
            for (int k = 0; k < src->n; k++) {
                float at[3] = {src->k[k].p[0] + dx, src->k[k].p[1], src->k[k].p[2] + dz};
                track_add(tr, src->k[k].t + delay, at);
            }
        } else if (!strcmp(w[0], "grid") && n >= 7) {
            float x0, z0, dx, dz;
            int nx, nz;
            unsigned long long serial = strtoull(w[6], NULL, 10);
            uint32_t scene = 1;
            for (int k = 7; k < n; k++)
                if (!strncmp(w[k], "scene=", 6)) scene = (uint32_t)atoi(w[k] + 6);
            if (sscanf(w[3], "%f,%f", &x0, &z0) != 2 || sscanf(w[4], "%f,%f", &dx, &dz) != 2 ||
                sscanf(w[5], "%d,%d", &nx, &nz) != 2) {
                bad(lineno, "grid PREFIX NAME X0,Z0 DX,DZ NX,NZ SERIAL");
                continue;
            }
            for (int i = 0; i < nx * nz && L.na < MAXA; i++) {
                actor *a = &L.a[L.na++];
                snprintf(a->name, sizeof a->name, "%s%d", w[1], i);
                snprintf(a->game_name, sizeof a->game_name, "%s", w[2]);
                a->id.origin = 0;
                a->id.serial = serial + (unsigned long long)i;
                float at[3] = {x0 + dx * (float)(i % nx), 0, z0 + dz * (float)(i / nx)};
                track_add(&a->tr, 0, at);
                a->scene = scene;
                a->exists = 1;
            }
        } else if (!strcmp(w[0], "at") && n >= 4 && L.ne < MAXE) {
            event *e = &L.e[L.ne++];
            e->t = atof(w[1]);
            e->line = lineno;
            e->n = n;
            memcpy(e->words, w, sizeof e->words);
        } else if (!strcmp(w[0], "run") && n == 2) {
            L.end = atof(w[1]);
        } else if (!strcmp(w[0], "expect") && n >= 3 && L.nx < MAXX) {
            memcpy(L.x[L.nx], w, sizeof L.x[L.nx]);
            L.xn[L.nx] = n;
            L.xline[L.nx++] = lineno;
        } else {
            bad(lineno, "unknown line");
        }
    }
    fclose(f);
    /* Events in time order, file order among equals. */
    for (int i = 1; i < L.ne; i++)
        for (int j = i; j > 0 && L.e[j - 1].t > L.e[j].t; j--) {
            event t = L.e[j];
            L.e[j] = L.e[j - 1];
            L.e[j - 1] = t;
        }
    return 0;
}

/* ---- expectations ---- */

static int compare(const char *op, double have, double want) {
    if (!strcmp(op, "<=")) return have <= want;
    if (!strcmp(op, ">=")) return have >= want;
    if (!strcmp(op, "==")) return have == want;
    if (!strcmp(op, "<")) return have < want;
    if (!strcmp(op, ">")) return have > want;
    return 0;
}

/* Whether change i counts for target: all, an actor's name, class=C, or flips (optionally flips:class=C): the
 * hand-overs rules 2 to 4 make from a present owner, the old port's measure of owner changes, which left out actors
 * coming into range (no owner), owners leaving and holds. */
static int matches_target(const char *target, int ai);
static int change_counts(const char *target, int i) {
    if (!strncmp(target, "flips", 5)) {
        uint8_t c = L.c[i].cause;
        if (c != OW_OWNER_FAR && c != OW_OWNER_CLOSER && c != OW_OWNER_PICKUP) return 0;
        return target[5] == ':' ? matches_target(target + 6, L.c[i].actor) : !target[5];
    }
    return matches_target(target, L.c[i].actor);
}

static int matches_target(const char *target, int ai) {
    if (!strcmp(target, "all")) return 1;
    if (ai < 0) return 0;
    if (!strncmp(target, "class=", 6)) {
        static const char *const names[] = {"map_object", "world_object", "enemy", "boss", "animal",
                                            "townsperson", "item", "projectile", "puppet", "device"};
        const ow_class_row *row = ow_class_find(L.a[ai].game_name);
        return row && row->class_ < 10 && !strcmp(names[row->class_], target + 6);
    }
    return !strcmp(L.a[ai].name, target);
}

static int window(char w[16][80], int n, int at, double *t0, double *t1) {
    *t0 = 0, *t1 = 1e18;
    if (at + 1 >= n || strcmp(w[at], "in")) return at;
    const char *dots = strstr(w[at + 1], "..");
    if (!dots) return -1;
    *t0 = atof(w[at + 1]);
    *t1 = atof(dots + 2);
    return at + 2;
}

static uint64_t owner_at(int ai, double t, int *known) {
    uint64_t o = 0;
    *known = 0;
    if (L.a[ai].spawned_by && (double)L.a[ai].spawned_at / 1e6 <= t) o = L.a[ai].spawned_by, *known = 1;
    for (int i = 0; i < L.nc; i++)
        if (L.c[i].actor == ai && (double)L.c[i].t / 1e6 <= t) o = L.c[i].to, *known = 1;
    return o;
}

static int cause_of(const char *w) {
    static const char *const names[] = {"offer", "far", "closer", "pickup", "left", "hold", "host"};
    for (int i = 0; i <= OW_OWNER_LAST; i++)
        if (!strcmp(w, names[i])) return i;
    return -1;
}

static int kept_of(const char *w) {
    static const struct {
        const char *w;
        int k;
    } names[] = {{"fixed", SVO_KEPT_FIXED}, {"held", SVO_KEPT_HELD}, {"quest", SVO_KEPT_QUEST},
                 {"fight", SVO_KEPT_FIGHT}, {"asleep", SVO_KEPT_ASLEEP}, {"still", SVO_KEPT_STILL},
                 {"recent", SVO_KEPT_RECENT}, {"far_margin", SVO_KEPT_FAR_MARGIN},
                 {"orphan_nearer", SVO_KEPT_ORPHAN_NEARER}, {"orphan_far", SVO_KEPT_ORPHAN_FAR},
                 {"paused", SVO_KEPT_PAUSED}};
    for (size_t i = 0; i < sizeof names / sizeof *names; i++)
        if (!strcmp(w, names[i].w)) return names[i].k;
    return -1;
}

static void check_expect(char w[16][80], int n, int line) {
    const char *kind = w[1];
    if (!strcmp(kind, "owner") && n == 7) { /* expect owner X at T is P */
        int ai = actor_of(w[2]), known;
        double t = atof(w[4]);
        if (ai < 0) return bad(line, "unknown actor");
        uint64_t o = owner_at(ai, t, &known);
        const char *want = w[6];
        int ok = !strcmp(want, "none") ? o == 0 : player_of(want) >= 0 && o == L.p[player_of(want)].account;
        if (!ok) fail(line, "owner of %s at %.1f s is %s, expected %s", w[2], t, who(o), want);
    } else if (!strcmp(kind, "changes")) { /* expect changes X [in T0..T1] OP N */
        double t0, t1;
        int k = window(w, n, 3, &t0, &t1);
        if (k < 0 || k + 2 > n) return bad(line, "expect changes X [in T0..T1] OP N");
        int count = 0;
        for (int i = 0; i < L.nc; i++) {
            double t = (double)L.c[i].t / 1e6;
            if (t >= t0 && t < t1 && change_counts(w[2], i)) count++;
        }
        if (!compare(w[k], count, atof(w[k + 1])))
            fail(line, "%d owner changes of %s in %.0f..%.0f s, expected %s %s", count, w[2], t0, t1 > 1e17 ? L.end : t1,
                 w[k], w[k + 1]);
    } else if (!strcmp(kind, "rate")) { /* expect rate all in T0..T1 <= R */
        double t0, t1;
        int k = window(w, n, 3, &t0, &t1);
        if (k < 0 || k + 2 > n || t1 > 1e17) return bad(line, "expect rate X in T0..T1 OP R");
        int count = 0;
        for (int i = 0; i < L.nc; i++) {
            double t = (double)L.c[i].t / 1e6;
            if (t >= t0 && t < t1 && change_counts(w[2], i)) count++;
        }
        double rate = count / ((t1 - t0) / 60.0);
        printf("  %s: %d owner changes of %s in %.0f..%.0f s, %.2f a minute\n", L.name, count, w[2], t0, t1, rate);
        if (!compare(w[k], rate, atof(w[k + 1])))
            fail(line, "%.2f owner changes a minute of %s, expected %s %s", rate, w[2], w[k], w[k + 1]);
    } else if (!strcmp(kind, "cause") && n == 6) { /* expect cause X CAUSE OP N */
        int ai = actor_of(w[2]), c = cause_of(w[3]), count = 0;
        if (ai < 0 || c < 0) return bad(line, "expect cause X CAUSE OP N");
        for (int i = 0; i < L.nc; i++) count += L.c[i].actor == ai && L.c[i].cause == c;
        if (!compare(w[4], count, atof(w[5])))
            fail(line, "%d changes of %s by %s, expected %s %s", count, w[2], w[3], w[4], w[5]);
    } else if (!strcmp(kind, "got") && (n == 6 || n == 7)) { /* expect got P TYPE [X] OP N */
        int pi = player_of(w[2]), type = -1, ai = n == 7 ? actor_of(w[4]) : -1;
        for (int i = 0; i < G_TYPES; i++)
            if (!strcmp(w[3], GOT_NAMES[i])) type = i;
        if (pi < 0 || type < 0 || (n == 7 && ai < 0)) return bad(line, "expect got P TYPE [X] OP N");
        int count = 0;
        for (int i = 0; i < L.na; i++)
            if (ai < 0 || i == ai) count += L.a[i].v[pi].got[type];
        const char *op = w[n - 2], *want = w[n - 1];
        if (!compare(op, count, atof(want)))
            fail(line, "%s's game got %d %s%s%s, expected %s %s", w[2], count, w[3], n == 7 ? " of " : "",
                 n == 7 ? w[4] : "", op, want);
    } else if (!strcmp(kind, "kept") && n == 5) { /* expect kept NAME OP N */
        int k = kept_of(w[2]);
        if (k < 0) return bad(line, "unknown kept name");
        uint64_t have = sva_get_stats()->kept[k];
        if (!compare(w[3], (double)have, atof(w[4])))
            fail(line, "kept by %s %llu times, expected %s %s", w[2], (unsigned long long)have, w[3], w[4]);
    } else {
        bad(line, "unknown expectation");
    }
}

/* ---- one case ---- */

static int run_case(const char *path) {
    memset(&L, 0, sizeof L);
    snprintf(L.file, sizeof L.file, "%s", path);
    if (load_case(path)) return 1;
    if (L.errors) return 1;
    sva_io io = { .send = io_send, .log = io_log, .changed = io_changed };
    sva_init(&io, 250.0f);
    int ei = 0;
    uint64_t end = US(L.end), next_game = 0;
    for (L.now = 0; L.now <= end; L.now += STEP_US) {
        while (ei < L.ne && US(L.e[ei].t) <= L.now) run_event(&L.e[ei++]);
        if (L.now >= next_game) {
            next_game = L.now + GAME_US;
            for (int pi = 0; pi < L.np; pi++) game_step(pi);
        }
        sva_tick(L.now);
    }
    for (int i = 0; i < L.nx; i++) check_expect(L.x[i], L.xn[i], L.xline[i]);
    const sva_stats *st = sva_get_stats();
    printf("%s %s: %d owner changes (", L.fails || L.errors ? "FAIL" : "ok  ", L.name, L.nc);
    for (int c = 0, first = 1; c <= OW_OWNER_LAST; c++)
        if (st->changes[c]) {
            printf("%s%s %llu", first ? "" : ", ", svo_cause_name((uint8_t)c), (unsigned long long)st->changes[c]);
            first = 0;
        }
    printf("), %llu states relayed, %llu hits passed\n", (unsigned long long)st->relayed,
           (unsigned long long)st->hits_passed);
    if (L.fails || L.errors)
        for (int i = 0; i < L.nc; i++)
            printf("    %7.2f s  %-14s %s -> %s (%s)\n", (double)L.c[i].t / 1e6,
                   L.c[i].actor >= 0 ? L.a[L.c[i].actor].name : "?", who(L.c[i].from), who(L.c[i].to),
                   svo_cause_name(L.c[i].cause));
    sva_shutdown();
    return L.fails || L.errors;
}

int main(int argc, char **argv) {
    int bad_cases = 0;
    for (int i = 1; i < argc; i++) bad_cases += run_case(argv[i]);
    printf("actor_lab: %d of %d cases %s\n", argc - 1 - bad_cases, argc - 1, bad_cases ? "passed, others FAILED" : "passed");
    return bad_cases ? 1 : 0;
}
