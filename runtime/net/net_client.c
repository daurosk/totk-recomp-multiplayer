// The client of online play: net/net_client.h over the wire of online/proto (docs/online-wire.md), against the
// release 1 server (server/, docs/server.md). With TOTK_NET_FAKE=1 every call goes to the offline fake instead
// (net_stub.c, under the names of net/net_fake.h).
//
// Threads. The network runs on a thread of its own ("net"), which owns the socket, the link, the handshake and its
// copy of the room. It shares one small block with the game, S below, under one mutex that neither side holds across
// a socket call or a wait: the game pushes commands and its position into S, the network thread pushes the state,
// the player list, chat lines and events into it. net_tick, once per presented frame, swaps what arrived into the
// game's own copy, G, which every other call of the header reads without a lock. So the present thread never waits
// on the network; at worst on a copy of a few kilobytes.
//
// Connection. net_connect starts the thread (once) and asks it to join cfg->address. The thread resolves the address,
// connects with a time limit, signs in (section 6 of docs/online-wire.md) with the player's identity key, kept in
// <profile>/online/identity.key and made from the system's random numbers on the first connection, and remembers the
// server's key per address in <profile>/online/servers.txt: a later connection to the same address must find the
// same key (OW_E_PIN), so nobody can pose as a server the player joined before. The join secret (cfg->token, the
// room's password or a claim code) goes in proof.claim; the server reads it only for a key it does not know yet.
// After admit and join_ready the state is NET_ONLINE. Pings go out each second; ten seconds without a frame, a closed
// socket or a read error count as a lost connection, which the thread retries for as long as the server holds the
// session (admit.rules.hold_ms), resuming the same account, team and rank. A refusal (full, banned, wrong password,
// another version, kicked, ...) ends it for good, with a plain sentence for the player in net_status_text and in the
// NET_EV_STATE event.
//
// Ids: NetPlayer.id is the server's account id (small numbers, counted from 1), team ids are the server's.
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET nsock;
#define NSOCK_BAD INVALID_SOCKET
#else
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int nsock;
#define NSOCK_BAD (-1)
#endif
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0   // Windows has no SIGPIPE
#endif

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "net/net_client.h"
#include "net/net_fake.h"
#include "ow_wire.h"
#include "platform/platform.h"

#define NC_PORT 27120                   // the server's default port (docs/server.md)
#define NC_CONNECT_MS 5000              // a TCP connect gives up after this
#define NC_HANDSHAKE_US 10000000ull     // greet to join_ready
#define NC_SILENCE_US 10000000ull       // no frame for this long: the connection is lost (section 7)
#define NC_PING_US 1000000ull
#define NC_POS_US 1000000ull            // position about once a second (section 7)
#define NC_JUMP_M 30.0f                 // a move this far between two reports is a teleport: report it at once
#define NC_RETRY_MS 1000                // between reconnect attempts
#define NC_OUT_MAX (256u << 10)         // bytes queued for a server that does not read; more ends the connection
#define NC_CMDS 64
#define NC_EVENTS 64
#define NC_POLL_MS 20                   // the thread's poll: how long a command may wait before it goes out

static uint64_t now_us(void) { return plat_now_ns() / 1000u; }
static uint64_t wall_ms(void) { return plat_unix_time_ns() / 1000000ull; }

// Copies src into dst (size n) cut at a UTF-8 character boundary, with control characters turned into spaces, so the
// result is a string the wire's encoder takes when src was UTF-8.
static void copy_text(char *dst, size_t n, const char *src) {
    size_t k = 0;
    if (!n) return;
    for (; src && src[k] && k + 1 < n; k++) dst[k] = (unsigned char)src[k] < 0x20 || src[k] == 0x7f ? ' ' : src[k];
    if (src && ((unsigned char)src[k] & 0xC0) == 0x80) {   // cut inside a character: drop the part that fitted
        while (k > 0 && ((unsigned char)dst[k - 1] & 0xC0) == 0x80) k--;
        if (k > 0) k--;
    }
    dst[k] = 0;
}

static int ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return !*a && !*b;
}

static void wipe(void *p, size_t n) {
    volatile uint8_t *v = p;
    while (n--) *v++ = 0;
}

// A place name from a position, the same rule the UI uses for the local player (ui/online_ui.cpp report_local).
static void area_of(char *out, size_t n, const float p[3]) {
    snprintf(out, n, "%s (%d, %d)", p[1] < -100 ? "Depths" : p[1] > 600 ? "Sky" : "Surface", (int)p[0], (int)p[2]);
}

// ---- what the game and the network thread share (under S.mx)

typedef struct {
    uint16_t type;
    union {
        ow_say say;
        ow_tp_ask tp_ask;
        ow_tp_reply tp_reply;
        ow_team_create team_create;
        ow_team_invite team_invite;
        ow_team_join team_join;
        ow_team_leave team_leave;
        ow_summon summon;
        ow_mod_action mod_action;
        ow_announce announce;
    } m;
} Cmd;

static struct {
    PlatMutex mx;
    int started;
    // the game's wishes
    unsigned gen;                   // raised by every net_connect and net_disconnect: the thread drops what it does
    int want;                       // 1: be connected to cfg
    NetConfig cfg;
    char dir[600];                  // <profile>/online
    Cmd cmd[NC_CMDS];
    int ncmd;
    float pos[3];
    int have_pos;
    uint8_t character;
    int test_drop;                  // tests: close the socket as if the network failed (net_test_drop)
    // the network's news
    NetState state;
    uint32_t self;
    char status[NET_TEXT_MAX];
    NetPlayer pl[NET_MAX_PLAYERS];
    int npl;
    NetTeam team[NET_MAX_TEAMS];
    int nteam;
    int teleport_on;
    uint32_t roster_ver;
    NetChatLine chat[NET_CHAT_KEEP];
    int nchat;
    NetEvent ev[NC_EVENTS];
    int nev;
} S;

// ---- the game's copy (present thread only)

static struct {
    NetState state;
    uint32_t self;
    char status[NET_TEXT_MAX];
    NetPlayer pl[NET_MAX_PLAYERS];
    int npl;
    NetTeam team[NET_MAX_TEAMS];
    int nteam;
    int teleport_on;
    uint32_t roster_ver;
    NetChatLine chat[NET_CHAT_KEEP];
    int nchat;                      // lines ever added; the ring holds the last NET_CHAT_KEEP
    uint32_t serial;
    NetEvent ev[NC_EVENTS];
    int nev;
    char area[NET_AREA_MAX], character[NET_NAME_MAX];
    float pos[3];
    int have_pos;
} G = {.character = "Link"};

static int g_fake = -1;

static int fake(void) {
    if (g_fake < 0) {
        const char *e = cfg_get("TOTK_NET_FAKE");
        g_fake = e && !strcmp(e, "1");
    }
    return g_fake;
}

// ---- sockets

static int sock_init(void) {
#ifdef _WIN32
    WSADATA w;
    return WSAStartup(MAKEWORD(2, 2), &w) == 0;
#else
    return 1;
#endif
}

static void sock_close(nsock s) {
    if (s == NSOCK_BAD) return;
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
}

static int sock_would_block(void) {
#ifdef _WIN32
    int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS || errno == EINTR;
#endif
}

static void sock_nonblocking(nsock s) {
#ifdef _WIN32
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
}

