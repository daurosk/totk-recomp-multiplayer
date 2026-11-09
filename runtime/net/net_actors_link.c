// The link between the network thread and the actor sync core (net/net_actors_link.h, docs/online-actors.md "The link").
//
// Each direction is a byte queue of records (type, length, encoded payload) under one mutex. The producer appends; the
// consumer moves the whole queue into a private buffer under the lock and works through it outside the lock, so the
// game's frame never waits on the socket and the network thread never waits on the core. A full queue drops the new
// record and counts it: the states that fill it are sent again within a quarter second, and the core's own resend rules
// cover the rest (an owner that sees no state of its copy keeps running it).
//
// Payloads travel encoded (ow_encode) rather than as structs: an actor_send or actor_recv struct is tens of kilobytes,
// its encoding a few hundred bytes. Decoding on the far side checks each one again.
#include <stdio.h>
#include <string.h>

#include "host.h"
#include "net/net_actors_link.h"
#include "ow_classes.h"
#include "platform/platform.h"

#define NAL_QUEUE (512u << 10)
#define REC_HEAD 6   // u16 type, u32 length

typedef struct {
    uint8_t b[NAL_QUEUE];
    size_t n;
} Queue;

// Every message the core sends (C2S) or reads (S2C); one decoded at a time.
typedef union {
    ow_offer offer;
    ow_release release;
    ow_actor_send actor_send;
    ow_actor_recv actor_recv;
    ow_owner owner;
    ow_hold hold;
    ow_hit hit;
    ow_hit_in hit_in;
    ow_spawn spawn;
    ow_spawned spawned;
    ow_gone gone;
    ow_group group;
    ow_herd herd;
    ow_take take;
    ow_taken taken;
} Msg;

static struct {
    PlatMutex mx;
    Queue in, out;
    uint64_t account, serial_base;
    int64_t offset_us;              // server clock minus local microseconds
    uint32_t admits, losses;        // raised by each admit and each loss: the game side catches up on its tick
    int granted;                    // the server granted OW_FEAT_ACTORS on the current connection
    float player[3];                // the local player's last position (nal_player), for the herd cell
    int have_player;
    NalStats st;
} L;

// The game side's (nal_tick, the core's send callback): only its thread touches these.
static struct {
    const NaGame *use;
    NaWorld *w;
    uint32_t admits, losses;
    Queue in;                       // the records taken from L.in this tick
    uint8_t enc[OW_FRAME_MAX];
    Msg msg;
} T;

// The network thread's (nal_send_out).
static struct {
    Queue out;
    size_t at;
} N;

static int put(Queue *q, uint16_t type, const uint8_t *p, size_t len) {
    if (len > UINT32_MAX || q->n + REC_HEAD + len > sizeof q->b) return 0;
    uint8_t *d = q->b + q->n;
    uint32_t l = (uint32_t)len;
    memcpy(d, &type, 2), memcpy(d + 2, &l, 4), memcpy(d + REC_HEAD, p, len);
    q->n += REC_HEAD + len;
    return 1;
}

// The record at *at of q, or 0 at the end.
static int next(const Queue *q, size_t *at, uint16_t *type, const uint8_t **p, size_t *len) {
    if (*at + REC_HEAD > q->n) return 0;
    uint32_t l;
    memcpy(type, q->b + *at, 2), memcpy(&l, q->b + *at + 2, 4);
    *p = q->b + *at + REC_HEAD, *len = l;
    *at += REC_HEAD + l;
    return 1;
}

static const NaGame *game(void) { return T.use ? T.use : na_game(); }

// ---- network thread

uint32_t nal_offer(void) { return game() ? OW_FEAT_ACTORS : 0; }

void nal_admitted(uint64_t account, uint64_t server_time_us, uint32_t features) {
    plat_mutex_lock(&L.mx);
    L.account = account, L.serial_base = server_time_us;
    L.offset_us = (int64_t)server_time_us - (int64_t)(plat_now_ns() / 1000u);
    L.granted = (features & OW_FEAT_ACTORS) != 0;
    L.admits++;
    L.in.n = L.out.n = 0;
    plat_mutex_unlock(&L.mx);
    N.out.n = N.at = 0;
}

void nal_clock(int64_t offset_us) {
    plat_mutex_lock(&L.mx);
    L.offset_us = offset_us;
    plat_mutex_unlock(&L.mx);
}

int nal_is_from_server(uint16_t type) {
    const ow_msg_info *m = ow_msg(type);
    return m && m->feature == OW_FEAT_ACTORS && m->dir != OW_DIR_C2S;
}

