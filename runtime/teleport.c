// Teleport: writes the player actor's position (+0x2b4, three floats; frame_rate.c, player_actor) once per presented
// frame for HOLD_FRAMES frames. A single store can lose against the game's own update of that field in the same
// frame, so the target is written again each present until the actor's position has stayed there for SETTLE_FRAMES
// reads in a row. The actor is looked up again every frame: a load that replaces it ends the move. Log lines
// ("teleport: ...") report the target, the read-back of each frame while the move runs, and how it ended.
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "cfg.h"
#include "dispatch.h"
#include "host.h"
#include "layout.h"
#include "platform/platform.h"
#include "teleport.h"

#if __has_include("patches.h")
#include "patches.h"
#endif

#define POS_OFF 0x2b4
#define HOLD_FRAMES 90      // give up after this many presents
#define SETTLE_FRAMES 6     // reads within SETTLE_DIST of the target before the move counts as done
#define SETTLE_DIST 1.5f
#define WAIT_ACTOR_FRAMES 300   // presents to wait for a resident player before dropping the move

// Where the player's position lives: the actor's own copies and a structure its pointer at +0x1c0 leads to (found with
// TOTK_TELEPORT_SCAN on 1.2.1). A slot is written only when it holds the player's current position at the start of
// the move, so an offset that means something else in another game version is left alone.
static const struct { uint64_t ptr, off; float dy; } kSlots[] = {
    {0, 0x2b4, 0}, {0, 0x284, 0}, {0x1c0, 0xb0, 0}, {0x1c0, 0xe0, 0}, {0x238, 0x2e4, 0.9f}, {0x238, 0x2fc, 1.5f},
    {0x238, 0x308, 0.9f}, {0x608, 0x240, 0.9f}};
#define NSLOTS (sizeof kSlots / sizeof *kSlots)
static int g_slot_on[NSLOTS];

static uint64_t slot_addr(uint64_t actor, int i) {
    if (!kSlots[i].ptr) return actor + kSlots[i].off;
    uint64_t p = 0;
    if (!plat_mem_read(actor + kSlots[i].ptr, &p, 8) || p < 0x10000 || p == actor) return 0;
    return p + kSlots[i].off;
}

static int near3(const float a[3], const float b[3], float dy) {
    return fabsf(a[0] - b[0]) < 0.05f && fabsf(a[1] - b[1] - dy) < 0.05f && fabsf(a[2] - b[2]) < 0.05f;
}

static float g_target[3];
static int g_frames_left;   // presents left to keep writing
static int g_wait;          // presents spent waiting for an actor
static int g_settled;
static int g_active;

// TOTK_TELEPORT_SCAN=1: at each teleport, walks the objects reachable from the player actor through pointers (a few levels
// deep) and logs every place that holds the player's position (x and z within 3 cm, y within 3 m), with the chain of
// offsets that leads there, to find the copy the game reads the position from.
#define SCAN_MAX 6000
static struct { uint64_t addr; float dy; } g_dyn[400];
static int g_dyn_n, g_dyn_all;
static uint64_t g_seen[SCAN_MAX];
static unsigned g_chain[SCAN_MAX][4];
static unsigned g_depth[SCAN_MAX];

static void scan_copies(uint64_t actor, const float pos[3]) {
    int n = 0, hits = 0;
    g_seen[n] = actor, g_depth[n] = 0, n++;
    g_dyn_n = 0;
    for (int i = 0; i < n && hits < 120; i++) {
        uint64_t o = g_seen[i];
        for (uint64_t off = 0; off < 0x800; off += 4) {
            float f[3];
            if (!plat_mem_read(o + off, f, 12)) break;
            if (fabsf(f[0] - pos[0]) < 0.03f && fabsf(f[2] - pos[2]) < 0.03f && fabsf(f[1] - pos[1]) < 3.0f && hits < 120) {
                char path[96] = "actor";
                for (unsigned d = 0; d < g_depth[i]; d++) snprintf(path + strlen(path), sizeof path - strlen(path), "->0x%x", g_chain[i][d]);
                rt_log("teleport: copy at %s +0x%llx = %.2f %.2f %.2f", path, (unsigned long long)off, f[0], f[1], f[2]);
                hits++;
                if (g_dyn_n < 400) g_dyn[g_dyn_n].addr = o + off, g_dyn[g_dyn_n++].dy = f[1] - pos[1];
            }
            uint64_t p = 0;
            if (!(off & 7) && g_depth[i] < 3 && n < SCAN_MAX && plat_mem_read(o + off, &p, 8) && p > 0x100000 && p < 0x7fffffffffffull) {
                int dup = 0;
                for (int k = 0; k < n && !dup; k++) dup = g_seen[k] == p;
                if (!dup) {
                    g_seen[n] = p, g_depth[n] = g_depth[i] + 1;
                    memcpy(g_chain[n], g_chain[i], sizeof g_chain[n]);
                    g_chain[n][g_depth[i]] = (unsigned)off;
                    n++;
                }
            }
        }
    }
    rt_log("teleport: scanned %d objects, %d copies", n, hits);
}