// Waits up to ms for s to become readable (want_write 0) or writable too (1). 1 ready, 0 time out, -1 failed.
static int sock_wait(nsock s, int want_write, int ms) {
#ifdef _WIN32
    WSAPOLLFD p = {.fd = s, .events = (SHORT)(POLLRDNORM | (want_write ? POLLWRNORM : 0))};
    int r = WSAPoll(&p, 1, ms);
#else
    struct pollfd p = {.fd = s, .events = (short)(POLLIN | (want_write ? POLLOUT : 0))};
    int r = poll(&p, 1, ms);
#endif
    if (r < 0) return sock_would_block() ? 0 : -1;
    return r > 0;
}

// ---- the network thread's connection and its copy of the room

enum { END_NONE, END_STOP, END_FINAL, END_LOST };
enum { P_NONE, P_GREETED, P_PROOF, P_JOINING, P_LIVE };

static struct {
    nsock s;
    int phase;
    ow_link link;
    ow_reader rd;
    ow_handshake hs;
    ow_identity id;
    uint8_t out[NC_OUT_MAX];
    size_t nout;
    uint8_t frame[OW_FRAME_MAX];
    uint64_t last_rx, next_ping, next_pos, deadline;
    uint32_t ping_seq;
    // the session
    NetConfig cfg;
    char dir[600];
    char host[128], addr_key[160];
    char port[8];
    uint8_t pin[32];
    int have_pin;
    uint64_t account;
    uint32_t hold_ms, options;
    int admitted, resumed_once;
    char why[NET_TEXT_MAX];         // the sentence of the attempt that just ended
    // the room
    ow_roster_row rows[OW_PLAYERS_MAX];
    int nrows;
    ow_spot spots[OW_PLAYERS_MAX];
    int nspots;
    ow_team_row teams[OW_TEAMS_MAX];
    int nteams;
    // what was last reported
    float sent_pos[3];
    int sent_any;
    int16_t cell[2];
    int area_sent;
    uint8_t char_sent;
} C = {.s = NSOCK_BAD};

static union {
    ow_refuse refuse;
    ow_challenge challenge;
    ow_admit admit;
    ow_bye bye;
    ow_join_ready join_ready;
    ow_ping ping;
    ow_pong pong;
    ow_roster roster;
    ow_arrive arrive;
    ow_depart depart;
    ow_positions positions;
    ow_teams teams;
    ow_team_invited team_invited;
    ow_chat chat;
    ow_notice notice;
    ow_marker marker;
    ow_rules rules;
    ow_tp_asked tp_asked;
    ow_tp_result tp_result;
    ow_go_to go_to;
    ow_mod_done mod_done;
} M;

static const ow_roster_row *row_of(uint64_t account) {
    for (int i = 0; i < C.nrows; i++)
        if (C.rows[i].account == account) return &C.rows[i];
    return NULL;
}

static const char *name_of(uint64_t account) {
    const ow_roster_row *r = row_of(account);
    return r ? r->nickname : "Someone";
}

// ---- news to the game (each takes S.mx for a copy)

static void ev_put_locked(const NetEvent *e) {
    if (S.nev == NC_EVENTS) memmove(S.ev, S.ev + 1, (size_t)(--S.nev) * sizeof *S.ev);
    S.ev[S.nev++] = *e;
}

static void pub_event(const NetEvent *e) {
    plat_mutex_lock(&S.mx);
    ev_put_locked(e);
    plat_mutex_unlock(&S.mx);
}

static void pub_state(NetState st, const char *why) {
    plat_mutex_lock(&S.mx);
    if (S.state != st || (why && *why)) {
        S.state = st;
        copy_text(S.status, sizeof S.status, why ? why : "");
        NetEvent e = {.type = NET_EV_STATE};
        copy_text(e.text, sizeof e.text, why ? why : "");
        ev_put_locked(&e);
    }
    if (st == NET_ONLINE) S.status[0] = 0;
    S.self = st == NET_ONLINE ? (uint32_t)C.account : 0;
    plat_mutex_unlock(&S.mx);
}

static void pub_chat(NetChannel ch, uint64_t from, const char *from_name, const char *to_name, const char *text) {
    NetChatLine l;
    memset(&l, 0, sizeof l);
    l.time_ms = wall_ms();
    l.channel = ch;
    l.from = (uint32_t)from;
    copy_text(l.from_name, sizeof l.from_name, from_name);
    copy_text(l.to_name, sizeof l.to_name, to_name);
    copy_text(l.text, sizeof l.text, text);
    plat_mutex_lock(&S.mx);
    if (S.nchat == NET_CHAT_KEEP) memmove(S.chat, S.chat + 1, (size_t)(--S.nchat) * sizeof *S.chat);
    S.chat[S.nchat++] = l;
    plat_mutex_unlock(&S.mx);
}

static void pub_system(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void pub_system(const char *fmt, ...) {
    char t[NET_TEXT_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t, sizeof t, fmt, ap);
    va_end(ap);
    pub_chat(NET_CHAT_SYSTEM, 0, "", "", t);
}

// The player list as the UI sees it, from the roster, the positions and the teams.
static void pub_roster(void) {
    static NetPlayer pl[NET_MAX_PLAYERS];
    static NetTeam tm[NET_MAX_TEAMS];
    int n = 0, nt = 0;
    for (int i = 0; i < C.nrows && n < NET_MAX_PLAYERS; i++) {
        const ow_roster_row *r = &C.rows[i];
        NetPlayer *p = &pl[n++];
        memset(p, 0, sizeof *p);
        p->id = (uint32_t)r->account;
        copy_text(p->name, sizeof p->name, r->nickname);
        snprintf(p->character, sizeof p->character, "%s", r->character == OW_CHAR_ZELDA ? "Zelda" : "Link");
        p->team = r->team;
        p->role = r->rank == OW_RANK_HOST ? NET_ROLE_HOST : r->rank == OW_RANK_MODERATOR ? NET_ROLE_MOD : NET_ROLE_PLAYER;
        p->ping_ms = r->ping_ms;
        p->muted = r->muted;
        int have = 0;
        for (int k = 0; k < C.nspots; k++)
            if (C.spots[k].account == r->account) memcpy(p->pos, C.spots[k].pos, sizeof p->pos), have = 1;
        static const char *const states[] = {"Title screen", "Playing", "Loading", "Paused", "Fallen"};
        if (r->afk) snprintf(p->area, sizeof p->area, "Reconnecting");
        else if (r->area.state != OW_PLACE_PLAYING && r->area.state <= OW_PLACE_LAST)
            snprintf(p->area, sizeof p->area, "%s", states[r->area.state]);
        else if (have) area_of(p->area, sizeof p->area, p->pos);
        else snprintf(p->area, sizeof p->area, "Playing");
    }
    for (int i = 0; i < C.nteams && nt < NET_MAX_TEAMS; i++) {
        NetTeam *t = &tm[nt++];
        memset(t, 0, sizeof *t);
        t->id = C.teams[i].id;
        copy_text(t->name, sizeof t->name, C.teams[i].name);
        t->leader = (uint32_t)C.teams[i].leader;
    }
    plat_mutex_lock(&S.mx);
    memcpy(S.pl, pl, (size_t)n * sizeof *pl);
    S.npl = n;
    memcpy(S.team, tm, (size_t)nt * sizeof *tm);
    S.nteam = nt;
    S.teleport_on = (C.options & OW_RULE_TELEPORT) != 0;
    S.roster_ver++;
    plat_mutex_unlock(&S.mx);
}

static void room_clear(void) {
    C.nrows = C.nspots = C.nteams = 0;
    pub_roster();
}

// ---- identity and known servers

static int identity_load(char *why, size_t n) {
    char path[700];
    snprintf(path, sizeof path, "%s/identity.key", C.dir);
    uint8_t seed[32];
    FILE *f = fopen(path, "rb");
    if (f) {
        size_t k = fread(seed, 1, sizeof seed, f);
        fclose(f);
        if (k != sizeof seed) {
            snprintf(why, n, "The online key file %s is damaged. Move it away to make a new key (a new account).", path);
            return 0;
        }
    } else {
        plat_file_mkdir_p(C.dir);
        if (ow_random(seed, sizeof seed) || !plat_file_write_atomic(path, seed, sizeof seed)) {
            wipe(seed, sizeof seed);
            snprintf(why, n, "Could not make the online key file %s.", path);
            return 0;
        }
        rt_log("net: made a new online identity in %s", path);
    }
    ow_identity_from_seed(&C.id, seed);
    wipe(seed, sizeof seed);
    return 1;
}

static void hex_of(char *out, const uint8_t *b, size_t n) {
    for (size_t i = 0; i < n; i++) snprintf(out + 2 * i, 3, "%02x", b[i]);
}

// The key recorded for C.addr_key in servers.txt (lines "<host:port> <64 hex>").
static void pin_load(void) {
    char path[700], line[300];
    snprintf(path, sizeof path, "%s/servers.txt", C.dir);
    C.have_pin = 0;
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char key[200], hex[80];
        if (sscanf(line, "%199s %79s", key, hex) != 2 || strcmp(key, C.addr_key) || strlen(hex) != 64) continue;
        for (int i = 0; i < 32; i++) {
            unsigned v;
            if (sscanf(hex + 2 * i, "%2x", &v) != 1) break;
            C.pin[i] = (uint8_t)v;
        }
        C.have_pin = 1;
    }
    fclose(f);
}

