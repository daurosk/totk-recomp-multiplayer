/* Saves and world state (sv_save.h, docs/online-saves.md).
 *
 * Values live in hash tables keyed by (GameData key hash, array index): one for the room's world and one per account,
 * the latter read from accounts/<id>/save.json the first time the account needs it. A file is rewritten whole and
 * atomically (write a .tmp, then replace) when something in it changed, at most every 5 s and at shutdown; the copy it
 * replaces moves into backups/ next to it, where the last 20 are kept (.0 the newest). */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sv.h"
#include "sv_save.h"

#define FLUSH_EVERY_US 5000000ull
#define BACKUPS 20
#define HIST 16 /* earlier values a steps key remembers */

typedef struct ent {
    uint32_t key;
    uint16_t index;
    uint8_t kind, used;
    uint16_t len;
    uint8_t *v;
    uint8_t nhist, hpos;
    uint64_t hist[HIST]; /* fingerprints of values a steps key held, since the server started */
} ent;

typedef struct table {
    ent *e;
    uint32_t cap, n; /* cap is a power of two */
} table;

typedef struct account {
    uint64_t id;
    table t;
    int dirty, live, host;
} account;

static struct {
    char dir[512];
    sv_save_send_fn send;
    table world;
    int world_dirty;
    account *acc;
    int nacc, capacc;
    uint64_t owner, last_flush;
    uint32_t *rule_key;
    uint8_t *rule_val;
    int nrules, caprules;
} S;

/* ---- tables ---- */

static uint32_t slot_hash(uint32_t key, uint16_t index) {
    uint32_t h = key ^ (uint32_t)index * 0x9e3779b1u;
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    return h;
}

static ent *t_find(table *t, uint32_t key, uint16_t index, int add) {
    if (add && (t->n + 1) * 4 > t->cap * 3) {
        table g = { calloc(t->cap ? t->cap * 2 : 256, sizeof(ent)), t->cap ? t->cap * 2 : 256, 0 };
        if (!g.e) return NULL;
        for (uint32_t i = 0; i < t->cap; i++)
            if (t->e[i].used) {
                uint32_t j = slot_hash(t->e[i].key, t->e[i].index) & (g.cap - 1);
                while (g.e[j].used) j = (j + 1) & (g.cap - 1);
                g.e[j] = t->e[i];
                g.n++;
            }
        free(t->e);
        *t = g;
    }
    if (!t->cap) return NULL;
    uint32_t j = slot_hash(key, index) & (t->cap - 1);
    while (t->e[j].used) {
        if (t->e[j].key == key && t->e[j].index == index) return &t->e[j];
        j = (j + 1) & (t->cap - 1);
    }
    if (!add) return NULL;
    ent *e = &t->e[j];
    memset(e, 0, sizeof *e);
    e->used = 1, e->key = key, e->index = index;
    t->n++;
    return e;
}

static int ent_set(ent *e, uint8_t kind, const uint8_t *v, int len) {
    uint8_t *nv = malloc(len ? (size_t)len : 1);
    if (!nv) return 0;
    memcpy(nv, v, (size_t)len);
    free(e->v);
    e->v = nv, e->len = (uint16_t)len, e->kind = kind;
    return 1;
}

static void t_free(table *t) {
    for (uint32_t i = 0; i < t->cap; i++) free(t->e[i].v);
    free(t->e);
    memset(t, 0, sizeof *t);
}

static uint64_t fingerprint(uint8_t kind, const uint8_t *v, int len) {
    uint64_t h = 1469598103934665603ull ^ kind;
    for (int i = 0; i < len; i++) h = (h ^ v[i]) * 1099511628211ull;
    return h ^ (uint64_t)len << 56;
}

/* ---- files ---- */

static void hex(char *out, const uint8_t *in, int n) {
    static const char d[] = "0123456789abcdef";
    for (int i = 0; i < n; i++) out[2 * i] = d[in[i] >> 4], out[2 * i + 1] = d[in[i] & 15];
    out[2 * n] = 0;
}

static int unhex_n(uint8_t *out, const char *in, int n) {
    for (int i = 0; i < n; i++) {
        int a = in[2 * i], b = in[2 * i + 1], x, y;
        x = a >= '0' && a <= '9' ? a - '0' : a >= 'a' && a <= 'f' ? a - 'a' + 10 : -1;
        y = b >= '0' && b <= '9' ? b - '0' : b >= 'a' && b <= 'f' ? b - 'a' + 10 : -1;
        if (x < 0 || y < 0) return 0;
        out[i] = (uint8_t)(x << 4 | y);
    }
    return 1;
}

