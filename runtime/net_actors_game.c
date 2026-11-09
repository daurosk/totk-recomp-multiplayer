// The game side of actor sync (runtime/net/net_actors.h, NaGame): the scan of the resident actors, the game's own calls
// that drive, hold and remove them, and the frame hook that steps the sync core on the game's own thread.
//
// Facts used here come from our own read-only probes of a running 1.2.1 build (docs/online-actors.md, "Probed on
// main"): the process manager is [main+0x4722268]; its +0x2d0 points at a table of 32 circular list heads 0x18 apart
// (a node's next at +0, its actor at +0x10; an empty head points at itself), and every updating actor is in at least
// one of them. An actor's name is at [+0x18] (also [+0x218]), its state at +0x24 (4 while it runs), its position at
// +0x2b4 with a 3x3 orientation before it at +0x290 (three axes of three floats, orthonormal on every actor but the
// system procs), and the map instance id of a placed actor is the u64 at [[+0x370]+0x88] (0 or all ones: made at run time).
//
// The game functions it calls (1.2.1 main offsets, named by the game's own symbols; each start checked in the 1.2.1
// main as a prologue right after the previous function's last branch, docs/online-actors.md "Game calls"):
//   engine::actor::BaseProc::requestDelete(proc, u32)                0x83aa68  returns at once for a state of 7 to 11
//   engine::actor::ActorBase::updateTrans(actor, Vector3f*, int)     0xe8daf0  queues a placement
//   engine::actor::ActorBase::updateRotation(actor, Matrix33f*, int) 0xe655d8
//   engine::actor::ActorBase::getMainRigidBodyEntity(actor)          0x1346e94
//   phive::RigidBodyBase::requestChangeMotionType(body, u32)         0xe8be30  2: keyframed, the body follows, no physics
// The manager slot above is the symbol engine::actor::BaseProcMgr::sInstance, which our probe had found on its own.
//
// Still missing: the actor's life (life_max stays 0), its AI switch (a copy of an enemy keeps thinking; only its body is
// keyframed), the damage entry that would call na_local_hit, and creating an actor by name (make_copy answers 0, so
// another game's run-time spawns are not shown). Map objects, which every game places itself, need none of these to be
// one body: the owner's states move them and the owner's removal takes them out everywhere.
//
// Threads: na_game_scan only reads and runs anywhere. Everything that calls the game runs inside actors_frame, the host
// function the patch set actors_frame (recomp/patches-v121.txt) calls from the game's root update, on the game's thread.
// Other game versions have no probed slot and no hook: na_game() is NULL there and net_client.c offers nothing.
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "host.h"
#include "layout.h"
#include "net/net_actors_link.h"
#include "ow_classes.h"
#include "platform/platform.h"
#if __has_include("patches.h")
#include "patches.h"
#endif

#ifdef RT_PATCH_PAGE
#define HOST_ACTORS_FRAME (RT_PATCH_PAGE + 0xf60)   // recomp/patches-v121.txt, set actors_frame
#endif

#define LISTS 32
#define LIST_STRIDE 0x18
#define WALK_MAX 4096        // a list longer than this is taken as broken (the probe saw about 150)

static int rd(uint64_t a, void *out, size_t n) { return a && plat_mem_read(a, out, n); }
static uint64_t rq(uint64_t a) {
    uint64_t v = 0;
    return rd(a, &v, 8) ? v : 0;
}

// The process manager, 0 when this version has no probed slot or the game has none yet.
static uint64_t manager(void) {
    if (rt_game_version != 121) return 0;
    return rq(MOD_MAIN_BASE + 0x4722268);
}

static int read_name(uint64_t actor, char *out, size_t cap) {
    uint64_t p = rq(actor + 0x18);
    if (!p || !rd(p, out, cap - 1)) return 0;
    out[cap - 1] = 0;
    for (size_t i = 0; out[i]; i++)
        if ((unsigned char)out[i] < 32 || (unsigned char)out[i] > 126) return 0;
    return out[0] != 0;
}