static void pin_save(const uint8_t key[32]) {
    char path[700], line[300], hex[65];
    snprintf(path, sizeof path, "%s/servers.txt", C.dir);
    hex_of(hex, key, 32);
    FILE *f = fopen(path, "a");
    if (!f) return;
    snprintf(line, sizeof line, "%s %s\n", C.addr_key, hex);
    fputs(line, f);
    fclose(f);
    memcpy(C.pin, key, 32);
    C.have_pin = 1;
    rt_log("net: remembered the key of the server at %s", C.addr_key);
}

// "host", "host:port", "[v6]:port" or "v6" into C.host, C.port and the key servers.txt files it under.
static int address_parse(const char *a) {
    while (*a == ' ') a++;
    const char *colon = strrchr(a, ':');
    size_t hl;
    if (*a == '[') {
        const char *end = strchr(a, ']');
        if (!end) return 0;
        a++;
        hl = (size_t)(end - a);
        colon = end[1] == ':' ? end + 1 : NULL;
    } else if (colon && strchr(a, ':') != colon) {   // a bare IPv6 address
        hl = strlen(a);
        colon = NULL;
    } else {
        hl = colon ? (size_t)(colon - a) : strlen(a);
    }
    while (hl && a[hl - 1] == ' ') hl--;
    if (!hl || hl >= sizeof C.host) return 0;
    memcpy(C.host, a, hl);
    C.host[hl] = 0;
    long port = colon ? strtol(colon + 1, NULL, 10) : NC_PORT;
    if (port <= 0 || port > 65535) return 0;
    snprintf(C.port, sizeof C.port, "%ld", port);
    snprintf(C.addr_key, sizeof C.addr_key, "%s:%ld", C.host, port);
    for (char *p = C.addr_key; *p; p++) *p = (char)tolower((unsigned char)*p);
    return 1;
}

// ---- the game's wishes, as the thread sees them

static int stopped(unsigned gen) {
    plat_mutex_lock(&S.mx);
    int s = S.gen != gen;
    plat_mutex_unlock(&S.mx);
    return s;
}

// ---- frames out

static int send_msg(uint16_t type, const void *msg) {
    size_t n;
    int e = ow_frame_write(&C.link, type, msg, C.frame, sizeof C.frame, &n);
    if (e) {
        const ow_msg_info *m = ow_msg(type);
        rt_log("net: could not write %s: %s", m ? m->name : "?", ow_strerror(e));
        return e;
    }
    if (C.nout + n > sizeof C.out) {
        snprintf(C.why, sizeof C.why, "Lost the connection to the server.");
        return -1000;
    }
    memcpy(C.out + C.nout, C.frame, n);
    C.nout += n;
    return 0;
}

static int flush_out(void) {
    size_t done = 0;
    while (done < C.nout) {
        int k = (int)send(C.s, (const char *)C.out + done, (int)(C.nout - done), MSG_NOSIGNAL);
        if (k < 0) {
            if (sock_would_block()) break;
            return -1;
        }
        done += (size_t)k;
    }
    memmove(C.out, C.out + done, C.nout - done);
    C.nout -= done;
    return 0;
}

// ---- reasons

static void why_of(uint16_t reason, const char *detail, uint16_t version, const char *build) {
    char *w = C.why;
    size_t n = sizeof C.why;
    switch (reason) {
    case OW_WHY_VERSION:
        snprintf(w, n, "This server runs another version of online play (server: protocol %u, %s; this game: protocol %u). "
                       "Update the older of the two.", version, build && *build ? build : "unknown build", OW_VERSION);
        break;
    case OW_WHY_AUTH: snprintf(w, n, "The server could not check this game's key."); break;
    case OW_WHY_UNKNOWN_KEY: snprintf(w, n, "This server needs a password or an invite code: enter it in the Online tab."); break;
    case OW_WHY_BAD_CLAIM: snprintf(w, n, "Wrong password or invite code."); break;
    case OW_WHY_FULL: snprintf(w, n, "%s", detail && *detail ? detail : "The server is full."); break;
    case OW_WHY_BANNED: snprintf(w, n, "%s", detail && *detail ? detail : "You are banned from this server."); break;
    case OW_WHY_SHUTDOWN: snprintf(w, n, "The server shut down."); break;
    case OW_WHY_TIMEOUT: snprintf(w, n, "Lost the connection to the server."); break;
    case OW_WHY_DUPLICATE: snprintf(w, n, "This account signed in again from another place."); break;
    case OW_WHY_KICKED: snprintf(w, n, "%s", detail && *detail ? detail : "You were kicked."); break;
    case OW_WHY_PROTOCOL:
        snprintf(w, n, "The server refused a message from this game (%s).", detail && *detail ? detail : "protocol");
        break;
    default: snprintf(w, n, "The server closed the connection%s%s", detail && *detail ? ": " : ".", detail ? detail : "");
    }
}

// What a refuse or bye ends: a timeout is a lost connection the hold may still save, the rest is final.
static int end_by(uint16_t reason, const char *detail, uint16_t version, const char *build) {
    why_of(reason, detail, version, build);
    rt_log("net: the server ended the connection (reason %u): %s", reason, C.why);
    if ((reason == OW_WHY_KICKED || reason == OW_WHY_BANNED) && C.phase == P_LIVE) {
        NetEvent e = {.type = NET_EV_KICKED};
        copy_text(e.text, sizeof e.text, C.why);
        pub_event(&e);
    }
    return reason == OW_WHY_TIMEOUT && C.admitted ? END_LOST : END_FINAL;
}