/* One entry per line, so a reader needs no JSON library:
 *   {"version": 1, "entries": [
 *   {"k": "1a2b3c4d", "i": 65535, "t": 0, "v": "01"},
 *   ...
 *   ]} */
static int load_table(table *t, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return errno == ENOENT ? 0 : -1;
    static char line[1024];
    char k[9], v[2 * OW_GD_BATCH + 520];
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned idx, kind;
        if (sscanf(line, " {\"k\": \"%8[0-9a-f]\", \"i\": %u, \"t\": %u, \"v\": \"%512[0-9a-f]\"", k, &idx, &kind, v) != 4 &&
            !(sscanf(line, " {\"k\": \"%8[0-9a-f]\", \"i\": %u, \"t\": %u, \"v\": \"\"", k, &idx, &kind) == 3 && !(v[0] = 0)))
            continue;
        size_t vl = strlen(v);
        uint8_t kb[4], vb[256];
        if (strlen(k) != 8 || idx > 0xffff || kind > 8 || (vl & 1) || vl > 512 || !unhex_n(kb, k, 4) ||
            !unhex_n(vb, v, (int)vl / 2))
            continue;
        uint32_t key = (uint32_t)kb[0] << 24 | (uint32_t)kb[1] << 16 | (uint32_t)kb[2] << 8 | kb[3];
        ent *e = t_find(t, key, (uint16_t)idx, 1);
        if (e && ent_set(e, (uint8_t)kind, vb, (int)vl / 2)) n++;
    }
    fclose(f);
    return n;
}

/* backups/<name>.0 is the newest copy; the oldest falls off at BACKUPS. */
static void keep_backup(const char *dir, const char *name) {
    char bdir[600], a[700], b[700], cur[700];
    snprintf(bdir, sizeof bdir, "%s/backups", dir);
    sv_mkdir(bdir);
    snprintf(cur, sizeof cur, "%s/%s", dir, name);
    FILE *probe = fopen(cur, "rb");
    if (!probe) return;
    fclose(probe);
    snprintf(a, sizeof a, "%s/%s.%d", bdir, name, BACKUPS - 1);
    remove(a);
    for (int i = BACKUPS - 2; i >= 0; i--) {
        snprintf(a, sizeof a, "%s/%s.%d", bdir, name, i);
        snprintf(b, sizeof b, "%s/%s.%d", bdir, name, i + 1);
        rename(a, b);
    }
    snprintf(a, sizeof a, "%s/%s.0", bdir, name);
    if (sv_replace_file(cur, a) != 0) sv_log("saves: cannot keep a backup of %s", cur);
}

static int write_table(const table *t, const char *dir, const char *name) {
    char tmp[700], dst[700];
    snprintf(tmp, sizeof tmp, "%s/%s.tmp", dir, name);
    snprintf(dst, sizeof dst, "%s/%s", dir, name);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        sv_log("saves: cannot write %s: %s", tmp, strerror(errno));
        return -1;
    }
    static char v[2 * 256 + 1];
    char k[9];
    fputs("{\"version\": 1, \"entries\": [\n", f);
    uint32_t left = t->n;
    for (uint32_t i = 0; i < t->cap; i++) {
        const ent *e = &t->e[i];
        if (!e->used) continue;
        uint8_t kb[4] = { (uint8_t)(e->key >> 24), (uint8_t)(e->key >> 16), (uint8_t)(e->key >> 8), (uint8_t)e->key };
        hex(k, kb, 4);
        hex(v, e->v, e->len);
        fprintf(f, "{\"k\": \"%s\", \"i\": %u, \"t\": %u, \"v\": \"%s\"}%s\n", k, e->index, e->kind, v, --left ? "," : "");
    }
    fputs("]}\n", f);
    int bad = fflush(f) != 0 || ferror(f);
    if (fclose(f) != 0 || bad) {
        sv_log("saves: cannot write %s", tmp);
        remove(tmp);
        return -1;
    }
    keep_backup(dir, name);
    if (sv_replace_file(tmp, dst) != 0) {
        sv_log("saves: cannot replace %s", dst);
        return -1;
    }
    return 0;
}

/* ---- rules ---- */

static const char *const rule_names[] = { "latest", "forward", "steps", "owner", "player" };

int sv_save_rule(uint32_t key) {
    int lo = 0, hi = S.nrules - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (S.rule_key[m] == key) return S.rule_val[m];
        if (S.rule_key[m] < key) lo = m + 1;
        else hi = m - 1;
    }
    return SV_RULE_LATEST;
}

