// Saves and world state on the client (docs/online-saves.md, runtime/net/net_save.h).
//
// Four tables under one lock:
//   keys     the key table (world-keys.tsv from tools/world_keys.py): key hash -> shared or the player's own
//   known    the last value of each (key, index) this game holds as settled: what it wrote and sent, or what the
//            server sent it. A game write equal to it is not sent again, which also keeps a value the game took from
//            the server from going back as a new write.
//   pending  writes waiting for the next batch (at most one per (key, index): a newer write replaces the older)
//   incoming values the game must take at its next GameData commit, in arrival order
// net_client.c drives it: net_save_connecting, net_save_admitted, net_save_on_msg for the four s2c messages,
// net_save_joined at join_ready, net_save_pump every pass of its loop while live, net_save_offline when it ends.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "net/net_save.h"
#include "ow_wire.h"
#include "platform/platform.h"

#define NS_BATCH_US 250000u        // a batch at most every 250 ms (docs/online-wire.md, save_write)
#define NS_BUSY_RUN 8              // a key in this many batches in a row counts as busy (a counter rewritten each frame)
#define NS_BUSY_US 2000000u        // a busy key goes out at most every 2 s
#define NS_LISTENERS 8
#define SENT 0xFF                  // kind of a pending slot whose write went out; the slot stays to count its run

typedef struct Slot {
    uint32_t key;
    uint16_t index;
    uint8_t used;
    uint8_t run;                   // pending: batches in a row this slot went out in
    uint64_t last_us;              // pending: when it last went out
    NetSaveValue v;
} Slot;

// Open addressing on (key, index); grows at 3/4. Slots are never removed one by one, only cleared with the table.
typedef struct Map {
    Slot *s;
    uint32_t cap, n;
} Map;

static PlatMutex g_mx = PLAT_MUTEX_INIT;
static struct {
    // key table: sorted hashes, rule byte (1 = shared, 0 = the player's own)
    uint32_t *key;
    uint8_t *shared;
    int nkeys, loaded;
    Map known, pending;
    int npend;                     // pending slots holding a write not yet sent (kind != SENT)
    // incoming FIFO
    NetSaveValue *in;
    int in_head, in_n, in_cap;
    // the session
    int session;                   // 1 from net_save_admitted (with saves granted) until net_save_offline
    int waiting;                   // a connection is on its way and the snapshot has not all come
    uint32_t seq;
    uint64_t next_batch;
    struct { NetSaveListener fn; void *ctx; } lis[NS_LISTENERS];
    int nlis;
} N;

// ---- the key table

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

// world-keys.tsv: "<8 hex digits>\t<rule>[\t<kind>\t<name>]", '#' starts a comment. Found through TOTK_WORLD_KEYS or
// next to the game (the working folder). Without it every key is the player's own: nothing is shared, nothing of the
// player's is lost.
static void load_keys(void) {
    N.loaded = 1;
    const char *path = getenv("TOTK_WORLD_KEYS");
    if (!path || !*path) path = "world-keys.tsv";
    FILE *f = fopen(path, "r");
    if (!f) {
        rt_log("saves: no %s, so the world is not shared (every value stays the player's own)", path);
        return;
    }
    int cap = 0;
    uint32_t *pairs = NULL;   // (hash, shared) interleaved, sorted afterwards
    char line[512];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        char k[16], r[16];
        if (sscanf(line, "%15s %15s", k, r) != 2 || strlen(k) != 8) continue;
        char *end;
        unsigned long h = strtoul(k, &end, 16);
        if (*end) continue;
        if (N.nkeys == cap) {
            cap = cap ? cap * 2 : 4096;
            uint32_t *np = realloc(pairs, (size_t)cap * 2 * sizeof *np);
            if (!np) break;
            pairs = np;
        }
        pairs[2 * N.nkeys] = (uint32_t)h;
        pairs[2 * N.nkeys + 1] = strcmp(r, "player") != 0;
        N.nkeys++;
    }
    fclose(f);
    qsort(pairs, (size_t)N.nkeys, 2 * sizeof *pairs, cmp_u32);
    N.key = malloc((size_t)(N.nkeys ? N.nkeys : 1) * sizeof *N.key);
    N.shared = malloc((size_t)(N.nkeys ? N.nkeys : 1));
    if (!N.key || !N.shared) {
        N.nkeys = 0;
    } else {
        for (int i = 0; i < N.nkeys; i++) N.key[i] = pairs[2 * i], N.shared[i] = (uint8_t)pairs[2 * i + 1];
    }
    free(pairs);
    int sh = 0;
    for (int i = 0; i < N.nkeys; i++) sh += N.shared[i];
    rt_log("saves: %d keys from %s, %d shared by the room", N.nkeys, path, sh);
}