// ---- frames in

static void on_roster(void) {
    const ow_roster_row *old = row_of(C.account);
    uint8_t old_rank = old ? old->rank : 0xFF, old_muted = old ? old->muted : 0;
    int had = old != NULL;
    C.nrows = M.roster.players_count;
    memcpy(C.rows, M.roster.players, (size_t)C.nrows * sizeof *C.rows);
    const ow_roster_row *me = row_of(C.account);
    if (had && me && me->muted != old_muted) {
        NetEvent e = {.type = NET_EV_MUTED, .player = (uint32_t)C.account, .accepted = me->muted};
        pub_event(&e);
    }
    if (had && me && me->rank != old_rank && (me->rank == OW_RANK_MODERATOR || old_rank == OW_RANK_MODERATOR)) {
        NetEvent e = {.type = NET_EV_ROLE_CHANGED, .player = (uint32_t)C.account, .accepted = me->rank == OW_RANK_MODERATOR};
        pub_event(&e);
    }
    pub_roster();
}

static void on_chat(void) {
    const ow_chat *m = &M.chat;
    switch (m->kind) {
    case OW_CHAT_TEAM: pub_chat(NET_CHAT_TEAM, m->from, m->nickname, "", m->text); break;
    case OW_CHAT_WHISPER: pub_chat(NET_CHAT_WHISPER, m->from, m->nickname, name_of(m->to), m->text); break;
    case OW_CHAT_ME: {
        char t[NET_TEXT_MAX];
        snprintf(t, sizeof t, "%s %s", m->nickname, m->text);
        pub_chat(NET_CHAT_SYSTEM, m->from, m->nickname, "", t);
        break;
    }
    case OW_CHAT_SYSTEM: pub_chat(NET_CHAT_SYSTEM, 0, "", "", m->text); break;
    default: pub_chat(NET_CHAT_ALL, m->from, m->nickname, "", m->text);
    }
}

static void on_notice(void) {
    const ow_notice *m = &M.notice;
    if (m->kind != OW_NOTICE_TOAST) pub_system("%s", m->text);
    if (m->kind != OW_NOTICE_MOTD) {
        NetEvent e = {.type = NET_EV_SERVER_MESSAGE};
        copy_text(e.text, sizeof e.text, m->text);
        pub_event(&e);
    }
}

static void on_tp_result(void) {
    const ow_tp_result *m = &M.tp_result;
    const char *who = name_of(m->target);
    switch (m->outcome) {
    case OW_TP_ACCEPTED:
    case OW_TP_REFUSED: {
        NetEvent e = {.type = NET_EV_TELEPORT_ANSWER, .player = (uint32_t)m->target, .request = (uint32_t)m->target,
                      .accepted = m->outcome == OW_TP_ACCEPTED};
        copy_text(e.name, sizeof e.name, who);
        pub_event(&e);
        break;
    }
    case OW_TP_TIMEOUT: pub_system("%s did not answer your teleport request.", who); break;
    case OW_TP_NOT_ALLOWED: pub_system("Teleports are turned off on this server."); break;
    case OW_TP_GONE: pub_system("%s can't be reached right now.", who); break;
    case OW_TP_BUSY: pub_system("You already have a teleport request open."); break;
    }
}

static void on_mod_done(void) {
    const ow_mod_done *m = &M.mod_done;
    const char *who = name_of(m->target);
    switch (m->outcome) {
    case OW_MOD_NOT_ALLOWED: pub_system("You are not allowed to do that to %s.", who); break;
    case OW_MOD_UNKNOWN: pub_system("That player is not here."); break;
    case OW_MOD_ALREADY: pub_system("Nothing to change for %s.", who); break;
    default: break;   // done: the server tells everyone in a system line
    }
}

// One frame from the server. Returns END_NONE to go on, or how the connection ends.
static int on_frame(const ow_frame *f, unsigned gen) {
    (void)gen;
    int e = ow_decode(f->type, f->payload, f->len, &M);
    if (e) {
        snprintf(C.why, sizeof C.why, "The server sent something this game could not read (%s).", ow_strerror(e));
        return END_FINAL;
    }
    switch (f->type) {
    case OW_T_refuse: return end_by(M.refuse.reason, M.refuse.detail, M.refuse.version, M.refuse.build);
    case OW_T_bye: return end_by(M.bye.reason, M.bye.detail, OW_VERSION, "");
    case OW_T_challenge: {
        if (C.phase != P_GREETED) break;
        ow_proof pr;
        char nick[OW_NICK_MAX + 1], claim[OW_CLAIM_MAX + 1];
        copy_text(nick, sizeof nick, C.cfg.name[0] ? C.cfg.name : "Player");
        copy_text(claim, sizeof claim, C.cfg.token);
        plat_mutex_lock(&S.mx);
        uint8_t ch = S.character;
        plat_mutex_unlock(&S.mx);
        e = ow_hs_client_challenge(&C.hs, &M.challenge, C.have_pin ? C.pin : NULL, &C.id, nick, ch, claim, OW_FEAT_CORE, &pr);
        if (!e) e = ow_hs_seal(&C.hs, &C.link);
        ow_hs_wipe(&C.hs);
        wipe(claim, sizeof claim);
        if (e == OW_E_PIN) {
            snprintf(C.why, sizeof C.why, "The server at %s has another key than on your last visit, so it may not be the same "
                     "server. If its host made a new key, remove its line from %s/servers.txt.", C.addr_key, C.dir);
            return END_FINAL;
        }
        if (e) {
            snprintf(C.why, sizeof C.why, "The server's identity did not check (%s).", ow_strerror(e));
            return END_FINAL;
        }
        if (!C.have_pin) memcpy(C.pin, M.challenge.identity, 32);   // recorded once admitted
        C.char_sent = ch;
        e = send_msg(OW_T_proof, &pr);
        wipe(&pr, sizeof pr);
        if (e) return END_LOST;
        C.phase = P_PROOF;
        break;
    }
    case OW_T_admit:
        if (C.phase != P_PROOF) break;
        ow_link_set_features(&C.link, M.admit.features);
        if (!C.have_pin) pin_save(C.pin);
        if (C.admitted && C.account != M.admit.account) room_clear();
        C.account = M.admit.account;
        C.hold_ms = M.admit.rules.hold_ms;
        C.options = M.admit.rules.options;
        C.resumed_once = M.admit.resumed;
        C.admitted = 1;
        C.phase = P_JOINING;
        rt_log("net: admitted as %s (account %llu)%s", M.admit.nickname, (unsigned long long)C.account,
               M.admit.resumed ? ", session resumed" : "");
        break;
    case OW_T_join_ready:
        if (C.phase != P_JOINING) break;
        C.phase = P_LIVE;
        C.sent_any = C.area_sent = 0;
        C.next_pos = 0;
        pub_state(NET_ONLINE, "");
        if (C.resumed_once) pub_system("Reconnected.");
        break;
    case OW_T_ping: {
        ow_pong r = {.seq = M.ping.seq, .echo_us = M.ping.sent_us, .reply_us = now_us()};
        if (send_msg(OW_T_pong, &r)) return END_LOST;
        break;
    }
    case OW_T_pong: break;
    case OW_T_roster: on_roster(); break;
    case OW_T_teams:
        C.nteams = M.teams.list_count;
        memcpy(C.teams, M.teams.list, (size_t)C.nteams * sizeof *C.teams);
        pub_roster();
        break;
    case OW_T_positions:
        C.nspots = M.positions.spots_count;
        memcpy(C.spots, M.positions.spots, (size_t)C.nspots * sizeof *C.spots);
        pub_roster();
        break;
    case OW_T_rules:
        if ((C.options ^ M.rules.rules.options) & OW_RULE_TELEPORT)
            pub_system(M.rules.rules.options & OW_RULE_TELEPORT ? "Teleport requests are on." : "Teleport requests are off.");
        C.options = M.rules.rules.options;
        C.hold_ms = M.rules.rules.hold_ms;
        pub_roster();
        break;
    case OW_T_arrive:
        if (M.arrive.account != C.account) {
            NetEvent ev = {.type = NET_EV_PLAYER_JOINED, .player = (uint32_t)M.arrive.account};
            copy_text(ev.name, sizeof ev.name, M.arrive.nickname);
            pub_event(&ev);
        }
        break;
    case OW_T_depart:
        if (M.depart.account != C.account) {
            NetEvent ev = {.type = NET_EV_PLAYER_LEFT, .player = (uint32_t)M.depart.account};
            copy_text(ev.name, sizeof ev.name, name_of(M.depart.account));
            pub_event(&ev);
        }
        break;
    case OW_T_chat: on_chat(); break;
    case OW_T_notice: on_notice(); break;
    case OW_T_team_invited: {
        NetEvent ev = {.type = NET_EV_TEAM_INVITE, .player = (uint32_t)M.team_invited.from, .request = M.team_invited.team};
        copy_text(ev.name, sizeof ev.name, M.team_invited.nickname);
        copy_text(ev.text, sizeof ev.text, M.team_invited.name);
        pub_event(&ev);
        break;
    }
    case OW_T_tp_asked: {
        NetEvent ev = {.type = NET_EV_TELEPORT_REQUEST, .player = (uint32_t)M.tp_asked.from,
                       .request = (uint32_t)M.tp_asked.from};
        copy_text(ev.name, sizeof ev.name, M.tp_asked.nickname);
        pub_event(&ev);
        break;
    }
    case OW_T_tp_result: on_tp_result(); break;
    case OW_T_go_to: {
        NetEvent ev = {.type = M.go_to.cause == OW_GO_SUMMON ? NET_EV_SUMMON : NET_EV_TELEPORT_GO,
                       .player = (uint32_t)M.go_to.target, .request = (uint32_t)M.go_to.target, .accepted = 1};
        memcpy(ev.pos, M.go_to.pos, sizeof ev.pos);
        copy_text(ev.name, sizeof ev.name, name_of(M.go_to.target));
        pub_event(&ev);
        C.next_pos = 0;   // report the new place soon after the move
        break;
    }
    case OW_T_mod_done: on_mod_done(); break;
    default: break;   // markers and the rest have no place in this release's UI
    }
    return END_NONE;
}

