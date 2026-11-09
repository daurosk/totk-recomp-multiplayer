// The scene camera for overlays such as the online name tags (ui/online_ui.cpp). The scene's vertex stage reads its
// camera from constant bank c4, a 0x940-byte block whose rows 3 to 6 (bytes 48 to 111) are the view-projection; a scene
// draw is one with the scene's D32F depth bound, at least DLAA_SCENE_MIN_W wide (the same test as vk_dlaa.c
// vk_dlaa_draw, which keeps the block's address for DLAA only). Here the 64 bytes are copied out of the uniform
// snapshot on the replay thread whenever a scene draw binds a block at a new address, into a sequence-locked copy the
// present thread reads.
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "host.h"
#include "platform/platform.h"
#include "vk.h"
#include "vk_camera.h"
#include "vk_dlaa.h"
#include "vk_exec.h"

#define CAM_BLOCK 0x940u
#define VP_OFF 48u

static struct {
    _Atomic uint32_t seq;           // odd while the writer is in the middle of a copy
    float vp[16];
    _Atomic uint64_t t_ns;          // when a scene draw last carried it, 0: never
    uint64_t addr;                  // the block copied last (replay thread only)
} C;

// TOTK_CAMERA_DUMP=1 (a probe): every 5 s, for 50 ms, logs each distinct 0x940 block bound at c4, rows 0 to 15 with
// the depth target it was drawn with, so the camera layout can be fitted offline against known world positions.
static void camera_dump(VkExec *ex, uint64_t now) {
    static int on = -1;
    if (on < 0) on = rt_knob("CAMERA_DUMP", "0");
    if (!on) return;
    static uint64_t win_start, next_win;
    static uint64_t seen[16];
    static int nseen;
    if (now >= next_win) {
        win_start = now, next_win = now + 5000000000ull, nseen = 0;
    }
    if (now - win_start > 50000000ull || nseen >= 16) return;
    const p_bind_buf *b = &ex->ubo[0][1];
    if (b->size != CAM_BLOCK || !b->addr) return;
    for (int i = 0; i < nseen; i++)
        if (seen[i] == b->addr) return;
    seen[nseen++] = b->addr;
    const float *r = (const float *)vk_usnap_bytes(b->addr, 256);
    if (!r) return;
    const VkExecTarget *z = &ex->depth;
    char line[2048];
    int o = snprintf(line, sizeof line, "camera dump: block %d %llx depth %ux%u fmt %d:", nseen, (unsigned long long)b->addr,
                     z->image ? z->width : 0, z->image ? z->height : 0, z->image ? (int)z->format : -1);
    for (int i = 0; i < 64 && o < (int)sizeof line - 24; i++)
        o += snprintf(line + o, sizeof line - o, "%s%.6g", i % 4 ? "," : " | ", (double)r[i]);
    rt_log("%s", line);
}

void vk_camera_draw(VkExec *ex) {
    const p_bind_buf *b = &ex->ubo[0][1];
    const VkExecTarget *z = &ex->depth;
    camera_dump(ex, plat_now_ns());
    if (b->size != CAM_BLOCK || !b->addr || !z->image || z->width < DLAA_SCENE_MIN_W || z->format != VK_FORMAT_D32_SFLOAT)
        return;
    uint64_t now = plat_now_ns();
    // a block seen again within the same frame holds the same camera; the game writes a new one each frame
    if (b->addr == C.addr && now - atomic_load_explicit(&C.t_ns, memory_order_relaxed) < 4000000ull) {
        atomic_store_explicit(&C.t_ns, now, memory_order_relaxed);
        return;
    }
    const float *src = (const float *)vk_usnap_bytes(b->addr + VP_OFF, 64);
    if (!src) return;
    C.addr = b->addr;
    uint32_t s = atomic_load_explicit(&C.seq, memory_order_relaxed);
    atomic_store_explicit(&C.seq, s + 1, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    memcpy(C.vp, src, sizeof C.vp);
    atomic_store_explicit(&C.seq, s + 2, memory_order_release);
    atomic_store_explicit(&C.t_ns, now, memory_order_relaxed);
}

int vk_camera_viewproj(float m[16], uint64_t *age_ns) {
    uint64_t t = atomic_load_explicit(&C.t_ns, memory_order_relaxed);
    if (!t) return 0;
    for (int tries = 0; tries < 8; tries++) {
        uint32_t a = atomic_load_explicit(&C.seq, memory_order_acquire);
        if (a & 1) continue;
        memcpy(m, C.vp, sizeof C.vp);
        atomic_thread_fence(memory_order_acquire);
        if (atomic_load_explicit(&C.seq, memory_order_relaxed) == a) {
            if (age_ns) *age_ns = plat_now_ns() - t;
            return 1;
        }
    }
    return 0;
}