// TOTK_TELEPORT_WATCH=<offset>: at a teleport request, writes nothing and instead arms a hardware write watchpoint on
// the four bytes at actor+<offset> (hex) for WATCH_FRAMES presents, then logs each distinct writer as host offsets of
// the executable (pc after the store, then its callers), to look up in build/<ver>/totk.map.
#define WATCH_FRAMES 30
#define WATCH_MAX 24
static struct { uint64_t pcs[8]; int n; volatile int hits; } g_watch[WATCH_MAX];
static volatile int g_watch_n;
static int g_watch_left;

static void watch_hit(const uint64_t *pcs, int n) {
    int k = __atomic_load_n(&g_watch_n, __ATOMIC_ACQUIRE);
    for (int i = 0; i < k; i++)
        if (g_watch[i].n == n && !memcmp(g_watch[i].pcs, pcs, (size_t)(n < 4 ? n : 4) * 8)) {
            __atomic_add_fetch(&g_watch[i].hits, 1, __ATOMIC_RELAXED);
            return;
        }
    int slot = __atomic_fetch_add(&g_watch_n, 1, __ATOMIC_ACQ_REL);
    if (slot >= WATCH_MAX) return;
    memcpy(g_watch[slot].pcs, pcs, (size_t)n * 8);
    g_watch[slot].n = n;
    g_watch[slot].hits = 1;
}

static void watch_report(void) {
    plat_watch_write(0, 0, NULL);
    uint64_t base = plat_exe_base();
    int k = g_watch_n < WATCH_MAX ? g_watch_n : WATCH_MAX;
    rt_log("teleport: watch saw %d writers", g_watch_n);
    for (int i = 0; i < k; i++) {
        char line[256];
        int o = snprintf(line, sizeof line, "%d hits:", g_watch[i].hits);
        for (int j = 0; j < g_watch[i].n && o < (int)sizeof line - 20; j++)
            o += snprintf(line + o, sizeof line - o, " %llx", (unsigned long long)(g_watch[i].pcs[j] - base));
        rt_log("teleport: watch %s", line);
    }
}

// ---- the set-transform hook (recomp/patches-v121.txt "teleport"): main+0x82133c calls the actor's set-transform with
// the matrix x26 points to; the host callback at page +0xf60 makes that load and, for the actor held here, passes a copy
// with the target translation instead (row-major 3x4, translation in elements 3, 7 and 11).
#define HOST_TELEPORT_SET (RT_PATCH_PAGE + 0xf60)
#define HOOK_MTX_OFF 0xe00
static uint64_t g_hook_actor;   // __atomic builtins: written on the present thread, read by the guest
static float g_hook_pos[3];
static int g_hook_hits;
static uint64_t g_player_seen;   // the player actor of the last present
static int g_hook_calls;         // set-transform calls for the player: its update ran

static uint64_t read_p(uint64_t a) {
    uint64_t p = 0;
    return a && plat_mem_read(a, &p, 8) && p >= 0x10000 ? p : 0;
}
static uint64_t player_body(uint64_t actor);
// The physics world the body lives in, as main+0xaa9b90 finds it: [[[main+0x462e038]] +0xc8 + 8 * (B+0x68 bit 5)]
// +0xc0, then +0xe0. Its vtable +0x1d0 (world, body id [H+8]) returns the body's linear velocity, writable in place.
#define WORLD_LINEAR_VELOCITY 0x1d0
static uint64_t body_world(uint64_t body) {
    uint64_t flags = 0, g = read_p(read_p(MOD_MAIN_BASE + 0x462e038));
    if (!g || !plat_mem_read(body + 0x68, &flags, 8)) return 0;
    uint64_t w = read_p(g + 0xc8);
    w = w ? read_p(w + ((flags >> 5) & 1) * 8 + 0xc0) : 0;
    return w ? read_p(w + 0xe0) : 0;
}
#define BODY_SET_TRANSFORM 0xaa9b90   // main+: (body handle [B+0x70], const row-major 3x4 *, B)