static int shared_locked(uint32_t key) {
    if (!N.loaded) load_keys();
    int lo = 0, hi = N.nkeys - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (N.key[m] == key) return N.shared[m];
        if (N.key[m] < key) lo = m + 1;
        else hi = m - 1;
    }
    return 0;
}

// ---- maps

static uint32_t slot_hash(uint32_t key, uint16_t index) {
    uint32_t h = key ^ ((uint32_t)index * 0x9e3779b1u);
    h ^= h >> 16, h *= 0x7feb352du, h ^= h >> 15;
    return h;
}

static Slot *map_find(Map *m, uint32_t key, uint16_t index, int add);

static int map_grow(Map *m) {
    uint32_t cap = m->cap ? m->cap * 2 : 1024;
    Slot *ns = calloc(cap, sizeof *ns);
    if (!ns) return 0;
    Map old = *m;
    m->s = ns, m->cap = cap, m->n = 0;
    for (uint32_t i = 0; i < old.cap; i++)
        if (old.s[i].used) *map_find(m, old.s[i].key, old.s[i].index, 1) = old.s[i];
    free(old.s);
    return 1;
}

// The slot of (key, index), or NULL when absent and add is 0 (or memory ran out). A new slot comes back zeroed except
// for key, index and used.
static Slot *map_find(Map *m, uint32_t key, uint16_t index, int add) {
    if (add && (m->n + 1) * 4 > m->cap * 3 && !map_grow(m)) return NULL;
    if (!m->cap) return NULL;
    for (uint32_t i = slot_hash(key, index) & (m->cap - 1);; i = (i + 1) & (m->cap - 1)) {
        Slot *s = &m->s[i];
        if (!s->used) {
            if (!add) return NULL;
            memset(s, 0, sizeof *s);
            s->used = 1, s->key = key, s->index = index;
            m->n++;
            return s;
        }
        if (s->key == key && s->index == index) return s;
    }
}

static void map_clear(Map *m) {
    if (m->s) memset(m->s, 0, (size_t)m->cap * sizeof *m->s);
    m->n = 0;
}

static int same(const NetSaveValue *a, const NetSaveValue *b) {
    return a->kind == b->kind && a->len == b->len && !memcmp(a->v, b->v, a->len);
}

static int valid(const NetSaveValue *v) {
    return v && v->kind <= OW_GD_KIND_LAST && v->len <= NET_SAVE_VALUE_MAX;
}

// ---- incoming

static void push_incoming(const NetSaveValue *v) {
    if (N.in_n == N.in_cap) {
        int cap = N.in_cap ? N.in_cap * 2 : 256;
        NetSaveValue *ni = malloc((size_t)cap * sizeof *ni);
        if (!ni) return;
        for (int i = 0; i < N.in_n; i++) ni[i] = N.in[(N.in_head + i) % N.in_cap];
        free(N.in);
        N.in = ni, N.in_cap = cap, N.in_head = 0;
    }
    N.in[(N.in_head + N.in_n) % N.in_cap] = *v;
    N.in_n++;
}

// A value from the server: settled, and for the game to take, unless the game wrote the key since and that write
// has not gone out yet (the newer write wins; the server hears of it in the next batch).
// Returns 1 when the value was taken.
static int from_server(const ow_gd_entry *e, NetSaveValue *v) {
    *v = (NetSaveValue){.key = e->key, .index = e->index, .kind = e->kind, .len = e->value_len};
    memcpy(v->v, e->value, e->value_len);
    Slot *p = map_find(&N.pending, v->key, v->index, 0);
    if (p && p->v.kind != SENT) return 0;
    Slot *k = map_find(&N.known, v->key, v->index, 1);
    if (k) k->v = *v;
    push_incoming(v);
    return 1;
}

