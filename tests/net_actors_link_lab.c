// Offline cases for the link between the network thread and the actor sync core (runtime/net/net_actors_link.c): the
// feature offer, the grant, both queues through the real codec, a loss and a new admit. One invented game with two
// placed actors stands in for the game side; the platform's lock, clock and log are the trivial ones below.
//
//     python tests/net_actors_check.py      (builds this next to the core's lab and runs it)
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "host.h"
#include "net/net_actors_link.h"
#include "ow_wire.h"
#include "platform/platform.h"

static int fails;
#define CHECK(c, ...)                                   \
    do {                                                \
        int ok_ = (c);                                  \
        printf("%s ", ok_ ? "ok  " : "FAIL");           \
        printf(__VA_ARGS__);                            \
        printf("\n");                                   \
        fails += !ok_;                                  \
    } while (0)

// ---- the platform, single-threaded here

void plat_mutex_lock(PlatMutex *m) { (void)m; }
void plat_mutex_unlock(PlatMutex *m) { (void)m; }
static uint64_t clock_ns = 1000000000ull;
uint64_t plat_now_ns(void) { return clock_ns; }
void rt_log(const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    vprintf(fmt, a);
    va_end(a);
    printf("\n");
}
const NaGame *na_game(void) { return NULL; }

// ---- an invented game: two placed trees

static struct {
    int scans, puppet_on, puppet_off;
    uint64_t last_puppet;   // the last actor whose AI was turned off
} G;

static int scan(void *ctx, NaSeen *out, int max) {
    (void)ctx;
    G.scans++;
    int n = 0;
    for (int i = 0; i < 2 && n < max; i++, n++) {
        memset(&out[n], 0, sizeof out[n]);
        out[n].handle = 0x1000 + (uint64_t)i;
        out[n].map_id = 500 + (uint64_t)i;
        snprintf(out[n].name, sizeof out[n].name, "Obj_Tree_A");
        out[n].cls = na_class_of(out[n].name);
        out[n].body.pos[0] = 10.0f * (float)i;
        out[n].body.rot[3] = 1.0f;
        out[n].life = out[n].life_max = 30;
    }
    return n;
}
static uint64_t make_copy(void *ctx, uint8_t cls, const char *name, const ow_body *b) {
    (void)ctx, (void)cls, (void)name, (void)b;
    return 0;
}
static void drive(void *ctx, uint64_t h, const ow_body *b, int32_t life, int32_t life_max) {
    (void)ctx, (void)h, (void)b, (void)life, (void)life_max;
}
static void puppet(void *ctx, uint64_t h, int on) {
    (void)ctx;
    if (on) G.puppet_on++, G.last_puppet = h;
    else G.puppet_off++;
}
static void hit(void *ctx, uint64_t h, const ow_hit_info *hi) { (void)ctx, (void)h, (void)hi; }
static void removal(void *ctx, uint64_t h, uint8_t cause) { (void)ctx, (void)h, (void)cause; }

static const NaGame game = {NULL, scan, make_copy, drive, puppet, hit, removal, NULL};

// ---- the network thread's side

static struct {
    int n, offers;
    uint16_t types[64];
    ow_offer offer[8];
} Sent;

static int capture(uint16_t type, const void *msg) {
    if (Sent.n < 64) Sent.types[Sent.n++] = type;
    if (type == OW_T_offer && Sent.offers < 8) Sent.offer[Sent.offers++] = *(const ow_offer *)msg;
    return 0;
}

static void sent_clear(void) { memset(&Sent, 0, sizeof Sent); }

static void server_sends(uint16_t type, const void *msg) {
    static uint8_t buf[OW_FRAME_MAX];
    size_t len;
    int e = ow_encode(type, msg, buf, sizeof buf, &len);
    CHECK(!e, "server message 0x%04x encodes", type);
    CHECK(nal_is_from_server(type), "0x%04x counts as a frame of the add-on from the server", type);
    nal_from_server(type, buf, len);
}

static void tick(int frames) {
    float pos[3] = {0, 0, 0};
    for (int i = 0; i < frames; i++) {
        clock_ns += 16666666ull;
        nal_tick(pos);
    }
}