// Calls body_set_transform on this guest thread with a matrix built on the guest stack below sp; every register the
// call may change is put back, so the interrupted code resumes as it was.
static void push_body(Cpu *c, uint64_t body, const float pos[3]) {
    uint64_t handle = read_p(body + 0x70);
    if (!handle) return;
    float m[12];
    memcpy(m, (const void *)(uintptr_t)(body + 0x98), sizeof m);
    m[3] = pos[0], m[7] = pos[1], m[11] = pos[2];
    static __thread Cpu saved;
    memcpy(&saved, c, sizeof saved);
    c->sp = (c->sp - 0x100) & ~15ull;
    memcpy((void *)(uintptr_t)c->sp, m, sizeof m);
    rt_call(c, MOD_MAIN_BASE + BODY_SET_TRANSFORM, 3, handle, c->sp, body);
    uint64_t world = body_world(body), id = 0, get_vel = world ? read_p(read_p(world) + WORLD_LINEAR_VELOCITY) : 0;
    plat_mem_read(handle + 8, &id, 4);
    if (get_vel && cfg_get("TOTK_TELEPORT_ZEROVEL")) {
        uint64_t v = rt_call(c, get_vel, 2, world, id);
        if (v >= 0x10000) memset((void *)(uintptr_t)v, 0, 12);
    }
    memcpy(c->x, saved.x, sizeof c->x);
    memcpy(c->v, saved.v, sizeof c->v);
    c->sp = saved.sp, c->nzcv = saved.nzcv, c->pc = saved.pc;
    float *t = (float *)(uintptr_t)(body + 0x98);
    t[3] = pos[0], t[7] = pos[1], t[11] = pos[2];
}

static void set_transform_hook(Cpu *c) {
    c->x[1] = c->x[26];
    if (c->x[0] && c->x[0] == __atomic_load_n(&g_player_seen, __ATOMIC_RELAXED)) __atomic_add_fetch(&g_hook_calls, 1, __ATOMIC_RELAXED);
    uint64_t want = __atomic_load_n(&g_hook_actor, __ATOMIC_ACQUIRE);
    if (!want || c->x[0] != want || !c->x[26]) return;
    uint64_t body = player_body(want);
    if (body) push_body(c, body, g_hook_pos);
    float *m = (float *)(uintptr_t)(RT_PATCH_PAGE + HOOK_MTX_OFF);
    memcpy(m, (const void *)(uintptr_t)c->x[26], 48);
    m[3] = g_hook_pos[0], m[7] = g_hook_pos[1], m[11] = g_hook_pos[2];
    c->x[1] = (uint64_t)(uintptr_t)m;
    __atomic_add_fetch(&g_hook_hits, 1, __ATOMIC_RELAXED);
}

__attribute__((constructor)) static void teleport_register(void) { rt_register_host(HOST_TELEPORT_SET, set_transform_hook); }

// The player's physics body: [[[actor+0x228]+0x50]+0x20] is the object main+0x8250f8 reads the transform from, its
// +0x150 the body whose +0x98 holds it as a row-major 3x4 matrix. 0 when a link is missing.
static uint64_t player_body(uint64_t actor) {
    uint64_t p = read_p(actor + 0x228);
    p = p ? read_p(p + 0x50) : 0;
    p = p ? read_p(p + 0x20) : 0;
    return p ? read_p(p + 0x150) : 0;
}

static int in_world(const float p[3]) {
    // TotK's map is about 10 km wide (x and z within +-5000); the sky islands reach 1500 m up and the Depths 1200 m down
    return isfinite(p[0]) && isfinite(p[1]) && isfinite(p[2]) && fabsf(p[0]) <= 5100.f && fabsf(p[2]) <= 5100.f &&
           p[1] >= -1300.f && p[1] <= 2100.f;
}