// A write for the next batch.
static void queue_write(const NetSaveValue *v) {
    Slot *p = map_find(&N.pending, v->key, v->index, 1);
    if (!p) return;
    if (p->v.kind == SENT || !p->v.key) N.npend++;
    p->v = *v;
}

// ---- for features

int net_save_get(uint32_t key, uint16_t index, NetSaveValue *out) {
    plat_mutex_lock(&g_mx);
    Slot *s = shared_locked(key) ? NULL : map_find(&N.known, key, index, 0);
    if (s && out) *out = s->v;
    plat_mutex_unlock(&g_mx);
    return s != NULL;
}

int net_save_get_s32(uint32_t key, int32_t *out) {
    NetSaveValue v;
    if (!net_save_get(key, NET_SAVE_SCALAR, &v) || v.kind != OW_GD_S32 || v.len != 4) return 0;
    if (out) memcpy(out, v.v, 4);
    return 1;
}

int net_save_put(const NetSaveValue *v) {
    if (!valid(v)) return 0;
    plat_mutex_lock(&g_mx);
    int ok = !shared_locked(v->key);
    if (ok) {
        Slot *k = map_find(&N.known, v->key, v->index, 1);
        if (k) k->v = *v;
        if (N.session) queue_write(v);
        push_incoming(v);
    }
    plat_mutex_unlock(&g_mx);
    return ok;
}

int net_save_put_s32(uint32_t key, int32_t value) {
    NetSaveValue v = {.key = key, .index = NET_SAVE_SCALAR, .kind = OW_GD_S32, .len = 4};
    memcpy(v.v, &value, 4);
    return net_save_put(&v);
}

int net_save_listen(NetSaveListener fn, void *ctx) {
    plat_mutex_lock(&g_mx);
    int ok = fn && N.nlis < NS_LISTENERS;
    if (ok) N.lis[N.nlis].fn = fn, N.lis[N.nlis].ctx = ctx, N.nlis++;
    plat_mutex_unlock(&g_mx);
    return ok;
}

int net_save_shared(uint32_t key) {
    plat_mutex_lock(&g_mx);
    int s = shared_locked(key);
    plat_mutex_unlock(&g_mx);
    return s;
}

// ---- for the GameData hooks

void net_save_game_wrote(const NetSaveValue *v) {
    if (!valid(v)) return;
    plat_mutex_lock(&g_mx);
    Slot *k = map_find(&N.known, v->key, v->index, 1);
    if (k && k->v.key && same(&k->v, v)) {
        plat_mutex_unlock(&g_mx);
        return;   // the value the game already holds as settled
    }
    if (k) k->v = *v;
    if (N.session) queue_write(v);
    plat_mutex_unlock(&g_mx);
}

int net_save_take_incoming(NetSaveValue *out, int max) {
    plat_mutex_lock(&g_mx);
    int n = 0;
    while (n < max && N.in_n) {
        out[n++] = N.in[N.in_head];
        N.in_head = (N.in_head + 1) % N.in_cap;
        N.in_n--;
    }
    plat_mutex_unlock(&g_mx);
    return n;
}

int net_save_snapshot_done(void) {
    plat_mutex_lock(&g_mx);
    int d = !N.waiting;
    plat_mutex_unlock(&g_mx);
    return d;
}

// ---- for net_client.c

void net_save_connecting(void) {
    plat_mutex_lock(&g_mx);
    N.waiting = 1;
    plat_mutex_unlock(&g_mx);
}

void net_save_admitted(int granted) {
    plat_mutex_lock(&g_mx);
    if (!N.loaded) load_keys();
    N.session = granted;
    N.waiting = granted;   // a server without saves has no snapshot to wait for
    N.next_batch = 0;
    plat_mutex_unlock(&g_mx);
}

void net_save_joined(void) {
    plat_mutex_lock(&g_mx);
    N.waiting = 0;
    plat_mutex_unlock(&g_mx);
}

