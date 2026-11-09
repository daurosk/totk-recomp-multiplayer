// Teleport: writes the player actor's position (+0x2b4, three floats; frame_rate.c, player_actor) once per presented
// frame for HOLD_FRAMES frames. A single store can lose against the game's own update of that field in the same
// frame, so the target is written again each present until the actor's position has stayed there for SETTLE_FRAMES
// reads in a row. The actor is looked up again every frame: a load that replaces it ends the move. Log lines
// ("teleport: ...") report the target, the read-back of each frame while the move runs, and how it ended.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "platform/platform.h"
#include "teleport.h"

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
    const char *scan = cfg_get("TOTK_TELEPORT_SCAN");
    uint64_t a = frame_step_player_actor();
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

void teleport_frame(void) {
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