// ---- what the game asked for, and the reports of the local player

static void send_commands(void) {
    static Cmd cmd[NC_CMDS];
    plat_mutex_lock(&S.mx);
    int n = S.ncmd;
    memcpy(cmd, S.cmd, (size_t)n * sizeof *cmd);
    S.ncmd = 0;
    plat_mutex_unlock(&S.mx);
    for (int i = 0; i < n; i++) {
        int e = send_msg(cmd[i].type, &cmd[i].m);
        if (e == -1000) return;
        if (e) pub_system("That could not be sent (%s).", ow_strerror(e));
    }
}

static void send_reports(uint64_t now) {
    plat_mutex_lock(&S.mx);
    float p[3] = {S.pos[0], S.pos[1], S.pos[2]};
    int have = S.have_pos;
    uint8_t ch = S.character;
    plat_mutex_unlock(&S.mx);
    if (ch != C.char_sent) {
        ow_character m = {.character = ch};
        if (!send_msg(OW_T_character, &m)) C.char_sent = ch;
    }
    if (!have) return;
    float dx = p[0] - C.sent_pos[0], dy = p[1] - C.sent_pos[1], dz = p[2] - C.sent_pos[2];
    int jumped = C.sent_any && dx * dx + dy * dy + dz * dz > NC_JUMP_M * NC_JUMP_M;
    if (now >= C.next_pos || jumped) {
        ow_position m = {.scene = 0, .pos = {p[0], p[1], p[2]}, .yaw = 0};
        if (!send_msg(OW_T_position, &m)) {
            memcpy(C.sent_pos, p, sizeof p);
            C.sent_any = 1;
        }
        C.next_pos = now + NC_POS_US;
    }
    // the map section: kilometre squares from the map's corner (the world is about 12 by 10 km around the origin)
    int16_t cell[2] = {(int16_t)((p[0] + 6000.0f) / 1000.0f), (int16_t)((p[2] + 5000.0f) / 1000.0f)};
    if (!C.area_sent || cell[0] != C.cell[0] || cell[1] != C.cell[1]) {
        ow_area m = {.where = {.scene = 0, .cell_x = cell[0], .cell_y = cell[1], .state = OW_PLACE_PLAYING}};
        if (!send_msg(OW_T_area, &m)) {
            C.cell[0] = cell[0], C.cell[1] = cell[1];
            C.area_sent = 1;
        }
    }
}

// ---- one connection attempt, from the TCP connect to its end

static int tcp_connect(unsigned gen) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    if (getaddrinfo(C.host, C.port, &hints, &res) || !res) {
        snprintf(C.why, sizeof C.why, "Could not find the server %s.", C.host);
        return END_LOST;
    }
    int r = END_LOST;
    for (struct addrinfo *a = res; a && r == END_LOST; a = a->ai_next) {
        nsock s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == NSOCK_BAD) continue;
        sock_nonblocking(s);
        if (connect(s, a->ai_addr, (int)a->ai_addrlen) != 0 && !sock_would_block()) {
            sock_close(s);
            continue;
        }
        int waited = 0, ok = 0;
        while (waited < NC_CONNECT_MS) {
            if (stopped(gen)) {
                r = END_STOP;
                break;
            }
#ifdef _WIN32
            WSAPOLLFD p = {.fd = s, .events = POLLWRNORM};
            int k = WSAPoll(&p, 1, 100);
            int done = k > 0, bad = k > 0 && (p.revents & (POLLERR | POLLHUP));
#else
            struct pollfd p = {.fd = s, .events = POLLOUT};
            int k = poll(&p, 1, 100);
            int done = k > 0, bad = k > 0 && (p.revents & (POLLERR | POLLHUP));
#endif
            if (k < 0) break;
            if (done) {
                int err = 0;
                socklen_t len = sizeof err;
                getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &len);
                ok = !bad && !err;
                break;
            }
            waited += 100;
        }
        if (ok) {
            C.s = s;
            r = END_NONE;
        } else {
            sock_close(s);
        }
    }
    freeaddrinfo(res);
    if (r == END_LOST) snprintf(C.why, sizeof C.why, "Could not reach the server at %s.", C.addr_key);
    return r;
}

