/* Offline check of server/sv_save.c: the shared-key rules, per-account saves, the join snapshot, the feature API and
 * the files (atomic rewrite, reload, 20 backups). Built and run by tests/sv_save_check.py; no network. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../server/sv.h"
#include "../server/sv_save.h"

void sv_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (getenv("SV_SAVE_CHECK_LOG")) vfprintf(stderr, fmt, ap), fputc('\n', stderr);
    va_end(ap);
}

typedef struct { uint64_t to, except; uint16_t type; uint64_t source; int n; uint32_t key0; uint8_t v0[8]; int parts; } sent;
static sent out[256];
static int nout, fails;

static void on_send(uint64_t to, uint64_t except, uint16_t type, const void *msg) {
    sent *s = &out[nout++ % 256];
    memset(s, 0, sizeof *s);
    s->to = to, s->except = except, s->type = type;
    const ow_gd_entry *e = NULL;
    if (type == OW_T_world_set) s->source = ((const ow_world_set *)msg)->source, s->n = ((const ow_world_set *)msg)->entries_count, e = ((const ow_world_set *)msg)->entries;
    if (type == OW_T_save_fix) s->n = ((const ow_save_fix *)msg)->entries_count, e = ((const ow_save_fix *)msg)->entries;
    if (type == OW_T_save_part) s->n = ((const ow_save_part *)msg)->entries_count, e = ((const ow_save_part *)msg)->entries, s->parts = ((const ow_save_part *)msg)->parts;
    if (type == OW_T_world_part) s->n = ((const ow_world_part *)msg)->entries_count, e = ((const ow_world_part *)msg)->entries, s->parts = ((const ow_world_part *)msg)->parts;
    if (e && s->n) s->key0 = e[0].key, memcpy(s->v0, e[0].value, e[0].value_len < 8 ? e[0].value_len : 8);
}

#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static ow_world_write ww;
static ow_save_write sw;

static void entry(ow_gd_entry *d, uint32_t key, uint8_t kind, uint64_t v, int len) {
    memset(d, 0, sizeof *d);
    d->key = key, d->index = 0xFFFF, d->kind = kind, d->value_len = (uint16_t)len;
    memcpy(d->value, &v, (size_t)len);
}

static void world(uint64_t who, uint32_t key, uint8_t kind, uint64_t v, int len) {
    ww.seq++, ww.entries_count = 1;
    entry(&ww.entries[0], key, kind, v, len);
    nout = 0;
    sv_save_on_world_write(who, &ww);
}

static int64_t wval(uint32_t key) {
    uint8_t b[8] = { 0 }, k;
    if (sv_world_get(key, 0xFFFF, &k, b, 8) < 0) return -999;
    int64_t x;
    memcpy(&x, b, 8);
    return x;
}

/* Rule table of the check: keys by first digit. */
enum { K_LATEST = 0x10000001, K_FWD_BOOL = 0x20000001, K_FWD_S32 = 0x20000002, K_FWD_U32 = 0x20000003,
       K_STEPS = 0x30000001, K_OWNER = 0x40000001, K_PLAYER = 0x50000001 };

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    const char *dir = argv[1];
    char path[600];
    snprintf(path, sizeof path, "%s/world-keys.tsv", dir);
    FILE *f = fopen(path, "w");
    fprintf(f, "# check table\n20000001\tforward\n20000002\tforward\n20000003\tforward\n30000001\tsteps\n"
               "40000001\towner\n50000001\tplayer\nzzzz\tforward\n");
    fclose(f);
    CHECK(sv_save_open(dir, on_send) == 0, "open");
    CHECK(sv_save_rule(K_FWD_BOOL) == SV_RULE_FORWARD && sv_save_rule(K_LATEST) == SV_RULE_LATEST &&
          sv_save_rule(K_PLAYER) == SV_RULE_PLAYER, "rules read");

    const uint64_t A = 7, B = 9; /* A is the host */
    sv_save_live(A, 1, 1);
    sv_save_live(B, 1, 0);

    /* latest: goes to everyone but the writer, with the writer as source */
    world(B, K_LATEST, OW_GD_S32, 5, 4);
    CHECK(nout == 1 && out[0].type == OW_T_world_set && out[0].to == 0 && out[0].except == B && out[0].source == B,
          "latest broadcast (%d)", nout);
    world(A, K_LATEST, OW_GD_S32, 3, 4);
    CHECK(wval(K_LATEST) == 3, "latest takes a lower value");
    world(A, K_LATEST, OW_GD_S32, 3, 4);
    CHECK(nout == 0, "an unchanged value is not sent again");

    /* forward bool: true stays true; the writer of false gets the room's true back */
    world(A, K_FWD_BOOL, OW_GD_BOOL, 1, 1);
    world(B, K_FWD_BOOL, OW_GD_BOOL, 0, 1);
    CHECK(wval(K_FWD_BOOL) == 1, "forward bool kept true");
    CHECK(nout == 1 && out[0].to == B && out[0].source == 0 && out[0].v0[0] == 1, "forward correction to the writer");

    /* forward numbers: signed and unsigned by kind */
    world(A, K_FWD_S32, OW_GD_S32, 4, 4);
    world(B, K_FWD_S32, OW_GD_S32, (uint32_t)-1, 4);
    CHECK(wval(K_FWD_S32) == 4, "forward s32 refuses -1 over 4");
    world(B, K_FWD_S32, OW_GD_S32, 6, 4);
    CHECK(wval(K_FWD_S32) == 6, "forward s32 takes 6");
    world(A, K_FWD_U32, OW_GD_U32, 4, 4);
    world(B, K_FWD_U32, OW_GD_U32, 0xFFFFFFFFu, 4);
    CHECK((uint32_t)wval(K_FWD_U32) == 0xFFFFFFFFu, "forward u32 is unsigned");

    /* steps: a quest step never goes back to an earlier value, in any order */
    world(A, K_STEPS, OW_GD_U32, 0xAAAA, 4);
    world(A, K_STEPS, OW_GD_U32, 0x1111, 4);
    world(B, K_STEPS, OW_GD_U32, 0xAAAA, 4);
    CHECK(wval(K_STEPS) == 0x1111 && nout == 1 && out[0].to == B, "steps refuses an earlier step");
    world(B, K_STEPS, OW_GD_U32, 0x2222, 4);
    CHECK(wval(K_STEPS) == 0x2222, "steps takes a new step");

    /* owner: the host's game while it is live, then the first other game that writes */
    world(B, K_OWNER, OW_GD_S32, 11, 4);
    CHECK(wval(K_OWNER) == -999, "owner key from a guest while the host plays");
    world(A, K_OWNER, OW_GD_S32, 12, 4);
    CHECK(wval(K_OWNER) == 12, "owner key from the host");
    world(B, K_OWNER, OW_GD_S32, 13, 4);
    CHECK(wval(K_OWNER) == 12 && nout == 1 && out[0].to == B, "guest corrected to the owner's value");
    sv_save_live(A, 0, 0);
    world(B, K_OWNER, OW_GD_S32, 14, 4);
    CHECK(wval(K_OWNER) == 14, "a guest owns the world while the host is away");
    sv_save_live(A, 1, 1);
    world(B, K_OWNER, OW_GD_S32, 15, 4);
    CHECK(wval(K_OWNER) == 14, "the host takes the world back");

    /* player rule through world_write stays the writer's own */
    world(B, K_PLAYER, OW_GD_S32, 300, 4);
    uint8_t b[8] = { 0 }, k;
    CHECK(nout == 0 && wval(K_PLAYER) == -999 && sv_save_get(B, K_PLAYER, 0xFFFF, &k, b, 8) == 4 && b[0] == 44,
          "player key kept in its writer's save");

    /* save_write: each account its own */
    sw.entries_count = 1;
    entry(&sw.entries[0], 0x60000001, OW_GD_S32, 100, 4);
    sv_save_on_save_write(A, &sw);
    entry(&sw.entries[0], 0x60000001, OW_GD_S32, 200, 4);
    sv_save_on_save_write(B, &sw);
    CHECK(sv_save_get(A, 0x60000001, 0xFFFF, &k, b, 8) == 4 && b[0] == 100, "A keeps its own");
    CHECK(sv_save_get(B, 0x60000001, 0xFFFF, &k, b, 8) == 4 && b[0] == 200, "B keeps its own");

    /* the feature API: a put to an account sends it save_fix */
    nout = 0;
    int32_t rupees = 999;
    sv_save_put(A, 0x70000001, 0xFFFF, OW_GD_S32, (uint8_t *)&rupees, 4);
    CHECK(nout == 1 && out[0].type == OW_T_save_fix && out[0].to == A, "save_fix on put");

    /* snapshot: the account's save, then the world */
    nout = 0;
    sv_save_snapshot(B);
    CHECK(nout == 2 && out[0].type == OW_T_save_part && out[0].to == B && out[0].n == 2 && out[0].parts == 1 &&
          out[1].type == OW_T_world_part && out[1].n == 6, "snapshot (%d messages, %d, %d)", nout, out[0].n, out[1].n);
    nout = 0;
    sv_save_snapshot(12345);
    CHECK(nout == 2 && out[0].n == 0 && out[0].parts == 1, "an empty save still sends its one part");

    /* big batch: more than one part */
    ww.entries_count = OW_GD_BATCH;
    for (int i = 0; i < OW_GD_BATCH; i++) {
        entry(&ww.entries[i], 0x80000000u + (uint32_t)i, OW_GD_S32, (uint64_t)i, 4);
    }
    nout = 0;
    sv_save_on_world_write(A, &ww);
    nout = 0;
    sv_save_snapshot(A);
    CHECK(nout == 3 && out[1].type == OW_T_world_part && out[1].parts == 2 && out[1].n == OW_GD_BATCH && out[2].n == 6,
          "world in two parts (%d)", nout);

    /* files: flush, reopen, backups */
    for (int round = 0; round < 23; round++) {
        world(A, K_LATEST, OW_GD_S32, 1000 + (uint64_t)round, 4);
        sv_save_flush(0, 1);
    }
    sv_save_close();
    CHECK(sv_save_open(dir, on_send) == 0, "reopen");
    CHECK(wval(K_LATEST) == 1022 && wval(K_FWD_BOOL) == 1, "world read back");
    CHECK(sv_save_get(B, 0x60000001, 0xFFFF, &k, b, 8) == 4 && b[0] == 200, "save read back");
    snprintf(path, sizeof path, "%s/backups/world.json.19", dir);
    f = fopen(path, "r");
    CHECK(f != NULL, "20 backups kept");
    if (f) fclose(f);
    snprintf(path, sizeof path, "%s/backups/world.json.20", dir);
    f = fopen(path, "r");
    CHECK(f == NULL, "no 21st backup");
    if (f) fclose(f);
    snprintf(path, sizeof path, "%s/world.json.tmp", dir);
    f = fopen(path, "r");
    CHECK(f == NULL, "no temporary file left");
    if (f) fclose(f);
    sv_save_close();

    printf(fails ? "sv_save_check: %d failed\n" : "sv_save_check: all passed\n", fails);
    return fails != 0;
}