void nal_from_server(uint16_t type, const uint8_t *payload, size_t len) {
    plat_mutex_lock(&L.mx);
    if (L.granted && put(&L.in, type, payload, len)) L.st.in++;
    else L.st.in_dropped++;
    plat_mutex_unlock(&L.mx);
}

int nal_send_out(int (*send)(uint16_t type, const void *msg)) {
    static Msg m;
    for (;;) {
        const uint8_t *p;
        size_t len;
        uint16_t type;
        if (next(&N.out, &N.at, &type, &p, &len)) {
            if (ow_decode(type, p, len, &m)) {
                plat_mutex_lock(&L.mx);
                L.st.bad++;
                plat_mutex_unlock(&L.mx);
                continue;
            }
            int e = send(type, &m);
            if (e) return e;
            continue;
        }
        plat_mutex_lock(&L.mx);
        memcpy(N.out.b, L.out.b, L.out.n);
        N.out.n = L.out.n, N.at = 0;
        L.out.n = 0;
        plat_mutex_unlock(&L.mx);
        if (!N.out.n) return 0;
    }
}

void nal_lost(void) {
    plat_mutex_lock(&L.mx);
    L.granted = 0;
    L.losses++;
    L.in.n = L.out.n = 0;
    plat_mutex_unlock(&L.mx);
    N.out.n = N.at = 0;
}

void nal_stats(NalStats *out) {
    plat_mutex_lock(&L.mx);
    *out = L.st;
    plat_mutex_unlock(&L.mx);
}

// ---- game side

void nal_use(const NaGame *g) { T.use = g; }

const NaStats *nal_core_stats(void) { return T.w ? na_stats(T.w) : NULL; }

// The core's send: encoded here, decoded again on the network thread just before it is sealed and written.
static int send_out(void *ctx, uint16_t type, const void *msg) {
    (void)ctx;
    size_t len;
    int e = ow_encode(type, msg, T.enc, sizeof T.enc, &len);
    if (e) {
        rt_log("net: actors: could not encode message 0x%04x: %s", type, ow_strerror(e));
        return e;
    }
    plat_mutex_lock(&L.mx);
    int ok = L.granted && put(&L.out, type, T.enc, len);
    if (ok) L.st.out++;
    else L.st.out_dropped++;
    plat_mutex_unlock(&L.mx);
    return ok ? 0 : -1;
}

void nal_player(const float *pos) {
    plat_mutex_lock(&L.mx);
    if (pos) memcpy(L.player, pos, sizeof L.player);
    L.have_player = pos != NULL;
    plat_mutex_unlock(&L.mx);
}

void nal_tick(const float *player_pos) {
    const NaGame *g = game();
    plat_mutex_lock(&L.mx);
    uint32_t admits = L.admits, losses = L.losses;
    int live = L.granted && g;
    uint64_t account = L.account, base = L.serial_base;
    int64_t offset = L.offset_us;
    float stored[3];
    int have_stored = L.have_player;
    memcpy(stored, L.player, sizeof stored);
    T.in.n = 0;
    if (live) {
        memcpy(T.in.b, L.in.b, L.in.n);
        T.in.n = L.in.n;
    }
    L.in.n = 0;
    L.st.live = live;
    plat_mutex_unlock(&L.mx);
    if (!g) return;

    // a loss (or a new connection) since the last tick: the copies of the old one go, this game takes back the rest
    if (T.w && (losses != T.losses || admits != T.admits)) na_offline(T.w);
    T.losses = losses;
    if (!live) return;
    if (!T.w && !(T.w = na_new(g, send_out, NULL))) return;
    if (admits != T.admits) {
        na_set_self(T.w, account);
        na_set_serial_base(T.w, base);
        T.admits = admits;
    }
    size_t at = 0, len;
    uint16_t type;
    const uint8_t *p;
    while (next(&T.in, &at, &type, &p, &len)) {
        if (!ow_decode(type, p, len, &T.msg)) {
            na_on_message(T.w, type, &T.msg);
            continue;
        }
        plat_mutex_lock(&L.mx);
        L.st.bad++;
        plat_mutex_unlock(&L.mx);
    }
    if (!player_pos && have_stored) player_pos = stored;
    if (player_pos) na_set_place(T.w, ow_herd_cell(player_pos[0], player_pos[2]));
    int64_t now = (int64_t)(plat_now_ns() / 1000u) + offset;
    na_step(T.w, now > 0 ? (uint64_t)now : 0);
}