/* world-keys.tsv: "<8 hex digits key hash>\t<rule name>" per line, '#' starts a comment. tools/world_keys.py makes
 * it from the game's own GameData list and server/data/world-keys.toml; without it every shared key is latest. */
static void load_rules(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        sv_log("saves: no %s, so every shared key follows the latest write", path);
        return;
    }
    char line[256], k[16], r[32];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || sscanf(line, "%15s %31s", k, r) != 2) continue;
        char *end;
        unsigned long key = strtoul(k, &end, 16);
        if (*end || strlen(k) != 8) continue;
        int rule = -1;
        for (int i = 0; i < 5; i++)
            if (!strcmp(r, rule_names[i])) rule = i;
        if (rule < 0) continue;
        if (S.nrules == S.caprules) {
            int c = S.caprules ? S.caprules * 2 : 1024;
            uint32_t *nk = realloc(S.rule_key, (size_t)c * sizeof *nk);
            if (!nk) break;
            S.rule_key = nk;
            uint8_t *nv = realloc(S.rule_val, (size_t)c);
            if (!nv) break;
            S.rule_val = nv, S.caprules = c;
        }
        int i = S.nrules++;
        while (i > 0 && S.rule_key[i - 1] > key) S.rule_key[i] = S.rule_key[i - 1], S.rule_val[i] = S.rule_val[i - 1], i--;
        S.rule_key[i] = (uint32_t)key, S.rule_val[i] = (uint8_t)rule;
    }
    fclose(f);
    sv_log("saves: %d key rules from %s", S.nrules, path);
}

static int64_t as_signed(const uint8_t *v, int len) {
    int64_t x = 0;
    for (int i = len - 1; i >= 0; i--) x = x << 8 | v[i];
    if (len > 0 && len < 8 && (v[len - 1] & 0x80)) x -= (int64_t)1 << (8 * len);
    return x;
}

static uint64_t as_unsigned(const uint8_t *v, int len) {
    uint64_t x = 0;
    for (int i = len - 1; i >= 0; i--) x = x << 8 | v[i];
    return x;
}

static float as_float(const uint8_t *v, int len) {
    float x = 0;
    if (len == 4) memcpy(&x, v, 4);
    return x;
}

static int seen_before(const ent *e, uint64_t fp) {
    for (int i = 0; i < e->nhist; i++)
        if (e->hist[i] == fp) return 1;
    return 0;
}

static void remember(ent *e) {
    e->hist[e->hpos] = fingerprint(e->kind, e->v, e->len);
    e->hpos = (uint8_t)((e->hpos + 1) % HIST);
    if (e->nhist < HIST) e->nhist++;
}

/* Whether a write of (kind, v) to e may go ahead under rule. e has a value. */
static int allowed(int rule, const ent *e, uint8_t kind, const uint8_t *v, int len) {
    if (rule == SV_RULE_LATEST || kind != e->kind) return 1;
    if (rule == SV_RULE_FORWARD) switch (kind) {
        case OW_GD_BOOL: return !(e->len && e->v[0] && !(len && v[0]));
        case OW_GD_S32: case OW_GD_S64: return as_signed(v, len) >= as_signed(e->v, e->len);
        case OW_GD_U32: case OW_GD_U64: return as_unsigned(v, len) >= as_unsigned(e->v, e->len);
        case OW_GD_F32: return as_float(v, len) >= as_float(e->v, e->len);
        default: break; /* text, vectors and bytes have no order: as steps */
        }
    return !seen_before(e, fingerprint(kind, v, len));
}

/* ---- accounts ---- */

static account *acc_of(uint64_t id) {
    for (int i = 0; i < S.nacc; i++)
        if (S.acc[i].id == id) return &S.acc[i];
    if (S.nacc == S.capacc) {
        int c = S.capacc ? S.capacc * 2 : 16;
        account *n = realloc(S.acc, (size_t)c * sizeof *n);
        if (!n) return NULL;
        S.acc = n, S.capacc = c;
    }
    account *a = &S.acc[S.nacc++];
    memset(a, 0, sizeof *a);
    a->id = id;
    char path[700];
    snprintf(path, sizeof path, "%s/accounts/%llu/save.json", S.dir, (unsigned long long)id);
    int n = load_table(&a->t, path);
    if (n < 0) sv_log("saves: cannot read %s; the account starts from its game's own save", path);
    return a;
}

/* ---- sending ---- */

static ow_world_set g_set; /* about 53 KB each: kept out of the stack */
static ow_save_fix g_fix;
static ow_save_part g_part;
static ow_world_part g_wpart;