// Sends what is queued (the bye), ends the sending half and reads until the server closes, for at most half a second.
// Closing a socket that still holds unread bytes resets the connection on Windows, and a reset can throw away the bye
// before the server reads it: the others would then see the player held instead of gone.
static void goodbye(void) {
    static uint8_t sink[4096];
    for (int i = 0; i < 25 && C.nout; i++)
        if (flush_out() || (C.nout && sock_wait(C.s, 1, 20) < 0)) return;
#ifdef _WIN32
    shutdown(C.s, SD_SEND);
#else
    shutdown(C.s, SHUT_WR);
#endif
    uint64_t end = now_us() + 500000;
    while (now_us() < end) {
        int w = sock_wait(C.s, 0, 20);
        if (w < 0) return;
        if (!w) continue;
        int k = (int)recv(C.s, (char *)sink, sizeof sink, 0);
        if (k == 0 || (k < 0 && !sock_would_block())) return;
    }
}

static int attempt(unsigned gen) {
    C.why[0] = 0;
    C.phase = P_NONE;
    int r = tcp_connect(gen);
    if (r != END_NONE) return r;
    ow_link_init(&C.link, OW_ROLE_CLIENT);
    ow_reader_init(&C.rd);
    C.nout = 0;
    C.phase = P_GREETED;
    uint8_t rnd[64];
    char build[OW_BUILD_MAX + 1];
    snprintf(build, sizeof build, "TotK Recomp %s", rt_port_version);
    ow_greet g;
    if (ow_random(rnd, sizeof rnd) || ow_hs_client_greet(&C.hs, rnd, build, &g)) {
        wipe(rnd, sizeof rnd);
        snprintf(C.why, sizeof C.why, "This computer gave no random numbers for the online key exchange.");
        return END_FINAL;
    }
    wipe(rnd, sizeof rnd);
    send_msg(OW_T_greet, &g);
    uint64_t now = now_us();
    C.deadline = now + NC_HANDSHAKE_US;
    C.last_rx = now;
    C.next_ping = now + NC_PING_US;
    static uint8_t buf[16384];
    for (;;) {
        plat_mutex_lock(&S.mx);
        int gone = S.gen != gen, drop = S.test_drop;
        S.test_drop = 0;
        plat_mutex_unlock(&S.mx);
        if (gone) {
            if (C.phase >= P_PROOF) {   // sealed: an orderly goodbye
                ow_bye b = {.reason = OW_WHY_NONE};
                if (!send_msg(OW_T_bye, &b)) goodbye();
            }
            return END_STOP;
        }
        if (drop) {
            snprintf(C.why, sizeof C.why, "Lost the connection to the server.");
            return END_LOST;
        }
        now = now_us();
        if (C.phase == P_LIVE) {
            send_commands();
            send_reports(now);
        }
        if (C.phase >= P_JOINING && now >= C.next_ping) {
            ow_ping p = {.seq = ++C.ping_seq, .sent_us = now};
            send_msg(OW_T_ping, &p);
            C.next_ping = now + NC_PING_US;
        }
        if (C.why[0]) return END_LOST;   // send_msg found the queue full
        if (flush_out()) {
            snprintf(C.why, sizeof C.why, "Lost the connection to the server.");
            return END_LOST;
        }
        int w = sock_wait(C.s, C.nout > 0, NC_POLL_MS);
        if (w < 0) {
            snprintf(C.why, sizeof C.why, "Lost the connection to the server.");
            return END_LOST;
        }
        for (int reads = 0; w > 0 && reads < 64; reads++) {
            int k = (int)recv(C.s, (char *)buf, sizeof buf, 0);
            if (k < 0 && sock_would_block()) break;
            if (k <= 0) {
                if (!C.admitted)
                    snprintf(C.why, sizeof C.why, "The server at %s closed the connection during sign-in.", C.addr_key);
                else
                    snprintf(C.why, sizeof C.why, "Lost the connection to the server.");
                return END_LOST;
            }
            C.last_rx = now_us();
            const uint8_t *d = buf;
            size_t n = (size_t)k;
            while (n) {
                ow_frame f;
                int t = ow_reader_take(&C.link, &C.rd, &d, &n, &f);
                if (t < 0) {
                    snprintf(C.why, sizeof C.why, "The server sent something this game could not read (%s).", ow_strerror(t));
                    return END_FINAL;
                }
                if (!t) break;
                r = on_frame(&f, gen);
                if (r != END_NONE) return r;
            }
        }
        now = now_us();
        if (C.phase != P_LIVE && now > C.deadline) {
            snprintf(C.why, sizeof C.why, "The server at %s did not finish signing in.", C.addr_key);
            return C.admitted ? END_LOST : END_FINAL;
        }
        if (C.phase == P_LIVE && now - C.last_rx > NC_SILENCE_US) {
            snprintf(C.why, sizeof C.why, "Lost the connection to the server.");
            return END_LOST;
        }
    }
}

static void conn_close(void) {
    sock_close(C.s);
    C.s = NSOCK_BAD;
    ow_hs_wipe(&C.hs);
    ow_link_wipe(&C.link);
    C.nout = 0;
}

// A whole session: the first attempt, then reconnects within the hold. Returns once it ended for good or the game
// asked for something else.
static void session(unsigned gen) {
    char why[NET_TEXT_MAX];
    C.admitted = C.resumed_once = 0;
    C.account = 0;
    C.hold_ms = 0;
    C.options = OW_RULE_TELEPORT;
    if (!address_parse(C.cfg.address)) {
        snprintf(why, sizeof why, "\"%s\" is not a server address: write host:port.", C.cfg.address);
        pub_state(NET_OFFLINE, why);
        return;
    }
    if (!identity_load(why, sizeof why)) {
        pub_state(NET_OFFLINE, why);
        return;
    }
    pin_load();
    rt_log("net: connecting to %s", C.addr_key);
    uint64_t lost_at = 0;
    for (;;) {
        int r = attempt(gen);
        int was_live = C.phase == P_LIVE;
        conn_close();
        ow_identity_wipe(&C.id);
        if (r == END_STOP) break;
        if (r == END_FINAL || !C.admitted || !C.hold_ms) {
            room_clear();
            pub_state(NET_OFFLINE, C.why);
            break;
        }
        // lost after a join: the server holds the session for hold_ms
        uint64_t now = now_us();
        if (was_live || !lost_at) {
            lost_at = now;
            rt_log("net: %s Reconnecting for up to %u s", C.why, C.hold_ms / 1000);
            pub_state(NET_CONNECTING, "");
        }
        if (now - lost_at > (uint64_t)C.hold_ms * 1000u) {
            room_clear();
            pub_state(NET_OFFLINE, "Lost the connection to the server.");
            break;
        }
        for (int i = 0; i < NC_RETRY_MS / 50 && !stopped(gen); i++) plat_sleep_ms(50);
        if (stopped(gen)) break;
        char tmp[NET_TEXT_MAX];
        if (!identity_load(tmp, sizeof tmp)) {
            room_clear();
            pub_state(NET_OFFLINE, tmp);
            break;
        }
    }
    ow_identity_wipe(&C.id);
}