// The orientation at +0x290 as a quaternion (x, y, z, w), taking the three vectors as the matrix's columns as the
// position after them is its fourth column. Identity when the floats are not a rotation.
static void read_rot(uint64_t actor, float q[4]) {
    float m[9];
    q[0] = q[1] = q[2] = 0.0f, q[3] = 1.0f;
    if (!rd(actor + 0x290, m, sizeof m)) return;
    for (int i = 0; i < 9; i++)
        if (!isfinite(m[i])) return;
#define M(r, c) m[(c) * 3 + (r)]
    float t = M(0, 0) + M(1, 1) + M(2, 2), s;
    if (t > 0.0f) {
        s = sqrtf(t + 1.0f) * 2.0f;
        q[3] = 0.25f * s, q[0] = (M(2, 1) - M(1, 2)) / s, q[1] = (M(0, 2) - M(2, 0)) / s, q[2] = (M(1, 0) - M(0, 1)) / s;
    } else if (M(0, 0) > M(1, 1) && M(0, 0) > M(2, 2)) {
        s = sqrtf(1.0f + M(0, 0) - M(1, 1) - M(2, 2)) * 2.0f;
        q[3] = (M(2, 1) - M(1, 2)) / s, q[0] = 0.25f * s, q[1] = (M(0, 1) + M(1, 0)) / s, q[2] = (M(0, 2) + M(2, 0)) / s;
    } else if (M(1, 1) > M(2, 2)) {
        s = sqrtf(1.0f + M(1, 1) - M(0, 0) - M(2, 2)) * 2.0f;
        q[3] = (M(0, 2) - M(2, 0)) / s, q[0] = (M(0, 1) + M(1, 0)) / s, q[1] = 0.25f * s, q[2] = (M(1, 2) + M(2, 1)) / s;
    } else {
        s = sqrtf(1.0f + M(2, 2) - M(0, 0) - M(1, 1)) * 2.0f;
        q[3] = (M(1, 0) - M(0, 1)) / s, q[0] = (M(0, 2) + M(2, 0)) / s, q[1] = (M(1, 2) + M(2, 1)) / s, q[2] = 0.25f * s;
    }
#undef M
    float n = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (!(n > 0.9f && n < 1.1f)) {   // not a rotation (a system proc's floats)
        q[0] = q[1] = q[2] = 0.0f, q[3] = 1.0f;
        return;
    }
    for (int i = 0; i < 4; i++) q[i] /= n;
}

static int seen_before(const NaSeen *out, int n, uint64_t actor) {
    for (int i = 0; i < n; i++)
        if (out[i].handle == actor) return 1;
    return 0;
}

// Fills out with the named, running actors of every list, each once. Present or frame thread; reads only.
int na_game_scan(NaSeen *out, int max) {
    uint64_t mgr = manager(), table = mgr ? rq(mgr + 0x2d0) : 0;
    if (!table) return 0;
    int n = 0;
    for (int l = 0; l < LISTS && n < max; l++) {
        uint64_t head = table + (uint64_t)l * LIST_STRIDE, node = rq(head);
        for (int steps = 0; node && node != head && steps < WALK_MAX && n < max; steps++, node = rq(node)) {
            uint64_t actor = rq(node + 0x10);
            uint32_t state = 0;
            if (!actor || !rd(actor + 0x24, &state, 4) || state != 4 || seen_before(out, n, actor)) continue;
            NaSeen *s = &out[n];
            memset(s, 0, sizeof *s);
            if (!read_name(actor, s->name, sizeof s->name)) continue;
            float pos[3];
            if (!rd(actor + 0x2b4, pos, sizeof pos) || !isfinite(pos[0]) || !isfinite(pos[1]) || !isfinite(pos[2])) continue;
            uint64_t place = rq(actor + 0x370), id = place ? rq(place + 0x88) : 0;
            s->handle = actor;
            s->map_id = id == UINT64_MAX ? 0 : id;
            s->cls = na_class_of(s->name);
            memcpy(s->body.pos, pos, sizeof pos);
            read_rot(actor, s->body.rot);
            n++;
        }
    }
    return n;
}

// ---- calls into the game (inside actors_frame only)