int main(void) {
    CHECK(nal_offer() == 0, "no adapter: the add-on is not offered");
    nal_tick(NULL);
    CHECK(G.scans == 0, "no adapter: nothing is scanned");

    nal_use(&game);
    CHECK(nal_offer() == OW_FEAT_ACTORS, "with an adapter the add-on is offered");
    CHECK(!nal_is_from_server(OW_T_offer) && !nal_is_from_server(OW_T_ping) && nal_is_from_server(OW_T_gone),
          "only the add-on's server-to-client types go to the link");

    // admitted without the add-on: nothing runs, nothing is queued
    nal_admitted(7, 5000000, OW_FEAT_CORE);
    tick(3);
    CHECK(G.scans == 0, "not granted: the core does not step");
    ow_gone gone0 = {0};
    server_sends(OW_T_gone, &gone0);
    NalStats st;
    nal_stats(&st);
    CHECK(st.in == 0 && st.in_dropped == 1, "not granted: a frame of the add-on is dropped (in %u, dropped %u)", st.in,
          st.in_dropped);

    // granted: the trees are offered through the out-queue, decoded again on the network side
    nal_admitted(7, 5000000, OW_FEAT_ACTORS);
    tick(2);
    CHECK(G.scans == 2, "granted: one scan per tick (%d)", G.scans);
    sent_clear();
    CHECK(nal_send_out(capture) == 0, "the out-queue drains");
    CHECK(Sent.offers == 2 && Sent.offer[0].id.serial == 500 && Sent.offer[1].id.serial == 501 &&
          Sent.offer[0].id.origin == 0 && !strcmp(Sent.offer[0].name, "Obj_Tree_A"),
          "both placed trees are offered once by map id (%d offers)", Sent.offers);
    tick(2);
    sent_clear();
    nal_send_out(capture);
    CHECK(Sent.offers == 0, "no offer twice in one load");

    // the server gives tree 500 to another game: it becomes a copy here, its AI off
    ow_owner o;
    memset(&o, 0, sizeof o);
    o.id = (ow_aid){0, 500};
    o.class_ = na_class_of("Obj_Tree_A");
    snprintf(o.name, sizeof o.name, "Obj_Tree_A");
    o.epoch = 1, o.owner = 9, o.cause = OW_OWNER_OFFER;
    o.body.rot[3] = 1.0f;
    server_sends(OW_T_owner, &o);
    // and 501 to this game
    o.id.serial = 501, o.owner = 7;
    o.body.pos[0] = 10.0f;
    server_sends(OW_T_owner, &o);
    tick(1);
    CHECK(G.puppet_on == 1 && G.last_puppet == 0x1000, "the tree the other game owns is a copy here (puppet on %d)",
          G.puppet_on);
    tick(30);
    sent_clear();
    nal_send_out(capture);
    int poses = 0;
    for (int i = 0; i < Sent.n; i++) poses += Sent.types[i] == OW_T_actor_send;
    CHECK(poses >= 1, "the owned tree sends states (%d actor_send in half a second)", poses);
    nal_stats(&st);
    CHECK(st.bad == 0 && st.live == 1, "no frame failed to decode (bad %u), live %d", st.bad, st.live);

    // the connection is lost: the copy gets its AI back, nothing steps, the queues are empty
    int scans = G.scans;
    nal_lost();
    tick(3);
    CHECK(G.puppet_off >= 1, "after a loss the game takes back what it showed (puppet off %d)", G.puppet_off);
    CHECK(G.scans == scans, "after a loss the core does not step");
    sent_clear();
    CHECK(nal_send_out(capture) == 0 && Sent.n == 0, "after a loss nothing is left to send");

    // a new admit: the trees are offered again
    nal_admitted(7, 9000000, OW_FEAT_ACTORS);
    tick(2);
    sent_clear();
    nal_send_out(capture);
    CHECK(Sent.offers == 2, "a new connection offers the trees again (%d)", Sent.offers);

    printf("%d failure(s)\n", fails);
    return fails;
}