static void net_main(void *arg) {
    (void)arg;
    if (!sock_init()) {
        pub_state(NET_OFFLINE, "The network could not be started.");
        return;
    }
    unsigned done = 0;
    for (;;) {
        plat_mutex_lock(&S.mx);
        unsigned gen = S.gen;
        int want = S.want;
        if (gen != done && want) {
            C.cfg = S.cfg;
            snprintf(C.dir, sizeof C.dir, "%s", S.dir);
            S.ncmd = 0;
        }
        plat_mutex_unlock(&S.mx);
        if (gen == done) {
            plat_sleep_ms(50);
            continue;
        }
        done = gen;
        if (want) session(gen);
        // a stop: offline, unless the game already asked for the next connection
        plat_mutex_lock(&S.mx);
        int next = S.gen != gen && S.want;
        plat_mutex_unlock(&S.mx);
        if (!next && C.nrows) room_clear();
        if (!next) {
            plat_mutex_lock(&S.mx);
            int quiet = S.state == NET_OFFLINE;
            plat_mutex_unlock(&S.mx);
            if (!quiet) pub_state(NET_OFFLINE, "");
        }
    }
}

// ---- the header's functions (present thread)

int net_enabled(void) {
    if (fake()) return 1;
    const char *a = cfg_get("TOTK_NET_ADDRESS");
    return (a && *a) || G.state != NET_OFFLINE;
}

int net_connect(const NetConfig *cfg) {
    if (fake()) return net_fake_connect(cfg);
    if (!cfg || !cfg->address[0]) {
        plat_mutex_lock(&S.mx);
        snprintf(S.status, sizeof S.status, "No server address: enter one in the Online tab.");
        plat_mutex_unlock(&S.mx);
        snprintf(G.status, sizeof G.status, "%s", S.status);
        return 0;
    }
    const RtConfig *rc = rt_config();
    const char *profile = rc && rc->profile && *rc->profile ? rc->profile : rt_default_profile;
    plat_mutex_lock(&S.mx);
    S.cfg = *cfg;
    snprintf(S.dir, sizeof S.dir, "%s/online", profile);
    S.want = 1;
    S.gen++;
    S.ncmd = 0;
    if (S.state != NET_CONNECTING) {
        S.state = NET_CONNECTING;
        S.status[0] = 0;
        NetEvent e = {.type = NET_EV_STATE};
        ev_put_locked(&e);
    }
    int start = !S.started;
    S.started = 1;
    plat_mutex_unlock(&S.mx);
    G.state = NET_CONNECTING;
    if (start && !plat_thread_spawn(net_main, NULL, "net", 0)) {
        plat_mutex_lock(&S.mx);
        S.started = 0;
        S.state = NET_OFFLINE;
        snprintf(S.status, sizeof S.status, "The network thread could not be started.");
        plat_mutex_unlock(&S.mx);
        return 0;
    }
    return 1;
}

void net_disconnect(void) {
    if (fake()) {
        net_fake_disconnect();
        return;
    }
    plat_mutex_lock(&S.mx);
    if (S.want || S.state != NET_OFFLINE) {
        S.want = 0;
        S.gen++;
    }
    plat_mutex_unlock(&S.mx);
}

NetState net_state(void) { return fake() ? net_fake_state() : G.state; }
uint32_t net_self_id(void) { return fake() ? net_fake_self_id() : G.state == NET_ONLINE ? G.self : 0; }
const char *net_status_text(void) { return fake() ? net_fake_status_text() : G.status; }

static NetPlayer *g_find(uint32_t id) {
    for (int i = 0; i < G.npl; i++)
        if (G.pl[i].id == id) return &G.pl[i];
    return NULL;
}

// The local player's own row shows where the game says it is, not the last report the server echoed.
static void self_apply(void) {
    NetPlayer *me = g_find(G.self);
    if (!me) return;
    if (G.have_pos) memcpy(me->pos, G.pos, sizeof me->pos);
    if (G.area[0]) snprintf(me->area, sizeof me->area, "%s", G.area);
}

static void g_chat_put(const NetChatLine *l) {
    G.chat[G.nchat % NET_CHAT_KEEP] = *l;
    G.nchat++;
    G.serial++;
}

static void g_system(const char *text) {
    NetChatLine l;
    memset(&l, 0, sizeof l);
    l.time_ms = wall_ms();
    l.channel = NET_CHAT_SYSTEM;
    copy_text(l.text, sizeof l.text, text);
    g_chat_put(&l);
}

void net_tick(void) {
    if (fake()) {
        net_fake_tick();
        return;
    }
    static NetChatLine lines[NET_CHAT_KEEP];
    static NetEvent evs[NC_EVENTS];
    plat_mutex_lock(&S.mx);
    G.state = S.state;
    G.self = S.self;
    snprintf(G.status, sizeof G.status, "%s", S.status);
    int fresh = S.roster_ver != G.roster_ver;
    if (fresh) {
        memcpy(G.pl, S.pl, (size_t)S.npl * sizeof *G.pl);
        G.npl = S.npl;
        memcpy(G.team, S.team, (size_t)S.nteam * sizeof *G.team);
        G.nteam = S.nteam;
        G.teleport_on = S.teleport_on;
        G.roster_ver = S.roster_ver;
    }
    int nl = S.nchat, ne = S.nev;
    memcpy(lines, S.chat, (size_t)nl * sizeof *lines);
    memcpy(evs, S.ev, (size_t)ne * sizeof *evs);
    S.nchat = S.nev = 0;
    plat_mutex_unlock(&S.mx);
    self_apply();
    for (int i = 0; i < nl; i++) g_chat_put(&lines[i]);
    for (int i = 0; i < ne; i++) {
        if (G.nev == NC_EVENTS) memmove(G.ev, G.ev + 1, (size_t)(--G.nev) * sizeof *G.ev);
        G.ev[G.nev++] = evs[i];
    }
}

int net_players(NetPlayer *out, int max) {
    if (fake()) return net_fake_players(out, max);
    if (G.state != NET_ONLINE) return 0;
    int n = G.npl < max ? G.npl : max;
    if (n > 0) memcpy(out, G.pl, (size_t)n * sizeof *out);
    return n;
}

int net_player_by_id(uint32_t id, NetPlayer *out) {
    if (fake()) return net_fake_player_by_id(id, out);
    NetPlayer *p = G.state == NET_ONLINE ? g_find(id) : NULL;
    if (p && out) *out = *p;
    return p != NULL;
}

static NetPlayer *g_find_name(const char *name) {
    for (int i = 0; name && i < G.npl; i++)
        if (ieq(G.pl[i].name, name)) return &G.pl[i];
    return NULL;
}

int net_player_by_name(const char *name, NetPlayer *out) {
    if (fake()) return net_fake_player_by_name(name, out);
    NetPlayer *p = G.state == NET_ONLINE ? g_find_name(name) : NULL;
    if (p && out) *out = *p;
    return p != NULL;
}

int net_teams(NetTeam *out, int max) {
    if (fake()) return net_fake_teams(out, max);
    if (G.state != NET_ONLINE) return 0;
    int n = G.nteam < max ? G.nteam : max;
    if (n > 0) memcpy(out, G.team, (size_t)n * sizeof *out);
    return n;
}

void net_set_local(const char *area, const float pos[3], const char *character) {
    if (fake()) {
        net_fake_set_local(area, pos, character);
        return;
    }
    if (area) copy_text(G.area, sizeof G.area, area);
    if (character) copy_text(G.character, sizeof G.character, character);
    if (pos) memcpy(G.pos, pos, sizeof G.pos), G.have_pos = 1;
    self_apply();
    plat_mutex_lock(&S.mx);
    if (pos) memcpy(S.pos, pos, sizeof S.pos), S.have_pos = 1;
    S.character = !strcmp(G.character, "Zelda") ? OW_CHAR_ZELDA : OW_CHAR_LINK;
    plat_mutex_unlock(&S.mx);
}