static void put_entry(ow_gd_entry *d, const ent *e) {
    d->key = e->key, d->index = e->index, d->kind = e->kind;
    d->value_len = e->len;
    memcpy(d->value, e->v, e->len);
}

static void set_flush(uint64_t to, uint64_t except) {
    if (g_set.entries_count) S.send(to, except, OW_T_world_set, &g_set);
    g_set.entries_count = 0;
}

static void set_add(uint64_t to, uint64_t except, const ent *e) {
    put_entry(&g_set.entries[g_set.entries_count++], e);
    if (g_set.entries_count == OW_GD_BATCH) set_flush(to, except);
}

static void fix_add(uint64_t to, const ent *e) {
    put_entry(&g_fix.entries[g_fix.entries_count++], e);
    if (g_fix.entries_count == OW_GD_BATCH) S.send(to, 0, OW_T_save_fix, &g_fix), g_fix.entries_count = 0;
}

static uint16_t parts_of(uint32_t n) { return (uint16_t)(n ? (n + OW_GD_BATCH - 1) / OW_GD_BATCH : 1); }

void sv_save_snapshot(uint64_t id) {
    account *a = acc_of(id);
    if (!a) return;
    memset(&g_part, 0, sizeof g_part);
    g_part.parts = parts_of(a->t.n);
    for (uint32_t i = 0; i < a->t.cap; i++) {
        if (!a->t.e[i].used) continue;
        put_entry(&g_part.entries[g_part.entries_count++], &a->t.e[i]);
        if (g_part.entries_count == OW_GD_BATCH) S.send(id, 0, OW_T_save_part, &g_part), g_part.part++, g_part.entries_count = 0;
    }
    if (g_part.entries_count || g_part.part < g_part.parts) S.send(id, 0, OW_T_save_part, &g_part);
    memset(&g_wpart, 0, sizeof g_wpart);
    g_wpart.parts = parts_of(S.world.n);
    for (uint32_t i = 0; i < S.world.cap; i++) {
        if (!S.world.e[i].used) continue;
        put_entry(&g_wpart.entries[g_wpart.entries_count++], &S.world.e[i]);
        if (g_wpart.entries_count == OW_GD_BATCH)
            S.send(id, 0, OW_T_world_part, &g_wpart), g_wpart.part++, g_wpart.entries_count = 0;
    }
    if (g_wpart.entries_count || g_wpart.part < g_wpart.parts) S.send(id, 0, OW_T_world_part, &g_wpart);
}

/* ---- writes ---- */

static void store_own(account *a, const ow_gd_entry *d) {
    ent *e = t_find(&a->t, d->key, d->index, 1);
    if (!e) return;
    if (e->v && e->kind == d->kind && e->len == d->value_len && !memcmp(e->v, d->value, d->value_len)) return;
    if (ent_set(e, d->kind, d->value, d->value_len)) a->dirty = 1;
}

void sv_save_on_save_write(uint64_t id, const ow_save_write *m) {
    account *a = acc_of(id);
    if (!a) return;
    for (int i = 0; i < m->entries_count; i++) store_own(a, &m->entries[i]);
}

static int is_owner(uint64_t id) {
    if (!S.owner) {
        for (int i = 0; i < S.nacc; i++)
            if (S.acc[i].live && S.acc[i].host) S.owner = S.acc[i].id;
        if (!S.owner) S.owner = id; /* the first live game to write an owner key */
    }
    return S.owner == id;
}

void sv_save_on_world_write(uint64_t id, const ow_world_write *m) {
    account *a = acc_of(id);
    if (!a) return;
    g_set.source = id, g_set.entries_count = 0;
    static ow_gd_entry back[OW_GD_BATCH]; /* copied now: a later entry of the batch may replace e's value */
    int nback = 0;
    for (int i = 0; i < m->entries_count; i++) {
        const ow_gd_entry *d = &m->entries[i];
        int rule = sv_save_rule(d->key);
        if (rule == SV_RULE_PLAYER) { /* the game thought it shared: it stays the writer's own */
            store_own(a, d);
            continue;
        }
        ent *e = t_find(&S.world, d->key, d->index, 0);
        if (e && e->kind == d->kind && e->len == d->value_len && !memcmp(e->v, d->value, d->value_len)) continue;
        if (e && !(rule == SV_RULE_OWNER ? is_owner(id) : allowed(rule, e, d->kind, d->value, d->value_len))) {
            put_entry(&back[nback++], e); /* the writer's game goes back to the room's value */
            continue;
        }
        if (!e && rule == SV_RULE_OWNER && !is_owner(id)) continue;
        if (!e) e = t_find(&S.world, d->key, d->index, 1);
        if (!e) continue;
        if (e->v && rule == SV_RULE_STEPS) remember(e);
        if (!ent_set(e, d->kind, d->value, d->value_len)) continue;
        S.world_dirty = 1;
        set_add(0, id, e);
    }
    set_flush(0, id);
    g_set.source = 0;
    memcpy(g_set.entries, back, (size_t)nback * sizeof *back);
    g_set.entries_count = (uint16_t)nback;
    set_flush(id, 0);
}

