// The game side of actor sync (runtime/net/net_actors.h, NaGame), first part: the read-only scan of the resident actors.
//
// Facts used here come from our own read-only probes of a running 1.2.1 build (docs/online-actors.md, "Probed on
// main"): the process manager is [main+0x4722268]; its +0x2d0 points at a table of 32 circular list heads 0x18 apart
// (a node's next at +0, its actor at +0x10; an empty head points at itself), and every updating actor is in at least
// one of them. An actor's name is at [+0x18] (also [+0x218]), its state at +0x24 (4 while it runs), its position at
// +0x2b4, and the map instance id of a placed actor is the u64 at [[+0x370]+0x88] (0 or all ones: made at run time).
//
// Only the scan exists so far. make_copy, drive, puppet, hit and remove need the game's own creation, AI switch, damage
// entry and removal, which are not probed yet, so na_game() stays NULL and net_client.c never offers OW_FEAT_ACTORS.
// Other game versions than 1.2.1 have no probed manager slot and scan nothing.
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "host.h"
#include "layout.h"
#include "net/net_actors.h"
#include "platform/platform.h"

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
            s->body.rot[3] = 1.0f;   // the rotation field is not probed yet
            n++;
        }
    }
    return n;
}

// The adapter for net_actors.c; NULL until the game side can make, drive and remove copies.
const NaGame *na_game(void) { return NULL; }