int teleport_player_pos(float xyz[3]) {
    uint64_t a = frame_step_player_actor();
    return a && plat_mem_read(a + POS_OFF, xyz, 12) && in_world(xyz);
}

int teleport_to(const float xyz[3]) {
    if (g_active || !in_world(xyz)) return 0;
    memcpy(g_target, xyz, sizeof g_target);
    g_active = 1;
    g_frames_left = HOLD_FRAMES;
    g_wait = g_settled = 0;
    rt_log("teleport: to %.1f %.1f %.1f", xyz[0], xyz[1], xyz[2]);
    const char *scan = cfg_get("TOTK_TELEPORT_SCAN"), *watch = cfg_get("TOTK_TELEPORT_WATCH");
    uint64_t a = frame_step_player_actor();
    if (a && cfg_get("TOTK_TELEPORT_VT")) {
        uint64_t b = player_body(a), flags = 0, g = read_p(read_p(MOD_MAIN_BASE + 0x462e038));
        plat_mem_read(b + 0x68, &flags, 8);
        uint64_t w = g ? read_p(g + 0xc8) : 0;
        w = w ? read_p(w + ((flags >> 5) & 1) * 8 + 0xc0) : 0;
        w = w ? read_p(w + 0xe0) : 0;
        uint64_t h = read_p(b + 0x70), vt = read_p(h);
        uint32_t id = 0;
        if (h) plat_mem_read(h + 8, &id, 4);
        rt_log("teleport: body %llx flags %llx world %llx vt main+%llx id %x", (unsigned long long)b, (unsigned long long)flags,
               (unsigned long long)w, (unsigned long long)(vt - MOD_MAIN_BASE), id);
        for (int i = 0; vt && i < 0x30; i++) {
            uint64_t f = read_p(vt + (uint64_t)i * 8);
            rt_log("teleport: vt +%x main+%llx", i * 8, (unsigned long long)(f - MOD_MAIN_BASE));
        }
    }
    if (a && watch && *watch) {
        uint64_t at = (*watch == 'b' ? player_body(a) : a) + strtoull(watch + (*watch == 'b'), NULL, 16);
        g_watch_n = 0;
        g_watch_left = WATCH_FRAMES;
        rt_log("teleport: watching writes to %llx: %s", (unsigned long long)at, plat_watch_write(at, 4, watch_hit) ? "armed" : "failed");
    }
    float now[3];
    if (a && plat_mem_read(a + POS_OFF, now, 12)) {
        g_dyn_all = scan && !strcmp(scan, "all");
        if (scan && (!strcmp(scan, "1") || g_dyn_all)) scan_copies(a, now);
        for (unsigned i = 0; i < NSLOTS; i++) {
            float v[3];
            uint64_t s = slot_addr(a, (int)i);
            g_slot_on[i] = s && plat_mem_read(s, v, 12) && near3(v, now, kSlots[i].dy);
        }
        rt_log("teleport: slots written: %d %d %d %d %d %d %d %d", g_slot_on[0], g_slot_on[1], g_slot_on[2], g_slot_on[3],
               g_slot_on[4], g_slot_on[5], g_slot_on[6], g_slot_on[7]);
    }
    return 1;
}

int teleport_busy(void) { return g_active; }