void net_save_offline(void) {
    plat_mutex_lock(&g_mx);
    N.session = 0;
    N.waiting = 0;
    map_clear(&N.pending);
    N.npend = 0;             // the next join's snapshot is the server's word on everything written meanwhile
    plat_mutex_unlock(&g_mx);
}

static void tell(const NetSaveValue *v, int n) {
    // listeners run outside the lock so they may call net_save_get / net_save_put
    plat_mutex_lock(&g_mx);
    int nl = N.nlis;
    struct { NetSaveListener fn; void *ctx; } lis[NS_LISTENERS];
    memcpy(lis, N.lis, sizeof lis);
    plat_mutex_unlock(&g_mx);
    for (int i = 0; i < n; i++)
        for (int j = 0; j < nl; j++) lis[j].fn(&v[i], lis[j].ctx);
}

int net_save_on_msg(uint16_t type, const void *msg) {
    const ow_gd_entry *e;
    int n, own;
    switch (type) {
    case OW_T_save_part: e = ((const ow_save_part *)msg)->entries, n = ((const ow_save_part *)msg)->entries_count, own = 1; break;
    case OW_T_save_fix: e = ((const ow_save_fix *)msg)->entries, n = ((const ow_save_fix *)msg)->entries_count, own = 1; break;
    case OW_T_world_part: e = ((const ow_world_part *)msg)->entries, n = ((const ow_world_part *)msg)->entries_count, own = 0; break;
    case OW_T_world_set: e = ((const ow_world_set *)msg)->entries, n = ((const ow_world_set *)msg)->entries_count, own = 0; break;
    default: return 0;
    }
    static NetSaveValue note[OW_GD_BATCH];   // the network thread alone comes here
    int nn = 0;
    plat_mutex_lock(&g_mx);
    for (int i = 0; i < n; i++) {
        NetSaveValue v;
        if (from_server(&e[i], &v) && own) note[nn++] = v;
    }
    plat_mutex_unlock(&g_mx);
    if (nn) tell(note, nn);
    return 1;
}

// One batch of pending values of one side (shared or own) into msg entries; 0 when none is due.
static int take_batch(int shared, ow_gd_entry *out, uint64_t now) {
    int n = 0;
    for (uint32_t i = 0; i < N.pending.cap && n < OW_GD_BATCH; i++) {
        Slot *s = &N.pending.s[i];
        if (!s->used || s->v.kind == SENT) continue;
        if (shared_locked(s->key) != shared) continue;
        if (s->run >= NS_BUSY_RUN && now - s->last_us < NS_BUSY_US) continue;
        out[n].key = s->key;
        out[n].index = s->index;
        out[n].kind = s->v.kind;
        out[n].value_len = s->v.len;
        memcpy(out[n].value, s->v.v, s->v.len);
        n++;
        s->run = s->run < 255 ? s->run + 1 : 255;
        s->last_us = now;
        s->v.kind = SENT, s->v.len = 0;
        N.npend--;
    }
    return n;
}

void net_save_pump(uint64_t now_us, int (*send)(uint16_t type, const void *msg)) {
    static ow_save_write sw;    // 53 KB each: the network thread alone comes here
    static ow_world_write ww;
    plat_mutex_lock(&g_mx);
    if (!N.session || now_us < N.next_batch || !N.npend) {
        plat_mutex_unlock(&g_mx);
        return;
    }
    N.next_batch = now_us + NS_BATCH_US;
    sw.entries_count = (uint16_t)take_batch(0, sw.entries, now_us);
    ww.entries_count = (uint16_t)take_batch(1, ww.entries, now_us);
    // A slot that did not go out in this batch ends its run, so a key written now and then is never held back.
    for (uint32_t i = 0; i < N.pending.cap; i++) {
        Slot *s = &N.pending.s[i];
        if (s->used && s->last_us != now_us && s->v.kind == SENT) s->run = 0;
    }
    if (sw.entries_count) sw.seq = ++N.seq;
    if (ww.entries_count) ww.seq = ++N.seq;
    plat_mutex_unlock(&g_mx);
    if (sw.entries_count) send(OW_T_save_write, &sw);
    if (ww.entries_count) send(OW_T_world_write, &ww);
}