/* ---- the feature API ---- */

static int get_from(table *t, uint32_t key, uint16_t index, uint8_t *kind, uint8_t *out, int cap) {
    ent *e = t_find(t, key, index, 0);
    if (!e) return -1;
    if (kind) *kind = e->kind;
    if (out) memcpy(out, e->v, (size_t)(e->len < cap ? e->len : cap));
    return e->len;
}

int sv_save_get(uint64_t id, uint32_t key, uint16_t index, uint8_t *kind, uint8_t *out, int cap) {
    account *a = acc_of(id);
    return a ? get_from(&a->t, key, index, kind, out, cap) : -1;
}

int sv_world_get(uint32_t key, uint16_t index, uint8_t *kind, uint8_t *out, int cap) {
    return get_from(&S.world, key, index, kind, out, cap);
}

void sv_save_put(uint64_t id, uint32_t key, uint16_t index, uint8_t kind, const uint8_t *v, int len) {
    account *a = acc_of(id);
    ent *e = a ? t_find(&a->t, key, index, 1) : NULL;
    if (!e || len < 0 || len > 256 || !ent_set(e, kind, v, len)) return;
    a->dirty = 1;
    g_fix.seq = 0, g_fix.entries_count = 0;
    fix_add(id, e);
    if (g_fix.entries_count) S.send(id, 0, OW_T_save_fix, &g_fix);
}

void sv_world_put(uint32_t key, uint16_t index, uint8_t kind, const uint8_t *v, int len) {
    ent *e = t_find(&S.world, key, index, 1);
    if (!e || len < 0 || len > 256) return;
    if (e->v && sv_save_rule(key) == SV_RULE_STEPS) remember(e);
    if (!ent_set(e, kind, v, len)) return;
    S.world_dirty = 1;
    g_set.source = 0, g_set.entries_count = 0;
    set_add(0, 0, e);
    set_flush(0, 0);
}

/* ---- owner ---- */

void sv_save_live(uint64_t id, int live, int is_host) {
    account *a = acc_of(id);
    if (!a) return;
    a->live = live, a->host = is_host;
    if (live && is_host) S.owner = id;
    else if (!live && S.owner == id) S.owner = 0; /* picked again at the next owner write */
}

/* ---- open, flush, close ---- */

int sv_save_open(const char *dir, sv_save_send_fn send) {
    memset(&S, 0, sizeof S);
    snprintf(S.dir, sizeof S.dir, "%s", dir);
    S.send = send;
    char path[700];
    snprintf(path, sizeof path, "%s/accounts", dir);
    sv_mkdir(path);
    snprintf(path, sizeof path, "%s/world-keys.tsv", dir);
    load_rules(path);
    snprintf(path, sizeof path, "%s/world.json", dir);
    int n = load_table(&S.world, path);
    if (n < 0) {
        sv_log("saves: cannot read %s", path);
        return -1;
    }
    sv_log("saves: the world has %d values", n);
    return 0;
}

void sv_save_flush(uint64_t now_us, int force) {
    if (!force && now_us - S.last_flush < FLUSH_EVERY_US) return;
    S.last_flush = now_us;
    if (S.world_dirty && write_table(&S.world, S.dir, "world.json") == 0) S.world_dirty = 0;
    for (int i = 0; i < S.nacc; i++) {
        account *a = &S.acc[i];
        if (!a->dirty) continue;
        char d[640];
        snprintf(d, sizeof d, "%s/accounts/%llu", S.dir, (unsigned long long)a->id);
        sv_mkdir(d);
        if (write_table(&a->t, d, "save.json") == 0) a->dirty = 0;
    }
}

void sv_save_close(void) {
    sv_save_flush(0, 1);
    t_free(&S.world);
    for (int i = 0; i < S.nacc; i++) t_free(&S.acc[i].t);
    free(S.acc);
    free(S.rule_key);
    free(S.rule_val);
    memset(&S, 0, sizeof S);
}