#define FN_REQUEST_DELETE 0x83aa68ull
#define FN_UPDATE_TRANS 0xe8daf0ull
#define FN_UPDATE_ROT 0xe655d8ull
#define FN_MAIN_BODY 0x1346e94ull
#define FN_MOTION_TYPE 0xe8be30ull
#define MOTION_DYNAMIC 0u
#define MOTION_KEYFRAMED 2u

static _Thread_local Cpu *hook_cpu;   // set while actors_frame runs; a call outside it does nothing

// An actor the game still runs. A handle the game has begun removing (or freed and reused for a non-actor) is left alone.
static int alive(uint64_t actor) {
    uint32_t state = 0;
    return actor && rd(actor + 0x24, &state, 4) && state == 4;
}

// fn(actor, &copy of data, arg) with the copy on the guest stack below the hook's frame, or fn(actor, arg) without data.
static uint64_t call_actor(uint64_t fn, uint64_t actor, const void *data, size_t n, uint64_t arg) {
    Cpu *c = hook_cpu;
    if (!c) return 0;
    uint64_t sp = c->sp, r;
    if (data) {
        c->sp = (sp - n - 16) & ~15ull;
        memcpy((void *)(uintptr_t)c->sp, data, n);
        r = rt_call(c, MOD_MAIN_BASE + fn, 3, actor, c->sp, arg);
    } else {
        r = rt_call(c, MOD_MAIN_BASE + fn, 2, actor, arg);
    }
    c->sp = sp;
    return r;
}

// The quaternion back to the nine floats in read_rot's order, so a copy stands the way the owner's game stores its actor
// whichever of rows or columns the game means.
static void rot_floats(const float q[4], float m[9]) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
#define M(r, c) m[(c) * 3 + (r)]
    M(0, 0) = 1 - 2 * (y * y + z * z), M(0, 1) = 2 * (x * y - z * w), M(0, 2) = 2 * (x * z + y * w);
    M(1, 0) = 2 * (x * y + z * w), M(1, 1) = 1 - 2 * (x * x + z * z), M(1, 2) = 2 * (y * z - x * w);
    M(2, 0) = 2 * (x * z - y * w), M(2, 1) = 2 * (y * z + x * w), M(2, 2) = 1 - 2 * (x * x + y * y);
#undef M
}

static uint64_t g_make_copy(void *ctx, uint8_t cls, const char *name, const ow_body *body) {
    (void)ctx, (void)cls, (void)name, (void)body;
    return 0;   // creating by name is not wired yet: requestCreateActorSync's CreateArg is not mapped
}

static void g_drive(void *ctx, uint64_t h, const ow_body *body, int32_t life, int32_t life_max) {
    (void)ctx, (void)life, (void)life_max;
    if (!alive(h)) return;
    float at[3], m[9], now[9];
    if (rd(h + 0x2b4, at, sizeof at) &&
        fabsf(at[0] - body->pos[0]) + fabsf(at[1] - body->pos[1]) + fabsf(at[2] - body->pos[2]) > 0.005f)
        call_actor(FN_UPDATE_TRANS, h, body->pos, sizeof body->pos, 0);
    rot_floats(body->rot, m);
    if (!rd(h + 0x290, now, sizeof now) || memcmp(now, m, sizeof m)) call_actor(FN_UPDATE_ROT, h, m, sizeof m, 0);
}

static void g_puppet(void *ctx, uint64_t h, int on) {
    (void)ctx;
    if (!hook_cpu || !alive(h)) return;
    uint64_t body = rt_call(hook_cpu, MOD_MAIN_BASE + FN_MAIN_BODY, 1, h);
    if (body) rt_call(hook_cpu, MOD_MAIN_BASE + FN_MOTION_TYPE, 2, body, (uint64_t)(on ? MOTION_KEYFRAMED : MOTION_DYNAMIC));
}

static uint32_t hits_unapplied;
static void g_hit(void *ctx, uint64_t h, const ow_hit_info *hit) {
    (void)ctx, (void)h, (void)hit;
    if (!hits_unapplied++) rt_log("actors: a hit from another game arrived; applying hits is not wired yet");
}

static void g_remove(void *ctx, uint64_t h, uint8_t cause) {
    (void)ctx, (void)cause;
    if (alive(h)) call_actor(FN_REQUEST_DELETE, h, NULL, 0, 0);
}

