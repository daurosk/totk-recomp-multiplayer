// The scene camera for overlays such as the online name tags (ui/online_ui.cpp). The scene's vertex stage reads its
// camera from constant bank c4, a 0x940-byte block whose rows 3 to 6 (bytes 48 to 111) are the view-projection; a scene
// draw is one with the scene's D32F depth bound, at least DLAA_SCENE_MIN_W wide (the same test as vk_dlaa.c
// vk_dlaa_draw, which keeps the block's address for DLAA only). Here the 64 bytes are copied out of the uniform
// snapshot on the replay thread whenever a scene draw binds a block at a new address, into a sequence-locked copy the
// present thread reads.
#include <stdatomic.h>
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

void vk_camera_draw(VkExec *ex) {
    const p_bind_buf *b = &ex->ubo[0][1];
    const VkExecTarget *z = &ex->depth;
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