int net_chat_count(void) {
    if (fake()) return net_fake_chat_count();
    return G.nchat < NET_CHAT_KEEP ? G.nchat : NET_CHAT_KEEP;
}

int net_chat_get(int index, NetChatLine *out) {
    if (fake()) return net_fake_chat_get(index, out);
    int n = net_chat_count();
    if (index < 0 || index >= n) return 0;
    *out = G.chat[(G.nchat - n + index) % NET_CHAT_KEEP];
    return 1;
}

uint32_t net_chat_serial(void) { return fake() ? net_fake_chat_serial() : G.serial; }

// Queues a message for the network thread. 0 when the queue is full (the server's own rate limits are far lower).
static int push(const Cmd *c) {
    plat_mutex_lock(&S.mx);
    int ok = S.ncmd < NC_CMDS;
    if (ok) S.cmd[S.ncmd++] = *c;
    plat_mutex_unlock(&S.mx);
    if (!ok) g_system("Too many requests at once: wait a moment.");
    return ok;
}

static NetPlayer *g_self(void) { return G.state == NET_ONLINE ? g_find(G.self) : NULL; }

int net_chat_send(NetChannel channel, const char *whisper_to, const char *text) {
    if (fake()) return net_fake_chat_send(channel, whisper_to, text);
    NetPlayer *me = g_self();
    if (!me || !text || !*text) return 0;
    Cmd c;
    memset(&c, 0, sizeof c);
    c.type = OW_T_say;
    c.m.say.kind = OW_SAY_ALL;
    if (channel == NET_CHAT_WHISPER) {
        NetPlayer *t = g_find_name(whisper_to);
        if (!t) {
            char line[NET_TEXT_MAX];
            snprintf(line, sizeof line, "No player called \"%s\" is here.", whisper_to ? whisper_to : "");
            g_system(line);
            return 0;
        }
        c.m.say.kind = OW_SAY_WHISPER;
        c.m.say.to = t->id;
    } else if (channel == NET_CHAT_TEAM) {
        if (!me->team) {
            g_system("You are not in a team.");
            return 0;
        }
        c.m.say.kind = OW_SAY_TEAM;
    } else if (!strncmp(text, "/me ", 4)) {
        c.m.say.kind = OW_SAY_ME;
        text += 4;
    }
    copy_text(c.m.say.text, sizeof c.m.say.text, text);
    return push(&c);
}

int net_teleport_request(uint32_t player) {
    if (fake()) return net_fake_teleport_request(player);
    if (!g_self() || player == G.self || !g_find(player)) return 0;
    if (!G.teleport_on) {
        g_system("Teleports are turned off on this server.");
        return 0;
    }
    Cmd c = {.type = OW_T_tp_ask, .m.tp_ask = {.target = player}};
    return push(&c);
}

void net_teleport_answer(uint32_t request, int accept) {
    if (fake()) {
        net_fake_teleport_answer(request, accept);
        return;
    }
    if (!g_self()) return;
    Cmd c = {.type = OW_T_tp_reply, .m.tp_reply = {.from = request, .accept = accept ? 1 : 0}};
    push(&c);
}

int net_team_create(const char *name) {
    if (fake()) return net_fake_team_create(name);
    if (!g_self()) return 0;
    Cmd c;
    memset(&c, 0, sizeof c);
    c.type = OW_T_team_create;
    copy_text(c.m.team_create.name, sizeof c.m.team_create.name, name && *name ? name : "My Team");
    return push(&c);
}

int net_team_invite(uint32_t player) {
    if (fake()) return net_fake_team_invite(player);
    NetPlayer *me = g_self();
    if (!me || !g_find(player) || player == G.self) return 0;
    if (!me->team) {
        g_system("Make a team first, then invite players to it.");
        return 0;
    }
    Cmd c = {.type = OW_T_team_invite, .m.team_invite = {.target = player}};
    return push(&c);
}

void net_team_answer(uint32_t invite, int accept) {
    if (fake()) {
        net_fake_team_answer(invite, accept);
        return;
    }
    if (!g_self()) return;
    if (!accept) {
        g_system("You declined the team invite.");
        return;
    }
    Cmd c = {.type = OW_T_team_join, .m.team_join = {.team = (uint16_t)invite}};
    push(&c);
}

void net_team_leave(void) {
    if (fake()) {
        net_fake_team_leave();
        return;
    }
    NetPlayer *me = g_self();
    if (!me || !me->team) return;
    Cmd c = {.type = OW_T_team_leave, .m.team_leave = {.team = (uint16_t)me->team}};
    push(&c);
}

int net_host_action(NetHostAction action, uint32_t target, const char *text) {
    if (fake()) return net_fake_host_action(action, target, text);
    NetPlayer *me = g_self();
    if (!me || me->role == NET_ROLE_PLAYER) return 0;
    int host = me->role == NET_ROLE_HOST;
    Cmd c;
    memset(&c, 0, sizeof c);
    if (action == NET_HOST_MESSAGE) {
        if (!text || !*text) return 0;
        c.type = OW_T_announce;
        copy_text(c.m.announce.text, sizeof c.m.announce.text, text);
        return push(&c);
    }
    if (action == NET_HOST_SUMMON_ALL) {
        if (!host) return 0;
        c.type = OW_T_summon;
        c.m.summon.scope = OW_SUMMON_ALL;
        return push(&c);
    }
    if (!g_find(target) || target == G.self) return 0;
    c.type = OW_T_mod_action;
    c.m.mod_action.target = target;
    switch (action) {
    case NET_HOST_KICK: c.m.mod_action.action = OW_MOD_KICK; break;
    case NET_HOST_BAN:
        if (!host) return 0;
        c.m.mod_action.action = OW_MOD_BAN;
        break;
    case NET_HOST_MUTE: c.m.mod_action.action = text && *text == '0' ? OW_MOD_UNMUTE : OW_MOD_MUTE; break;
    case NET_HOST_GRANT_MOD:
    case NET_HOST_REVOKE_MOD:
        if (!host) return 0;
        c.m.mod_action.action = OW_MOD_SET_RANK;
        c.m.mod_action.rank = action == NET_HOST_GRANT_MOD ? OW_RANK_MODERATOR : OW_RANK_PLAYER;
        break;
    default: return 0;
    }
    if (action == NET_HOST_KICK || action == NET_HOST_BAN)
        copy_text(c.m.mod_action.reason, sizeof c.m.mod_action.reason, text);
    return push(&c);
}

int net_poll_events(NetEvent *out, int max) {
    if (fake()) return net_fake_poll_events(out, max);
    int n = G.nev < max ? G.nev : max;
    if (n > 0) memcpy(out, G.ev, (size_t)n * sizeof *out);
    memmove(G.ev, G.ev + n, (size_t)(G.nev - n) * sizeof *G.ev);
    G.nev -= n;
    return n;
}

// Tests (tests/net_client_check.py): close the socket without a goodbye, as a failing network would, so the reconnect
// within the hold can be checked.
void net_test_drop(void) {
    plat_mutex_lock(&S.mx);
    S.test_drop = 1;
    plat_mutex_unlock(&S.mx);
}