// Names the scan has seen, by name hash, for herd rows (NaGame.name_of). Open addressing; a full table knows no more.
#define NAMES 2048
static struct {
    uint32_t hash;
    char name[OW_ACTOR_NAME_MAX + 1];
} names[NAMES];

static void remember_name(const char *name) {
    uint32_t h = ow_name_hash(name);
    for (uint32_t i = 0, k = h & (NAMES - 1); i < NAMES; i++, k = (k + 1) & (NAMES - 1)) {
        if (!names[k].name[0]) {
            names[k].hash = h;
            strncpy(names[k].name, name, OW_ACTOR_NAME_MAX);
            return;
        }
        if (names[k].hash == h && !strcmp(names[k].name, name)) return;
    }
}

static const char *g_name_of(void *ctx, uint32_t hash) {
    (void)ctx;
    for (uint32_t i = 0, k = hash & (NAMES - 1); i < NAMES && names[k].name[0]; i++, k = (k + 1) & (NAMES - 1))
        if (names[k].hash == hash) return names[k].name;
    return NULL;
}

static int g_scan(void *ctx, NaSeen *out, int max) {
    (void)ctx;
    int n = na_game_scan(out, max);
    for (int i = 0; i < n; i++) remember_name(out[i].name);
    return n;
}

static const NaGame game_121 = {
    .scan = g_scan, .make_copy = g_make_copy, .drive = g_drive, .puppet = g_puppet,
    .hit = g_hit, .remove = g_remove, .name_of = g_name_of,
};

static int hook_on(void) {
    static int set = -2;
    if (set == -2) set = rt_patch_set("actors_frame");
    return rt_game_version == 121 && rt_patch_enabled(set);
}

// The adapter for net_actors.c: on 1.2.1 with the frame hook switched on; NULL elsewhere, and nothing is offered then.
const NaGame *na_game(void) { return hook_on() ? &game_121 : NULL; }

#ifdef HOST_ACTORS_FRAME
// One step of the sync core, at most every 15 ms and on the first thread that ran the hook (the core keeps one thread,
// net_actors.h), then the root update's displaced `ldr x0, [x0, #0xa8]` (main+0xb36948). The game calls of the step
// change registers the root update still holds (x1 among them), so every register is put back before the load.
static void actors_frame(Cpu *c) {
    static _Atomic uint32_t owner_tid;
    static uint64_t last_ns, log_ns, calls, steps, other;
    uint32_t tid = plat_thread_id(), none = 0;
    uint64_t now = plat_now_ns();
    int mine = atomic_compare_exchange_strong(&owner_tid, &none, tid) || none == tid;
    if (!mine) other++;
    else if (++calls, now - log_ns >= 10000000000ull) {   // a line every 10 s: how often and where it ran, what it did
        NalStats ls;
        nal_stats(&ls);
        const NaStats *s = nal_core_stats();
        rt_log("actors: frame hook %llu calls (%llu steps, %llu on other threads) on thread %u; live %d, in %u out %u "
               "dropped %u/%u bad %u; owned %u copies %u offers %u poses %u gone in %u out %u removed %u",
               (unsigned long long)calls, (unsigned long long)steps, (unsigned long long)other, tid, ls.live, ls.in,
               ls.out, ls.in_dropped, ls.out_dropped, ls.bad, s ? s->owned : 0, s ? s->copies : 0, s ? s->offers : 0,
               s ? s->poses : 0, s ? s->gones_in : 0, s ? s->gones_out : 0, s ? s->removed : 0);
        log_ns = now, calls = steps = other = 0;
    }
    if (mine && now - last_ns >= 15000000u) {
        static _Thread_local Cpu saved;
        steps++;
        last_ns = now;
        memcpy(&saved, c, sizeof saved);
        hook_cpu = c;
        nal_tick(NULL);
        hook_cpu = NULL;
        memcpy(c, &saved, sizeof saved);
    }
    memcpy(&c->x[0], (const void *)(uintptr_t)(c->x[0] + 0xa8), 8);
}

__attribute__((constructor)) static void actors_frame_register(void) { rt_register_host(HOST_ACTORS_FRAME, actors_frame); }
#endif