// TOTK_TELEPORT_DUMP=<dir>: every DUMP_EVERY presents in the world, saves the player actor's first 0x2000 bytes and
// 0x800 bytes of each object an 8-aligned pointer in them leads to as <dir>/<world ms>.bin, to find the fields that
// tell the player's states apart (tools: compare the files of known states). Read only.
#define DUMP_EVERY 15
static char g_dump_shot[600];   // a picture to save at this present, for the dump's timeline
static void dump_actor(uint64_t a) {
    static const char *dir;
    static int n;
    if (!dir) dir = cfg_get("TOTK_TELEPORT_DUMP");
    if (!dir || !*dir || ++n % DUMP_EVERY) return;
    int64_t w = frame_step_world_us();
    char path[600];
    snprintf(path, sizeof path, "%s/%010lld.bin", dir, (long long)(w / 1000));
    FILE *f = fopen(path, "wb");
    if (!f) return;
    static uint8_t blk[0x2000], sub[0x800];
    if (!plat_mem_read(a, blk, sizeof blk)) memset(blk, 0, sizeof blk);
    fwrite(&a, 8, 1, f);
    fwrite(blk, 1, sizeof blk, f);
    for (uint32_t off = 0; off < sizeof blk; off += 8) {
        uint64_t p;
        memcpy(&p, blk + off, 8);
        if (p < 0x100000 || p > 0x7fffffffffffull || !plat_mem_read(p, sub, sizeof sub)) continue;
        fwrite(&off, 4, 1, f);
        fwrite(&p, 8, 1, f);
        fwrite(sub, 1, sizeof sub, f);
    }
    fclose(f);
    // the strings the pointers in each of those objects lead to (names of states, resources), and the update count
    snprintf(path, sizeof path, "%s/%010lld.txt", dir, (long long)(w / 1000));
    f = fopen(path, "w");
    if (!f) return;
    float pos[3] = {0};
    plat_mem_read(a + POS_OFF, pos, 12);
    fprintf(f, "calls %d pos %.2f %.2f %.2f\n", __atomic_load_n(&g_hook_calls, __ATOMIC_RELAXED), pos[0], pos[1], pos[2]);
    for (uint32_t off = 0; off < sizeof blk; off += 8) {
        uint64_t p;
        memcpy(&p, blk + off, 8);
        if (p < 0x100000 || p > 0x7fffffffffffull || !plat_mem_read(p, sub, sizeof sub)) continue;
        for (uint32_t o2 = 0; o2 < sizeof sub; o2 += 8) {
            uint64_t q;
            char str[49] = {0};
            memcpy(&q, sub + o2, 8);
            if (q < 0x100000 || q > 0x7fffffffffffull || !plat_mem_read(q, str, 48)) continue;
            int k = 0;
            while (k < 48 && str[k] >= 32 && str[k] < 127) k++;
            if (k >= 4 && (k == 48 || !str[k])) fprintf(f, "%x %x %s\n", off, o2, str);
        }
    }
    fclose(f);
    if (n % (DUMP_EVERY * 10) == 0) snprintf(g_dump_shot, sizeof g_dump_shot, "%s/%010lld.png", dir, (long long)(w / 1000));
}

void teleport_frame(void) {
    if (g_watch_left && !--g_watch_left) watch_report();
    {
        uint64_t a = frame_step_player_actor();
        __atomic_store_n(&g_player_seen, a, __ATOMIC_RELAXED);
        if (a && frame_step_world_us() >= 0) dump_actor(a);
    }
    if (!g_active) return;
    uint64_t a = frame_step_player_actor();
    if (!a || !plat_mem_readable(a + POS_OFF, 12)) {
        if (++g_wait > WAIT_ACTOR_FRAMES) {
            g_active = 0;
            rt_log("teleport: no player actor, dropped");
        }
        return;
    }
    float now[3];
    plat_mem_read(a + POS_OFF, now, 12);
    if (cfg_get("TOTK_TELEPORT_HOOK")) {
        int f = HOLD_FRAMES - g_frames_left, hold = atoi(cfg_get("TOTK_TELEPORT_HOOK"));
        memcpy(g_hook_pos, g_target, sizeof g_hook_pos);
        __atomic_store_n(&g_hook_actor, f < hold ? a : 0, __ATOMIC_RELEASE);
        float v[3] = {0};
        plat_mem_read(a + 0x338, v, 12);
        rt_log("teleport: hook frame %d %s hits %d read %.2f %.2f %.2f vel %.2f %.2f %.2f", f, f < hold ? "hold" : "free",
               g_hook_hits, now[0], now[1], now[2], v[0], v[1], v[2]);
        if (--g_frames_left <= 0) g_active = 0, __atomic_store_n(&g_hook_actor, 0, __ATOMIC_RELEASE);
        return;
    }
    float d = hypotf(hypotf(now[0] - g_target[0], now[1] - g_target[1]), now[2] - g_target[2]);
    g_settled = d <= SETTLE_DIST ? g_settled + 1 : 0;
    if (HOLD_FRAMES - g_frames_left < 12)
        rt_log("teleport: frame %d read %.1f %.1f %.1f (%.1f m from the target)", HOLD_FRAMES - g_frames_left, now[0],
               now[1], now[2], d);
    if (g_settled >= SETTLE_FRAMES || --g_frames_left <= 0) {
        rt_log("teleport: %s after %d presents", g_settled >= SETTLE_FRAMES ? "done" : "gave up", HOLD_FRAMES - g_frames_left);
        g_active = 0;
        return;
    }
    for (unsigned i = 0; i < NSLOTS; i++) {
        uint64_t s = g_slot_on[i] ? slot_addr(a, (int)i) : 0;
        float v[3] = {g_target[0], g_target[1] + kSlots[i].dy, g_target[2]};
        if (s && plat_mem_readable(s, 12)) memcpy((void *)(uintptr_t)s, v, 12);
    }
    if (g_dyn_all)
        for (int i = 0; i < g_dyn_n; i++) {
            float v[3] = {g_target[0], g_target[1] + g_dyn[i].dy, g_target[2]};
            if (plat_mem_readable(g_dyn[i].addr, 12)) memcpy((void *)(uintptr_t)g_dyn[i].addr, v, 12);
        }
}

