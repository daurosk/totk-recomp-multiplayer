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

static float g_target[3];
static int g_frames_left;   // presents left to keep writing
static int g_wait;          // presents spent waiting for an actor
static int g_settled;
static int g_active;

// TOTK_TELEPORT_SCAN=1: at each teleport, logs where else the player's position sits, the actor's own fields and the
// structures its pointers lead to, to find the copy the game reads the position from.
static void scan_copies(uint64_t actor, const float pos[3]) {
    int hits = 0;
    for (uint64_t off = 0; off < 0x1000 && hits < 60; off += 8) {
        uint64_t p = 0;
        if (!plat_mem_read(actor + off, &p, 8) || p < 0x10000 || p > 0x7fffffffffffull) continue;
        for (uint64_t o2 = 0; o2 < 0x800 && hits < 60; o2 += 4) {
            float f[3];
            if (!plat_mem_read(p + o2, f, 12)) break;
            if (fabsf(f[0] - pos[0]) < 0.05f && fabsf(f[1] - pos[1]) < 0.05f && fabsf(f[2] - pos[2]) < 0.05f) {
                rt_log("teleport: copy at [actor+0x%llx]+0x%llx = %.2f %.2f %.2f", (unsigned long long)off, (unsigned long long)o2,
                       f[0], f[1], f[2]);
                hits++;
            }
        }
    }
    for (uint64_t off = 0; off < 0x1000; off += 4) {
        float f[3];
        if (plat_mem_read(actor + off, f, 12) && fabsf(f[0] - pos[0]) < 0.05f && fabsf(f[1] - pos[1]) < 0.05f &&
            fabsf(f[2] - pos[2]) < 0.05f)
            rt_log("teleport: copy at actor+0x%llx", (unsigned long long)off);
    }
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
    if (scan && !strcmp(scan, "1") && a && plat_mem_read(a + POS_OFF, now, 12)) scan_copies(a, now);
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
    memcpy((void *)(uintptr_t)(a + POS_OFF), g_target, 12);
}