// TOTK_TELEPORT_TEST="<wait> <dx> <dy> <dz>; <wait> @<x> <y> <z>; ...": scripted moves for checks without input. Each
// step waits <wait> presents after the player is resident and the previous move ended, then moves by the offset or,
// with '@', to the position. With TOTK_TELEPORT_SHOTS=<dir>, the frame before each move is saved as <dir>/<n>-before.png
// and the frame AFTER_SHOT presents after it ended as <dir>/<n>-after.png (menu.cpp menu_overlay_frame saves them).
#define AFTER_SHOT 90
static struct { int wait, abs; float v[3]; } g_test[16];
static int g_test_n = -1, g_test_i, g_test_wait, g_test_after;
static char g_shot[512];

static void test_parse(void) {
    g_test_n = 0;
    const char *p = cfg_get("TOTK_TELEPORT_TEST");
    while (p && *p && g_test_n < 16) {
        char *e;
        int timed = *p == 't';   // "t<ms>": at that time of the world clock instead of a wait in presents
        int w = (int)strtol(p + timed, &e, 10);
        if (timed) w = -w;
        if (e == p) break;
        p = e;
        while (*p == ' ') p++;
        int abs = *p == '@';
        if (abs) p++;
        float v[3];
        int ok = 1;
        for (int k = 0; k < 3 && ok; k++) v[k] = strtof(p, &e), ok = e != p, p = e;
        if (!ok) break;
        g_test[g_test_n].wait = w, g_test[g_test_n].abs = abs, memcpy(g_test[g_test_n].v, v, sizeof v), g_test_n++;
        while (*p == ' ' || *p == ';') p++;
    }
    if (g_test_n) rt_log("teleport: test script with %d moves", g_test_n);
}

const char *teleport_test_shot(void) {
    static char shot[600];
    if (g_dump_shot[0]) {
        memcpy(shot, g_dump_shot, sizeof shot);
        g_dump_shot[0] = 0;
        return shot;
    }
    if (g_test_n < 0) test_parse();
    if (!g_test_n) return NULL;
    const char *dir = cfg_get("TOTK_TELEPORT_SHOTS");
    float now[3];
    if (g_active || frame_step_world_us() < 0 || !teleport_player_pos(now)) return NULL;
    if (g_test_after && !--g_test_after && dir) {
        snprintf(g_shot, sizeof g_shot, "%s/%d-after.png", dir, g_test_i);
        rt_log("teleport: test move %d ended at %.1f %.1f %.1f", g_test_i, now[0], now[1], now[2]);
        return g_shot;
    }
    if (g_test_after || g_test_i >= g_test_n) return NULL;
    if (g_test[g_test_i].wait < 0 ? frame_step_world_us() / 1000 < -g_test[g_test_i].wait : ++g_test_wait < g_test[g_test_i].wait)
        return NULL;
    g_test_wait = 0;
    float to[3];
    for (int k = 0; k < 3; k++) to[k] = g_test[g_test_i].abs ? g_test[g_test_i].v[k] : now[k] + g_test[g_test_i].v[k];
    g_test_i++;
    rt_log("teleport: test move %d from %.1f %.1f %.1f: %s", g_test_i, now[0], now[1], now[2], teleport_to(to) ? "started" : "refused");
    g_test_after = AFTER_SHOT;
    if (!dir) return NULL;
    snprintf(g_shot, sizeof g_shot, "%s/%d-before.png", dir, g_test_i);
    return g_shot;
}
