/* The room: connections, the handshake, sessions, the player list, positions, chat, teams, teleports, host tools
 * and the host's console.
 *
 * Rules are those of docs/online-wire.md, sections 6 to 8; what the host sees and sets is docs/server.md. Every
 * client is treated as hostile:
 * - the wire layer refuses any malformed, oversized, unsealed, replayed or out-of-stage frame, and any such error
 *   ends the connection with refuse OW_WHY_PROTOCOL;
 * - a connection that does not finish the handshake in handshake_seconds, or sends no frame for idle_seconds, ends
 *   (slow or stalled senders);
 * - each address has a limit on connections, handshakes in progress, new connections per minute and tries at new
 *   accounts per hour, and the server on handshakes in progress overall, since each greet costs a signature;
 * - frames of any kind beyond frame_burst / frames_per_second cut the client off; chat and requests have their own,
 *   smaller budgets that drop the excess with a system line instead;
 * - a client that does not read what it is sent is cut off once SV_OUT_MAX bytes wait for it;
 * - every request is checked against the sender's rank and the room's state before anything changes, and a refused
 *   request changes nothing.
 * Single-threaded: only sv_main.c's loop calls in here.
 */
#include "sv.h"
#include "sv_econ.h"
#include "sv_actors.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#define SV_OUT_MAX (256 * 1024)
#define SEC 1000000ull
#define TP_TIMEOUT_US (30 * SEC)
#define CLOSE_LINGER_US (2 * SEC)
#define INTEREST_M 250.0f

typedef struct bucket {
    double tokens;
    uint64_t at;
} bucket;

/* Takes one token; refills at rate per second up to burst. Returns 0 when none is left. */
static int take(bucket *b, double burst, double rate, uint64_t now) {
    if (!b->at) {
        b->tokens = burst;
        b->at = now;
    }
    b->tokens += (double)(now - b->at) / 1e6 * rate;
    b->at = now;
    if (b->tokens > burst) b->tokens = burst;
    if (b->tokens < 1.0) return 0;
    b->tokens -= 1.0;
    return 1;
}

enum { C_FREE, C_GREET, C_PROOF, C_LIVE, C_CLOSING };

typedef struct conn {
    int state;
    sv_sock s;
    char addr[64];
    ow_link link;
    ow_reader reader;
    ow_handshake hs;
    uint8_t *out;
    size_t out_off, out_len;
    int shut;              /* the sending half is closed (closing, everything sent) */
    uint64_t opened, last_frame, close_at, next_ping, ping_sent;
    uint32_t ping_seq;
    int player;            /* index into P, -1 before admit */
    bucket frames;
} conn;

typedef struct player {
    int used;
    uint64_t account;
    uint8_t key[32];
    char nick[OW_NICK_MAX + 1];
    uint8_t character, rank;
    int conn;              /* index into C, -1 while held */
    uint64_t held_until;
    uint16_t team;
    uint64_t team_seq;     /* when the player joined its team, for handing the lead on */
    ow_place area;
    uint16_t hearts_q, hearts_max_q, ping_ms;
    uint8_t fps;
    int has_pos;
    uint32_t scene;
    float pos[3], yaw;
    uint64_t tp_to, tp_deadline; /* the player's open teleport request */
    bucket chat, action;   /* kept across reconnects, so leaving and coming back does not refill them */
    uint32_t features;     /* the add-ons granted to its current connection */
    uint64_t warned;
    uint64_t join_seq;     /* order of first joining, for the sky leader */
    uint64_t active_at;    /* when the player last moved, spoke or acted */
    int afk;               /* away: nothing from the player for afk_seconds */
    bucket eco;            /* finds of Lights and seeds, which every player's ledger shows */
    bucket sky;            /* clock updates from the leader */
} player;

typedef struct team {
    uint16_t id;           /* 0: slot unused */
    char name[OW_TEAM_NAME_MAX + 1];
    uint64_t leader;
    uint8_t open;
    uint64_t invited[OW_PLAYERS_MAX];
    int ninvited;
} team;

typedef struct ip_entry {
    char addr[64];
    bucket connects;
    bucket accounts;       /* tries to make an account, good or bad claim alike */
    uint64_t seen;
} ip_entry;

static struct {
    sv_config cfg;
    sv_sock listener;
    uint64_t now, next_second;
    conn *c[SV_MAX_CONNS];
    player p[OW_PLAYERS_MAX];
    team t[OW_TEAMS_MAX];
    uint32_t next_team;
    uint64_t team_seq;
    uint64_t join_counter;
    ow_sky_info sky_info;  /* the leader's last clock */
    int sky_known;
    uint64_t sky_leader;   /* account the players were last told leads; 0 for none */
    ip_entry ips[256];
    int roster_dirty, roster_soft, teams_dirty;
    unsigned refused;      /* connections refused at accept since the last log line about them */
    uint64_t refused_logged;
} G;

static uint8_t g_frame[OW_FRAME_MAX];

/* ---- sending ---- */

static void conn_end(conn *c, int notify, uint16_t reason, const char *detail, int hold);
static ip_entry *ip_of(const char *addr);

static void conn_write(conn *c, uint16_t type, const void *msg) {
    if (c->state == C_FREE || c->state == C_CLOSING) return;
    size_t n;
    int e = ow_frame_write(&c->link, type, msg, g_frame, sizeof g_frame, &n);
    if (e) {
        /* Everything the server sends is built by the server; an error here is a server bug, not the client's. */
        sv_log("%s: cannot send %s: %s", c->addr, ow_msg(type) ? ow_msg(type)->name : "?", ow_strerror(e));
        return;
    }
    if (c->out_off && c->out_len + n > SV_OUT_MAX) {
        memmove(c->out, c->out + c->out_off, c->out_len - c->out_off);
        c->out_len -= c->out_off;
        c->out_off = 0;
    }
    if (c->out_len + n > SV_OUT_MAX) {
        sv_log("%s: not reading what it is sent; cut off", c->addr);
        conn_end(c, 0, OW_WHY_TIMEOUT, "", c->state == C_LIVE);
        return;
    }
    memcpy(c->out + c->out_len, g_frame, n);
    c->out_len += n;
}

static int live(const player *p) { return p->used && p->conn >= 0; }

/* The player did something a person does: it is no longer away. */
static void active(player *p) {
    p->active_at = G.now;
    if (p->afk) {
        p->afk = 0;
        G.roster_dirty = 1;
    }
}

static void send_to(player *p, uint16_t type, const void *msg) {
    if (live(p)) conn_write(G.c[p->conn], type, msg);
}

static void send_all(uint16_t type, const void *msg, const player *except) {
    for (int i = 0; i < OW_PLAYERS_MAX; i++)
        if (&G.p[i] != except) send_to(&G.p[i], type, msg);
}

static void vline(char *out, size_t max, const char *fmt, va_list ap) {
    char buf[1024];
    vsnprintf(buf, sizeof buf, fmt, ap);
    sv_copy_cut(out, max, buf);
}

/* A system chat line to one player. */
static void sys_to(player *p, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void sys_to(player *p, const char *fmt, ...) {
    ow_chat m;
    memset(&m, 0, sizeof m);
    m.kind = OW_CHAT_SYSTEM;
    snprintf(m.nickname, sizeof m.nickname, "Server");
    va_list ap;
    va_start(ap, fmt);
    vline(m.text, OW_TEXT_MAX, fmt, ap);
    va_end(ap);
    send_to(p, OW_T_chat, &m);
}

static void sys_all(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void sys_all(const char *fmt, ...) {
    ow_chat m;
    memset(&m, 0, sizeof m);
    m.kind = OW_CHAT_SYSTEM;
    snprintf(m.nickname, sizeof m.nickname, "Server");
    va_list ap;
    va_start(ap, fmt);
    vline(m.text, OW_TEXT_MAX, fmt, ap);
    va_end(ap);
    send_all(OW_T_chat, &m, NULL);
}

static void notice_all(uint8_t kind, const char *text) {
    ow_notice n;
    memset(&n, 0, sizeof n);
    n.kind = kind;
    sv_copy_cut(n.text, OW_NOTICE_MAX, text);
    send_all(OW_T_notice, &n, NULL);
}

/* ---- lookups ---- */

static player *find_player(uint64_t account) {
    if (!account) return NULL;
    for (int i = 0; i < OW_PLAYERS_MAX; i++)
        if (G.p[i].used && G.p[i].account == account) return &G.p[i];
    return NULL;
}

static player *find_player_key(const uint8_t key[32]) {
    for (int i = 0; i < OW_PLAYERS_MAX; i++)
        if (G.p[i].used && !memcmp(G.p[i].key, key, 32)) return &G.p[i];
    return NULL;
}

static int player_count(void) {
    int n = 0;
    for (int i = 0; i < OW_PLAYERS_MAX; i++) n += G.p[i].used;
    return n;
}

static team *find_team(uint64_t id) {
    if (!id || id > 0xFFFF) return NULL;
    for (int i = 0; i < OW_TEAMS_MAX; i++)
        if (G.t[i].id == id) return &G.t[i];
    return NULL;
}

static int team_members(const team *t) {
    int n = 0;
    for (int i = 0; i < OW_PLAYERS_MAX; i++) n += G.p[i].used && G.p[i].team == t->id;
    return n;
}

static int muted(const player *p) { return sv_sanction_find(&g_store.mutes, p->key) != NULL; }

static const char *rank_name(int r) {
    return r == OW_RANK_HOST ? "host" : r == OW_RANK_MODERATOR ? "moderator" : "player";
}

/* "until lifted", or how long is left. */
static void span_text(char *out, size_t n, int64_t until) {
    if (!until) {
        snprintf(out, n, "until lifted");
        return;
    }
    int64_t left = until - sv_unix_time();
    if (left < 60) left = 60;
    if (left < 3600) snprintf(out, n, "for %lld more minutes", (long long)(left / 60));
    else if (left < 86400 * 2) snprintf(out, n, "for %lld more hours", (long long)(left / 3600));
    else snprintf(out, n, "for %lld more days", (long long)(left / 86400));
}

/* Trims ASCII spaces at both ends into out. */
static void trimmed(char *out, size_t max, const char *in) {
    while (*in == ' ') in++;
    size_t n = strlen(in);
    while (n && in[n - 1] == ' ') n--;
    if (n > max) n = max;
    memcpy(out, in, n);
    out[n] = 0;
    sv_copy_cut(out, max, out);
}

/* ---- what everyone sees ---- */

static void rules_of(ow_room_rules *r) {
    r->max_players = (uint8_t)G.cfg.max_players;
    r->interest_m = INTEREST_M;
    r->hold_ms = (uint32_t)G.cfg.hold_seconds * 1000u;
    r->options = G.cfg.teleport ? OW_RULE_TELEPORT : 0;
}

static void roster_of(ow_roster *r) {
    memset(r, 0, sizeof *r);
    for (int i = 0; i < OW_PLAYERS_MAX && r->players_count < OW_PLAYERS_MAX; i++) {
        player *p = &G.p[i];
        if (!p->used) continue;
        ow_roster_row *w = &r->players[r->players_count++];
        w->account = p->account;
        snprintf(w->nickname, sizeof w->nickname, "%s", p->nick);
        w->character = p->character;
        w->rank = p->rank;
        w->afk = !live(p) || p->afk;
        w->muted = (uint8_t)muted(p);
        w->team = p->team;
        w->area = p->area;
        w->hearts_q = p->hearts_q;
        w->hearts_max_q = p->hearts_max_q;
        w->ping_ms = p->ping_ms;
        w->fps = p->fps;
    }
}

static void teams_of(ow_teams *t) {
    memset(t, 0, sizeof *t);
    for (int i = 0; i < OW_TEAMS_MAX; i++) {
        if (!G.t[i].id) continue;
        ow_team_row *w = &t->list[t->list_count++];
        w->id = G.t[i].id;
        snprintf(w->name, sizeof w->name, "%s", G.t[i].name);
        w->leader = G.t[i].leader;
        w->open = G.t[i].open;
        w->members = (uint8_t)team_members(&G.t[i]);
    }
}

static void positions_for(const player *viewer, ow_positions *m) {
    memset(m, 0, sizeof *m);
    int all = !G.cfg.positions_team_only || viewer->rank == OW_RANK_HOST;
    for (int i = 0; i < OW_PLAYERS_MAX; i++) {
        player *p = &G.p[i];
        if (!p->used || !p->has_pos || p == viewer) continue;
        if (!all && (!viewer->team || p->team != viewer->team)) continue;
        ow_spot *s = &m->spots[m->spots_count++];
        s->account = p->account;
        s->scene = p->scene;
        memcpy(s->pos, p->pos, sizeof s->pos);
        s->yaw = p->yaw;
    }
}

static void send_rules_all(void) {
    ow_rules r;
    rules_of(&r.rules);
    send_all(OW_T_rules, &r, NULL);
}

/* ---- teams ---- */

static void team_drop_invites_of(uint64_t account) {
    for (int i = 0; i < OW_TEAMS_MAX; i++) {
        team *t = &G.t[i];
        for (int k = 0; k < t->ninvited; k++)
            if (t->invited[k] == account) t->invited[k--] = t->invited[--t->ninvited];
    }
}

/* Takes p out of its team: the last member ends the team, a leader hands the lead to the earliest member. */
static void team_leave_of(player *p) {
    team *t = find_team(p->team);
    p->team = 0;
    G.teams_dirty = G.roster_dirty = 1;
    if (!t) return;
    player *first = NULL;
    for (int i = 0; i < OW_PLAYERS_MAX; i++) {
        player *q = &G.p[i];
        if (q->used && q->team == t->id && (!first || q->team_seq < first->team_seq)) first = q;
    }
    if (!first) {
        memset(t, 0, sizeof *t);
        return;
    }
    if (t->leader == p->account) t->leader = first->account;
}

static void team_put(player *p, team *t) {
    if (p->team) team_leave_of(p);
    p->team = t->id;
    p->team_seq = ++G.team_seq;
    for (int k = 0; k < t->ninvited; k++)
        if (t->invited[k] == p->account) t->invited[k--] = t->invited[--t->ninvited];
    G.teams_dirty = G.roster_dirty = 1;
}

static void on_team_create(player *p, const ow_team_create *m) {
    team *t = NULL;
    for (int i = 0; i < OW_TEAMS_MAX && !t; i++)
        if (!G.t[i].id) t = &G.t[i];
    if (!t) {
        sys_to(p, "There are already %d teams.", OW_TEAMS_MAX);
        return;
    }
    if (G.next_team > 0xFFFF) {
        sys_to(p, "No team ids are left until the server restarts.");
        return;
    }
    char name[OW_TEAM_NAME_MAX + 1];
    trimmed(name, OW_TEAM_NAME_MAX, m->name);
    memset(t, 0, sizeof *t);
    t->id = (uint16_t)G.next_team++;
    if (!name[0]) snprintf(name, sizeof name, "Team %u", t->id);
    snprintf(t->name, sizeof t->name, "%s", name);
    t->open = m->open;
    t->leader = p->account;
    team_put(p, t); /* leaves the old team first, which may hand its lead on */
    sv_log("%s made team %u \"%s\" (%s)", p->nick, t->id, t->name, t->open ? "open" : "invite only");
}

static void on_team_invite(player *p, const ow_team_invite *m) {
    team *t = find_team(p->team);
    player *q = find_player(m->target);
    if (!t) {
        sys_to(p, "You are not in a team.");
        return;
    }
    if (!q || !live(q) || q == p) {
        sys_to(p, "That player is not online.");
        return;
    }
    if (q->team == t->id) {
        sys_to(p, "%s is already in your team.", q->nick);
        return;
    }
    int known = 0;
    for (int k = 0; k < t->ninvited; k++) known |= t->invited[k] == q->account;
    if (!known) {
        if (t->ninvited == OW_PLAYERS_MAX) memmove(t->invited, t->invited + 1, sizeof t->invited - sizeof *t->invited),
                                           t->ninvited--;
        t->invited[t->ninvited++] = q->account;
    }
    ow_team_invited w;
    memset(&w, 0, sizeof w);
    w.team = t->id;
    snprintf(w.name, sizeof w.name, "%s", t->name);
    w.from = p->account;
    snprintf(w.nickname, sizeof w.nickname, "%s", p->nick);
    send_to(q, OW_T_team_invited, &w);
}

static void on_team_join(player *p, const ow_team_join *m) {
    team *t = find_team(m->team);
    if (!t) {
        sys_to(p, "That team does not exist.");
        return;
    }
    if (p->team == t->id) return;
    int invited = 0;
    for (int k = 0; k < t->ninvited; k++) invited |= t->invited[k] == p->account;
    if (!t->open && !invited) {
        sys_to(p, "Team %s is invite only.", t->name);
        return;
    }
    team_put(p, t);
    sv_log("%s joined team %u", p->nick, t->id);
}

static void on_team_leave(player *p, const ow_team_leave *m) {
    if (!p->team || p->team != m->team) {
        sys_to(p, "You are not in that team.");
        return;
    }
    sv_log("%s left team %u", p->nick, p->team);
    team_leave_of(p);
}

static void on_team_kick(player *p, const ow_team_kick *m) {
    team *t = find_team(p->team);
    player *q = find_player(m->target);
    if (!t || t->leader != p->account) {
        sys_to(p, "Only the team's leader can remove members.");
        return;
    }
    if (!q || q == p || q->team != t->id) {
        sys_to(p, "That player is not in your team.");
        return;
    }
    team_leave_of(q);
    sys_to(q, "%s removed you from team %s.", p->nick, t->name);
    sv_log("%s removed %s from team %u", p->nick, q->nick, t->id);
}

/* ---- teleports ---- */

static void tp_result(player *p, uint64_t target, uint8_t outcome) {
    ow_tp_result r = { .target = target, .outcome = outcome };
    send_to(p, OW_T_tp_result, &r);
}

/* Everyone asking to go to `target` learns that it went away. */
static void tp_target_gone(const player *target) {
    for (int i = 0; i < OW_PLAYERS_MAX; i++) {
        player *q = &G.p[i];
        if (q->used && q->tp_to == target->account) {
            q->tp_to = 0;
            tp_result(q, target->account, OW_TP_GONE);
        }
    }
}

static void go_to(player *p, const player *to, uint8_t cause) {
    ow_go_to g;
    memset(&g, 0, sizeof g);
    g.target = to->account;
    g.scene = to->scene;
    memcpy(g.pos, to->pos, sizeof g.pos);
    g.cause = cause;
    send_to(p, OW_T_go_to, &g);
}

static void on_tp_ask(player *p, const ow_tp_ask *m) {
    player *q = find_player(m->target);
    if (!G.cfg.teleport || q == p) {
        tp_result(p, m->target, OW_TP_NOT_ALLOWED);
        return;
    }
    if (!q || !live(q)) {
        tp_result(p, m->target, OW_TP_GONE);
        return;
    }
    if (p->tp_to) {
        tp_result(p, m->target, OW_TP_BUSY);
        return;
    }
    p->tp_to = q->account;
    p->tp_deadline = G.now + TP_TIMEOUT_US;
    ow_tp_asked a;
    memset(&a, 0, sizeof a);
    a.from = p->account;
    snprintf(a.nickname, sizeof a.nickname, "%s", p->nick);
    send_to(q, OW_T_tp_asked, &a);
}

static void on_tp_reply(player *q, const ow_tp_reply *m) {
    player *p = find_player(m->from);
    if (!p || p->tp_to != q->account) return; /* no such request open, or it already ended: nothing to answer */
    p->tp_to = 0;
    if (!m->accept) {
        tp_result(p, q->account, OW_TP_REFUSED);
        return;
    }
    if (!G.cfg.teleport) {
        tp_result(p, q->account, OW_TP_NOT_ALLOWED);
        return;
    }
    if (!q->has_pos) {
        tp_result(p, q->account, OW_TP_GONE);
        sys_to(p, "%s's position is not known yet.", q->nick);
        return;
    }
    go_to(p, q, OW_GO_REQUEST);
    tp_result(p, q->account, OW_TP_ACCEPTED);
    sv_log("%s teleports to %s", p->nick, q->nick);
}

static void on_summon(player *p, const ow_summon *m) {
    if (p->rank != OW_RANK_HOST) {
        tp_result(p, m->target, OW_TP_NOT_ALLOWED);
        return;
    }
    if (!p->has_pos) {
        sys_to(p, "Your position is not known yet.");
        return;
    }
    team *t = m->scope == OW_SUMMON_TEAM ? find_team(m->target) : NULL;
    int n = 0;
    for (int i = 0; i < OW_PLAYERS_MAX; i++) {
        player *q = &G.p[i];
        if (!live(q) || q == p) continue;
        if (m->scope == OW_SUMMON_ONE && q->account != m->target) continue;
        if (m->scope == OW_SUMMON_TEAM && (!t || q->team != t->id)) continue;
        go_to(q, p, OW_GO_SUMMON);
        n++;
    }
    if (!n) {
        sys_to(p, "Nobody to summon.");
        return;
    }
    sys_to(p, "Summoned %d player%s.", n, n == 1 ? "" : "s");
    sv_log("%s summoned %d players", p->nick, n);
}

/* ---- leaving ---- */

static void player_remove(player *p, uint16_t reason) {
    if (!p->used) return;
    sv_log("%s (account %llu) left: %s", p->nick, (unsigned long long)p->account,
           reason == OW_WHY_NONE ? "goodbye" : reason == OW_WHY_TIMEOUT ? "timed out" :
           reason == OW_WHY_KICKED ? "kicked" : reason == OW_WHY_BANNED ? "banned" :
           reason == OW_WHY_PROTOCOL ? "broke the protocol" : reason == OW_WHY_SHUTDOWN ? "shutdown" : "other");
    if (p->conn >= 0 && G.c[p->conn]) G.c[p->conn]->player = -1;
    p->conn = -1;
    if (p->team) team_leave_of(p);
    team_drop_invites_of(p->account);
    tp_target_gone(p);
    uint64_t account = p->account;
    if (p->features & OW_FEAT_ACTORS) sva_player_leave(account, G.now);
    memset(p, 0, sizeof *p);
    p->conn = -1;
    ow_depart d = { .account = account, .reason = reason };
    send_all(OW_T_depart, &d, NULL);
    G.roster_dirty = 1;
}

static void player_hold(player *p) {
    p->conn = -1;
    p->held_until = G.now + (uint64_t)G.cfg.hold_seconds * SEC;
    p->tp_to = 0;
    tp_target_gone(p);
    if (p->features & OW_FEAT_ACTORS) sva_player_away(p->account, G.now);
    G.roster_dirty = 1;
    sv_log("%s dropped; held for %d s", p->nick, G.cfg.hold_seconds);
}

/* Ends a connection. notify: send refuse with reason and detail first. hold: keep the player's session for the
 * hold time instead of removing it (a lost connection, not a goodbye, kick or protocol error). */
static void conn_end(conn *c, int notify, uint16_t reason, const char *detail, int hold) {
    if (c->state == C_FREE || c->state == C_CLOSING) return;
    if (notify) {
        ow_refuse r;
        ow_refuse_make(&r, reason, SV_BUILD, detail);
        conn_write(c, OW_T_refuse, &r);
        if (c->state == C_CLOSING) return; /* the write itself cut the connection off */
    }
    int pi = c->player;
    c->player = -1;
    c->state = C_CLOSING;
    c->close_at = notify ? G.now + CLOSE_LINGER_US : 0;
    if (pi >= 0) {
        player *p = &G.p[pi];
        p->conn = -1;
        if (hold && G.cfg.hold_seconds > 0) player_hold(p);
        else player_remove(p, reason);
    }
}

static void protocol_error(conn *c, int err) {
    sv_log("%s: %s; disconnected", c->addr, ow_strerror(err));
    conn_end(c, 1, OW_WHY_PROTOCOL, ow_strerror(err), 0);
}

/* ---- chat ---- */

static void chat_command(player *p, const char *text) {
    char cmd[16];
    size_t n = 0;
    for (const char *s = text + 1; *s && *s != ' ' && n + 1 < sizeof cmd; s++) cmd[n++] = (char)tolower((unsigned char)*s);
    cmd[n] = 0;
    if (!strcmp(cmd, "help")) {
        sys_to(p, "Commands: /who lists players, /teams lists teams, /motd shows the welcome message.");
    } else if (!strcmp(cmd, "who")) {
        char line[OW_TEXT_MAX + 1];
        int k = snprintf(line, sizeof line, "%d of %d online:", player_count(), G.cfg.max_players);
        for (int i = 0; i < OW_PLAYERS_MAX; i++) {
            player *q = &G.p[i];
            if (!q->used) continue;
            char item[80];
            snprintf(item, sizeof item, " %s%s%s%s", q->nick, q->rank == OW_RANK_HOST ? " (host)" :
                     q->rank == OW_RANK_MODERATOR ? " (mod)" : "", live(q) ? "" : " (away)", ",");
            if ((size_t)k + strlen(item) > OW_TEXT_MAX) {
                sys_to(p, "%s", line);
                k = 0;
                line[0] = 0;
            }
            k += snprintf(line + k, sizeof line - (size_t)k, "%s", item);
        }
        if (k && line[k - 1] == ',') line[k - 1] = 0;
        sys_to(p, "%s", line);
    } else if (!strcmp(cmd, "teams")) {
        int any = 0;
        for (int i = 0; i < OW_TEAMS_MAX; i++) {
            team *t = &G.t[i];
            if (!t->id) continue;
            player *l = find_player(t->leader);
            sys_to(p, "Team %u %s: %d members, led by %s, %s.", t->id, t->name, team_members(t), l ? l->nick : "?",
                   t->open ? "open" : "invite only");
            any = 1;
        }
        if (!any) sys_to(p, "There are no teams.");
    } else if (!strcmp(cmd, "motd")) {
        ow_notice m;
        memset(&m, 0, sizeof m);
        m.kind = OW_NOTICE_MOTD;
        snprintf(m.text, sizeof m.text, "%s", G.cfg.motd);
        send_to(p, OW_T_notice, &m);
    } else {
        sys_to(p, "Unknown command. Type /help.");
    }
}

static void on_say(player *p, const ow_say *m) {
    if (!take(&p->chat, G.cfg.chat_burst, G.cfg.chat_per_second, G.now)) {
        if (G.now - p->warned > 2 * SEC) {
            p->warned = G.now;
            sys_to(p, "You are sending messages too fast.");
        }
        return;
    }
    char text[OW_TEXT_MAX + 1];
    trimmed(text, OW_TEXT_MAX, m->text);
    if (!text[0]) return;
    if (text[0] == '/' && (m->kind == OW_SAY_ALL || m->kind == OW_SAY_TEAM)) {
        chat_command(p, text);
        return;
    }
    sv_sanction *mute = sv_sanction_find(&g_store.mutes, p->key);
    if (mute) {
        char span[48];
        span_text(span, sizeof span, mute->until);
        sys_to(p, "You are muted %s.", span);
        return;
    }
    ow_chat c;
    memset(&c, 0, sizeof c);
    c.from = p->account;
    snprintf(c.nickname, sizeof c.nickname, "%s", p->nick);
    snprintf(c.text, sizeof c.text, "%s", text);
    const char *where = "all";
    switch (m->kind) {
    case OW_SAY_ALL:
    case OW_SAY_ME:
        c.kind = m->kind == OW_SAY_ME ? OW_CHAT_ME : OW_CHAT_ALL;
        where = m->kind == OW_SAY_ME ? "me" : "all";
        send_all(OW_T_chat, &c, NULL);
        break;
    case OW_SAY_TEAM: {
        if (!p->team) {
            sys_to(p, "You are not in a team.");
            return;
        }
        c.kind = OW_CHAT_TEAM;
        where = "team";
        for (int i = 0; i < OW_PLAYERS_MAX; i++)
            if (G.p[i].used && G.p[i].team == p->team) send_to(&G.p[i], OW_T_chat, &c);
        break;
    }
    case OW_SAY_WHISPER: {
        player *q = find_player(m->to);
        if (!q || !live(q) || q == p) {
            sys_to(p, "That player is not online.");
            return;
        }
        c.kind = OW_CHAT_WHISPER;
        c.to = q->account;
        where = "whisper";
        send_to(q, OW_T_chat, &c);
        send_to(p, OW_T_chat, &c);
        break;
    }
    default:
        return;
    }
    if (G.cfg.log_chat) sv_log("chat [%s] %s: %s", where, p->nick, text);
}

static void on_mark(player *p, const ow_mark *m) {
    ow_marker w;
    memset(&w, 0, sizeof w);
    w.account = p->account;
    w.slot = m->slot;
    w.on = m->on;
    w.scene = m->scene;
    memcpy(w.pos, m->pos, sizeof w.pos);
    for (int i = 0; i < OW_PLAYERS_MAX; i++) {
        player *q = &G.p[i];
        if (!q->used || q == p) continue;
        if (p->team && q->team != p->team) continue;
        send_to(q, OW_T_marker, &w);
    }
}

/* ---- host tools ---- */

enum { RANK_CONSOLE = OW_RANK_HOST + 1 };

static const char *action_name(int a) {
    static const char *n[] = { "kick", "ban", "unban", "mute", "unmute", "set rank" };
    return a >= 0 && a <= OW_MOD_LAST ? n[a] : "?";
}

/* Applies a host tool for actor (NULL: the console, which may do anything). Returns an OW_MOD_* outcome. */
static int mod_apply(player *actor, int action, uint64_t target, int rank, uint32_t minutes, const char *why) {
    int arank = actor ? actor->rank : RANK_CONSOLE;
    const char *aname = actor ? actor->nick : "The server";
    sv_account *acc = sv_account_by_id(target);
    if (arank == OW_RANK_PLAYER) return OW_MOD_NOT_ALLOWED;
    if (!acc) return OW_MOD_UNKNOWN;
    if (actor && actor->account == target) return OW_MOD_NOT_ALLOWED;
    int trank = sv_rank_of(acc->key);
    player *tp = find_player(target);
    char reason[OW_REASON_MAX + 1];
    trimmed(reason, OW_REASON_MAX, why ? why : "");
    switch (action) {
    case OW_MOD_KICK:
    case OW_MOD_MUTE:
    case OW_MOD_UNMUTE:
        if (arank == OW_RANK_MODERATOR && trank != OW_RANK_PLAYER) return OW_MOD_NOT_ALLOWED;
        if (arank == OW_RANK_HOST && trank == OW_RANK_HOST) return OW_MOD_NOT_ALLOWED;
        break;
    default:
        if (arank < OW_RANK_HOST || trank == OW_RANK_HOST) return OW_MOD_NOT_ALLOWED;
        if (action == OW_MOD_SET_RANK && rank == OW_RANK_HOST) return OW_MOD_NOT_ALLOWED;
    }
    int64_t until = minutes ? sv_unix_time() + (int64_t)minutes * 60 : 0;
    char span[48], detail[OW_DETAIL_MAX + 1];
    span_text(span, sizeof span, until);
    switch (action) {
    case OW_MOD_KICK:
        if (!tp) return OW_MOD_UNKNOWN;
        snprintf(detail, sizeof detail, "Kicked by %s%s%s", aname, reason[0] ? ": " : ".", reason);
        if (tp->conn >= 0) conn_end(G.c[tp->conn], 1, OW_WHY_KICKED, detail, 0);
        else player_remove(tp, OW_WHY_KICKED);
        sys_all("%s kicked %s%s%s", aname, acc->nick, reason[0] ? ": " : ".", reason);
        break;
    case OW_MOD_BAN:
        if (sv_sanction_find(&g_store.bans, acc->key)) return OW_MOD_ALREADY;
        sv_sanction_set(&g_store.bans, acc->key, until, reason);
        snprintf(detail, sizeof detail, "Banned %s by %s%s%s", span, aname, reason[0] ? ": " : ".", reason);
        if (tp && tp->conn >= 0) conn_end(G.c[tp->conn], 1, OW_WHY_BANNED, detail, 0);
        else if (tp) player_remove(tp, OW_WHY_BANNED);
        sys_all("%s banned %s %s%s%s", aname, acc->nick, span, reason[0] ? ": " : ".", reason);
        break;
    case OW_MOD_UNBAN:
        if (!sv_sanction_remove(&g_store.bans, acc->key)) return OW_MOD_ALREADY;
        sys_all("%s lifted the ban on %s.", aname, acc->nick);
        break;
    case OW_MOD_MUTE:
        if (sv_sanction_find(&g_store.mutes, acc->key)) return OW_MOD_ALREADY;
        sv_sanction_set(&g_store.mutes, acc->key, until, reason);
        G.roster_dirty = 1;
        sys_all("%s muted %s %s%s%s", aname, acc->nick, span, reason[0] ? ": " : ".", reason);
        break;
    case OW_MOD_UNMUTE:
        if (!sv_sanction_remove(&g_store.mutes, acc->key)) return OW_MOD_ALREADY;
        G.roster_dirty = 1;
        sys_all("%s unmuted %s.", aname, acc->nick);
        break;
    case OW_MOD_SET_RANK:
        if (trank == rank) return OW_MOD_ALREADY;
        sv_set_moderator(acc->key, rank == OW_RANK_MODERATOR);
        if (tp) tp->rank = (uint8_t)rank;
        G.roster_dirty = 1;
        sys_all("%s made %s a %s.", aname, acc->nick, rank_name(rank));
        break;
    default:
        return OW_MOD_NOT_ALLOWED;
    }
    sv_log("%s: %s %s (account %llu) %s%s%s", aname, action_name(action), acc->nick, (unsigned long long)acc->id,
           action == OW_MOD_BAN || action == OW_MOD_MUTE ? span : "", reason[0] ? " reason: " : "", reason);
    return OW_MOD_DONE;
}

static void on_mod_action(player *p, const ow_mod_action *m) {
    int outcome = mod_apply(p, m->action, m->target, m->rank, m->minutes, m->reason);
    if (outcome != OW_MOD_DONE)
        sv_log("%s: %s on account %llu refused (outcome %d)", p->nick, action_name(m->action),
               (unsigned long long)m->target, outcome);
    ow_mod_done d = { .action = m->action, .target = m->target, .outcome = (uint8_t)outcome };
    send_to(p, OW_T_mod_done, &d);
}

static void on_announce(player *p, const ow_announce *m) {
    if (p->rank < OW_RANK_MODERATOR) {
        sys_to(p, "Only the host and moderators can send server messages.");
        return;
    }
    char text[OW_NOTICE_MAX + 1];
    trimmed(text, OW_NOTICE_MAX, m->text);
    if (!text[0]) return;
    notice_all(OW_NOTICE_ANNOUNCE, text);
    sv_log("%s announced: %s", p->nick, text);
}

/* ---- economy and sky ---- */

static int has_feature(const player *p, uint32_t f) { return live(p) && (p->features & f) == f; }

static void send_wallet(player *p, uint32_t seq, uint32_t rupees, int32_t granted) {
    ow_wallet w = { .seq = seq, .rupees = rupees, .granted = granted };
    send_to(p, OW_T_wallet, &w);
}

static void send_ledger(player *p) {
    sv_ledger_view v;
    sv_ledger_get(p->account, &v);
    ow_ledger l = { .lights_found = v.lights_found, .lights_spent = v.lights_spent, .seeds_found = v.seeds_found,
                    .seeds_spent = v.seeds_spent };
    send_to(p, OW_T_ledger, &l);
}

static void on_wallet_ask(player *p, const ow_wallet_ask *m) {
    uint32_t rupees;
    int32_t granted = sv_wallet_apply(p->account, m->cause, m->delta, G.now, &rupees);
    if (granted != m->delta)
        sv_log("%s: wallet change %d asked (cause %u), %d granted", p->nick, (int)m->delta, m->cause, (int)granted);
    send_wallet(p, m->seq, rupees, granted);
}

static void on_ledger_note(player *p, const ow_ledger_note *m) {
    switch (m->what) {
    case OW_LEDGER_LIGHT_FOUND:
    case OW_LEDGER_SEED_FOUND: {
        if (!m->key) return;
        /* Finds are shared by everyone, so a client may not stuff the ledger with invented ones. */
        if (!take(&p->eco, 20, 2.0, G.now)) return;
        int seed = m->what == OW_LEDGER_SEED_FOUND;
        if (!sv_ledger_find(seed ? SV_LEDGER_SEED : SV_LEDGER_LIGHT, m->key)) {
            send_ledger(p);
            return;
        }
        sv_log("%s found %s %08x", p->nick, seed ? "a Korok seed" : "a Light of Blessing", m->key);
        for (int i = 0; i < OW_PLAYERS_MAX; i++)
            if (has_feature(&G.p[i], OW_FEAT_ECONOMY)) send_ledger(&G.p[i]);
        break;
    }
    default: {
        int seed = m->what == OW_LEDGER_SEED_SPENT;
        uint32_t got = sv_ledger_spend(p->account, seed ? SV_LEDGER_SEED : SV_LEDGER_LIGHT, m->count);
        if (got != m->count) sv_log("%s: spent %u asked, %u allowed", p->nick, m->count, got);
        send_ledger(p);
    }
    }
}

/* The leader of the sky clock (and so of the Blood Moon): the player who joined first of those here now. */
static player *sky_leader(void) {
    player *best = NULL;
    for (int i = 0; i < OW_PLAYERS_MAX; i++) {
        player *p = &G.p[i];
        if (has_feature(p, OW_FEAT_SKY) && (!best || p->join_seq < best->join_seq)) best = p;
    }
    return best;
}

/* Day SV_SKY_NONE says that nobody has sent a clock yet: the message then only tells who leads. */
#define SV_SKY_NONE 0xFFFFFFFFu

static void send_sky(player *to, uint64_t leader) {
    ow_sky_set m = { .leader = leader, .info = G.sky_info };
    if (!G.sky_known) m.info.day = SV_SKY_NONE;
    send_to(to, OW_T_sky_set, &m);
}

/* Tells everyone who leads when that changed. Without a clock yet there is nothing to send. */
static void sky_update_leader(void) {
    player *l = sky_leader();
    uint64_t now_leader = l ? l->account : 0;
    if (now_leader == G.sky_leader) return;
    G.sky_leader = now_leader;
    if (l) sv_log("%s leads the sky clock", l->nick);
    if (!l) return;
    for (int i = 0; i < OW_PLAYERS_MAX; i++)
        if (has_feature(&G.p[i], OW_FEAT_SKY)) send_sky(&G.p[i], now_leader);
}

static void on_sky(player *p, const ow_sky *m) {
    if (!G.sky_leader || p->account != G.sky_leader) return; /* only the leader's clock counts */
    if (!take(&p->sky, 10, 2.0, G.now)) return;
    if (!(m->info.hour >= 0.0f && m->info.hour < 24.0f) || m->info.day > 1000000u) return;
    G.sky_info = m->info;
    G.sky_known = 1;
    for (int i = 0; i < OW_PLAYERS_MAX; i++)
        if (&G.p[i] != p && has_feature(&G.p[i], OW_FEAT_SKY)) send_sky(&G.p[i], p->account);
}

/* What a joining or returning game is told at once: its rupees, the ledger and the leader's clock. */
static void send_economy_state(player *p) {
    if (p->features & OW_FEAT_ECONOMY) {
        send_wallet(p, 0, sv_wallet_get(p->account), 0);
        send_ledger(p);
    }
    if ((p->features & OW_FEAT_SKY) && G.sky_leader) send_sky(p, G.sky_leader);
}

/* ---- the handshake ---- */

static int nick_taken(const char *nick, const player *self) {
    for (int i = 0; i < OW_PLAYERS_MAX; i++) {
        const player *q = &G.p[i];
        if (q->used && q != self && strlen(q->nick) == strlen(nick)) {
            size_t k = 0;
            while (nick[k] && tolower((unsigned char)nick[k]) == tolower((unsigned char)q->nick[k])) k++;
            if (!nick[k]) return 1;
        }
    }
    return 0;
}

/* The name the room uses: the asked one trimmed, "Player" when empty or when it would pass for the server, and a
 * number after it when another player has it. */
static void make_nick(char *out, const char *want, const player *self) {
    char base[OW_NICK_MAX + 1];
    trimmed(base, OW_NICK_MAX, want);
    char low[OW_NICK_MAX + 1];
    size_t i = 0;
    for (; base[i]; i++) low[i] = (char)tolower((unsigned char)base[i]);
    low[i] = 0;
    if (!base[0] || !strcmp(low, "server") || !strcmp(low, "host")) snprintf(base, sizeof base, "Player");
    snprintf(out, OW_NICK_MAX + 1, "%s", base);
    for (int n = 2; nick_taken(out, self) && n < 1000; n++) {
        char suffix[8], cut[OW_NICK_MAX + 1];
        snprintf(suffix, sizeof suffix, " %d", n);
        sv_copy_cut(cut, OW_NICK_MAX - strlen(suffix), base);
        snprintf(out, OW_NICK_MAX + 1, "%s%s", cut, suffix);
    }
}

static void on_greet(conn *c, const ow_frame *f) {
    uint32_t magic;
    uint16_t version;
    if (ow_greet_peek(f->payload, f->len, &magic, &version) || magic != OW_MAGIC) {
        sv_log("%s: not a game client (bad magic); closed", c->addr);
        conn_end(c, 0, OW_WHY_PROTOCOL, "", 0);
        return;
    }
    if (version != OW_VERSION) {
        char detail[OW_DETAIL_MAX + 1];
        snprintf(detail, sizeof detail, "This server speaks protocol %d and the game protocol %u: update the %s.",
                 OW_VERSION, version, version < OW_VERSION ? "game" : "server");
        sv_log("%s: protocol %u refused", c->addr, version);
        conn_end(c, 1, OW_WHY_VERSION, detail, 0);
        return;
    }
    ow_greet g;
    int e = ow_decode(OW_T_greet, f->payload, f->len, &g);
    if (e) {
        protocol_error(c, e);
        return;
    }
    uint8_t rnd[64];
    ow_challenge ch;
    if (ow_random(rnd, sizeof rnd)) {
        conn_end(c, 1, OW_WHY_SHUTDOWN, "The server has no random numbers.", 0);
        return;
    }
    e = ow_hs_server_greet(&c->hs, &g_store.identity, rnd, &g, &ch);
    memset(rnd, 0, sizeof rnd);
    if (e) {
        protocol_error(c, e);
        return;
    }
    conn_write(c, OW_T_challenge, &ch);
    if ((e = ow_hs_seal(&c->hs, &c->link))) {
        protocol_error(c, e);
        return;
    }
    c->state = C_PROOF;
}

static void refuse_join(conn *c, uint16_t reason, const char *detail, const char *log_what) {
    sv_log("%s: refused, %s", c->addr, log_what);
    conn_end(c, 1, reason, detail, 0);
}

static void on_proof(conn *c, const ow_frame *f) {
    if (f->type != OW_T_proof) {
        sv_log("%s: %s before proof; disconnected", c->addr, ow_msg(f->type)->name);
        conn_end(c, 1, OW_WHY_PROTOCOL, "the first sealed frame must be proof", 0);
        return;
    }
    ow_proof pr;
    int e = ow_decode(OW_T_proof, f->payload, f->len, &pr);
    if (e) {
        protocol_error(c, e);
        return;
    }
    e = ow_hs_server_proof(&c->hs, &pr);
    ow_hs_wipe(&c->hs);
    if (e) {
        refuse_join(c, OW_WHY_AUTH, "Your key's signature did not check.", "bad signature");
        return;
    }
    char keyhex[65];
    sv_hex(keyhex, pr.identity, 32);
    sv_sanction *ban = sv_sanction_find(&g_store.bans, pr.identity);
    if (ban) {
        char span[48], detail[OW_DETAIL_MAX + 1];
        span_text(span, sizeof span, ban->until);
        snprintf(detail, sizeof detail, "You are banned from this room %s%s%s", span, ban->reason[0] ? ": " : ".",
                 ban->reason);
        refuse_join(c, OW_WHY_BANNED, detail, "banned key");
        return;
    }
    sv_account *acc = sv_account_by_key(pr.identity);
    player *p = find_player_key(pr.identity);
    int rank = g_store.has_host ? sv_rank_of(pr.identity) : OW_RANK_HOST;
    if (!p) {
        int n = player_count();
        if (n >= OW_PLAYERS_MAX || (n >= G.cfg.max_players && rank != OW_RANK_HOST)) {
            refuse_join(c, OW_WHY_FULL, "The room is full.", "room full");
            return;
        }
    }
    if (!acc) {
        /* Every try at a new account costs a token, so neither account spam nor guessing a password or a claim
         * code gets far from one address. */
        double n = G.cfg.new_accounts_per_hour;
        if (!take(&ip_of(c->addr)->accounts, n, n / 3600.0, G.now)) {
            refuse_join(c, OW_WHY_FULL, "Too many new accounts from your address: try again later.",
                        "too many new accounts from one address");
            return;
        }
        int ok = 0;
        if (pr.claim[0] && sv_claim_take(pr.claim)) ok = 1;
        else if (G.cfg.open && !G.cfg.password[0]) ok = 1;
        else if (G.cfg.open && pr.claim[0] && sv_password_matches(pr.claim, G.cfg.password)) ok = 1;
        if (!ok) {
            if (!pr.claim[0])
                refuse_join(c, OW_WHY_UNKNOWN_KEY, G.cfg.open ? "This room needs a password: type it as the claim code."
                                                              : "This room is invite only: ask the host for a claim code.",
                            "new key without a claim code");
            else
                refuse_join(c, OW_WHY_BAD_CLAIM, G.cfg.open ? "Wrong password." : "That claim code is unknown or used.",
                            "wrong claim code or password");
            return;
        }
    }
    int pi = -1, resumed = p != NULL;
    if (p) {
        pi = (int)(p - G.p);
        if (p->conn >= 0) {
            /* The newer connection takes the session over; the older one only learns why it ends. */
            conn *old = G.c[p->conn];
            sv_log("%s signed in again from %s; the older connection ends", p->nick, c->addr);
            old->player = -1;
            p->conn = -1;
            conn_end(old, 1, OW_WHY_DUPLICATE, "This account signed in again from another place.", 0);
        }
        p->held_until = 0;
    } else {
        for (int i = 0; i < OW_PLAYERS_MAX && pi < 0; i++)
            if (!G.p[i].used) pi = i;
        p = &G.p[pi];
        memset(p, 0, sizeof *p);
        p->used = 1;
        memcpy(p->key, pr.identity, 32);
        make_nick(p->nick, pr.nickname, p);
        if (!acc) {
            acc = sv_account_add(pr.identity, p->nick);
            sv_log("new account %llu for key %s", (unsigned long long)acc->id, keyhex);
        } else if (strcmp(acc->nick, p->nick)) {
            snprintf(acc->nick, sizeof acc->nick, "%s", p->nick);
            sv_accounts_save();
        }
        p->account = acc->id;
        p->area.state = OW_PLACE_TITLE;
        p->join_seq = ++G.join_counter;
    }
    if (!g_store.has_host) {
        sv_set_host(pr.identity);
        sv_log("%s is the first account and becomes the host (host.txt)", p->nick);
    }
    p->rank = (uint8_t)sv_rank_of(pr.identity);
    p->character = pr.character;
    int ci = -1;
    for (int i = 0; i < SV_MAX_CONNS; i++)
        if (G.c[i] == c) ci = i;
    p->conn = ci;
    c->player = pi;
    c->state = C_LIVE;
    c->last_frame = G.now;
    c->next_ping = G.now + SEC;

    ow_admit a;
    memset(&a, 0, sizeof a);
    a.account = p->account;
    snprintf(a.nickname, sizeof a.nickname, "%s", p->nick);
    a.rank = p->rank;
    a.resumed = (uint8_t)resumed;
    a.features = pr.features & SV_FEATURES_GRANTED;
    a.server_time_us = G.now;
    rules_of(&a.rules);
    conn_write(c, OW_T_admit, &a);
    ow_link_set_features(&c->link, a.features);
    /* The actors add-on knows only games that have it: one that resumes without it leaves the actors' room. */
    if ((p->features & OW_FEAT_ACTORS) && !(a.features & OW_FEAT_ACTORS)) sva_player_leave(p->account, G.now);
    p->features = a.features;
    active(p);
    if (a.features & OW_FEAT_ACTORS) {
        sva_player_join(p->account, G.now);
        sva_player_area(p->account, &p->area, G.now);
        if (p->has_pos) sva_player_pos(p->account, p->scene, p->pos, G.now);
    }

    static ow_roster roster;
    static ow_teams teams;
    static ow_positions pos;
    roster_of(&roster);
    teams_of(&teams);
    positions_for(p, &pos);
    conn_write(c, OW_T_roster, &roster);
    conn_write(c, OW_T_teams, &teams);
    conn_write(c, OW_T_positions, &pos);
    ow_join_ready jr = { 0, 0, roster.players_count };
    conn_write(c, OW_T_join_ready, &jr);
    sky_update_leader();
    send_economy_state(p);
    if (G.cfg.motd[0]) {
        ow_notice m;
        memset(&m, 0, sizeof m);
        m.kind = OW_NOTICE_MOTD;
        snprintf(m.text, sizeof m.text, "%s", G.cfg.motd);
        conn_write(c, OW_T_notice, &m);
    }
    if (!resumed) {
        ow_arrive ar;
        memset(&ar, 0, sizeof ar);
        ar.account = p->account;
        snprintf(ar.nickname, sizeof ar.nickname, "%s", p->nick);
        ar.character = p->character;
        send_all(OW_T_arrive, &ar, p);
    }
    G.roster_dirty = 1;
    sv_log("%s (account %llu, %s) %s from %s, key %s", p->nick, (unsigned long long)p->account, rank_name(p->rank),
           resumed ? "came back" : "joined", c->addr, keyhex);
}

/* ---- frames of an admitted session ---- */

#define SV_UNION_MEMBER(id, name, dir, stage, cap, feat) ow_##name name;
static union { OW_MESSAGES(SV_UNION_MEMBER) } g_msg;

static int action_ok(player *p) {
    if (take(&p->action, G.cfg.action_burst, G.cfg.action_per_second, G.now)) return 1;
    if (G.now - p->warned > 2 * SEC) {
        p->warned = G.now;
        sys_to(p, "Slow down: too many requests.");
    }
    return 0;
}

static void on_live(conn *c, const ow_frame *f) {
    int e = ow_decode(f->type, f->payload, f->len, &g_msg);
    if (e) {
        protocol_error(c, e);
        return;
    }
    player *p = &G.p[c->player];
    switch (f->type) {
    case OW_T_bye:
        sv_log("%s said goodbye", p->nick);
        conn_end(c, 0, OW_WHY_NONE, "", 0);
        break;
    case OW_T_ping: {
        ow_pong r = { .seq = g_msg.ping.seq, .echo_us = g_msg.ping.sent_us, .reply_us = G.now };
        conn_write(c, OW_T_pong, &r);
        break;
    }
    case OW_T_pong:
        if (c->ping_sent && g_msg.pong.seq == c->ping_seq) {
            uint64_t ms = (G.now - c->ping_sent) / 1000;
            uint16_t v = (uint16_t)(ms > 0xFFFF ? 0xFFFF : ms);
            c->ping_sent = 0;
            if (v != p->ping_ms) G.roster_soft = 1;
            p->ping_ms = v;
        }
        break;
    case OW_T_area:
        if (memcmp(&p->area, &g_msg.area.where, sizeof p->area)) {
            G.roster_soft = 1;
            active(p);
        }
        p->area = g_msg.area.where;
        if (p->features & OW_FEAT_ACTORS) sva_player_area(p->account, &p->area, G.now);
        break;
    case OW_T_vitals:
        if (p->hearts_q != g_msg.vitals.hearts_q || p->hearts_max_q != g_msg.vitals.hearts_max_q ||
            p->fps != g_msg.vitals.fps)
            G.roster_soft = 1;
        p->hearts_q = g_msg.vitals.hearts_q;
        p->hearts_max_q = g_msg.vitals.hearts_max_q;
        p->fps = g_msg.vitals.fps;
        break;
    case OW_T_position: {
        float dx = g_msg.position.pos[0] - p->pos[0], dy = g_msg.position.pos[1] - p->pos[1],
              dz = g_msg.position.pos[2] - p->pos[2];
        if (!p->has_pos || dx * dx + dy * dy + dz * dz > 0.25f || fabsf(g_msg.position.yaw - p->yaw) > 0.05f)
            active(p);
        p->has_pos = 1;
        p->scene = g_msg.position.scene;
        memcpy(p->pos, g_msg.position.pos, sizeof p->pos);
        p->yaw = g_msg.position.yaw;
        if (p->features & OW_FEAT_ACTORS) sva_player_pos(p->account, p->scene, p->pos, G.now);
        break;
    }
    case OW_T_character:
        if (p->character != g_msg.character.character) {
            p->character = g_msg.character.character;
            G.roster_dirty = 1;
        }
        break;
    case OW_T_say: active(p); on_say(p, &g_msg.say); break;
    case OW_T_wallet_ask: on_wallet_ask(p, &g_msg.wallet_ask); break;
    case OW_T_ledger_note: on_ledger_note(p, &g_msg.ledger_note); break;
    case OW_T_sky: on_sky(p, &g_msg.sky); break;
    case OW_T_mark: active(p); if (action_ok(p)) on_mark(p, &g_msg.mark); break;
    case OW_T_team_create: active(p); if (action_ok(p)) on_team_create(p, &g_msg.team_create); break;
    case OW_T_team_invite: active(p); if (action_ok(p)) on_team_invite(p, &g_msg.team_invite); break;
    case OW_T_team_join: active(p); if (action_ok(p)) on_team_join(p, &g_msg.team_join); break;
    case OW_T_team_leave: active(p); if (action_ok(p)) on_team_leave(p, &g_msg.team_leave); break;
    case OW_T_team_kick: active(p); if (action_ok(p)) on_team_kick(p, &g_msg.team_kick); break;
    case OW_T_tp_ask: active(p); if (action_ok(p)) on_tp_ask(p, &g_msg.tp_ask); break;
    case OW_T_tp_reply: active(p); if (action_ok(p)) on_tp_reply(p, &g_msg.tp_reply); break;
    case OW_T_summon: active(p); if (action_ok(p)) on_summon(p, &g_msg.summon); break;
    case OW_T_mod_action: active(p); if (action_ok(p)) on_mod_action(p, &g_msg.mod_action); break;
    case OW_T_announce: active(p); if (action_ok(p)) on_announce(p, &g_msg.announce); break;
    /* The actors add-on (sv_actors.c). The reader let these through only on a link that was granted it. */
    case OW_T_offer: case OW_T_release: case OW_T_actor_send: case OW_T_hold: case OW_T_hit: case OW_T_spawn:
    case OW_T_gone: case OW_T_group: case OW_T_herd: case OW_T_take:
        sva_message(p->account, f->type, &g_msg, G.now);
        break;
    default:
        /* A sealed client type the session has no use for (a second proof). The reader already refused every
         * type of an add-on this link was not granted. */
        sv_log("%s: unexpected %s; disconnected", p->nick, ow_msg(f->type)->name);
        conn_end(c, 1, OW_WHY_PROTOCOL, "unexpected message", 0);
    }
}

/* Whether a live connection's frame of this type is an actor frame: those have their own, larger budget
 * (sv_actors.h, SVA_FRAME_BURST), since a game offers hundreds of actors at once when it loads a busy place. */
static int actor_frame(const conn *c, uint16_t type) {
    return c->state == C_LIVE && c->player >= 0 && type >= OW_T_offer && type <= OW_T_taken;
}

static void on_frame(conn *c, const ow_frame *f) {
    int ok = actor_frame(c, f->type) ? sva_frame_ok(G.p[c->player].account, G.now)
                                     : take(&c->frames, G.cfg.frame_burst, G.cfg.frames_per_second, G.now);
    if (!ok) {
        sv_log("%s: flooding (over %d frames at once); disconnected", c->addr, G.cfg.frame_burst);
        conn_end(c, 1, OW_WHY_PROTOCOL, "too many frames", 0);
        return;
    }
    c->last_frame = G.now;
    if (c->state == C_GREET) on_greet(c, f);
    else if (c->state == C_PROOF) on_proof(c, f);
    else if (c->state == C_LIVE) on_live(c, f);
}

/* ---- sockets ---- */

static ip_entry *ip_of(const char *addr) {
    ip_entry *old = &G.ips[0];
    for (size_t i = 0; i < sizeof G.ips / sizeof G.ips[0]; i++) {
        ip_entry *e = &G.ips[i];
        if (e->seen && !strcmp(e->addr, addr)) {
            e->seen = G.now;
            return e;
        }
        if (e->seen < old->seen) old = e;
    }
    memset(old, 0, sizeof *old);
    snprintf(old->addr, sizeof old->addr, "%s", addr);
    old->seen = G.now;
    return old;
}

static void accept_new(void) {
    for (int round = 0; round < 32; round++) {
        char addr[64];
        sv_sock s = sv_accept(G.listener, addr, sizeof addr);
        if (s == SV_BAD_SOCK) return;
        ip_entry *ip = ip_of(addr);
        double per_min = G.cfg.connects_per_minute;
        double burst = per_min / 4 < 4 ? 4 : per_min / 4;
        int total = 0, pending = 0, all_pending = 0, slot = -1;
        for (int i = 0; i < SV_MAX_CONNS; i++) {
            conn *c = G.c[i];
            if (!c) {
                if (slot < 0) slot = i;
                continue;
            }
            int handshaking = c->state == C_GREET || c->state == C_PROOF;
            all_pending += handshaking;
            if (!strcmp(c->addr, addr)) {
                total++;
                pending += handshaking;
            }
        }
        conn *c = NULL;
        if (slot >= 0 && total < G.cfg.max_per_ip && pending < G.cfg.max_pending_per_ip &&
            all_pending < SV_MAX_PENDING && take(&ip->connects, burst, per_min / 60.0, G.now)) {
            c = calloc(1, sizeof *c);
            if (c) c->out = malloc(SV_OUT_MAX);
            if (c && !c->out) {
                free(c);
                c = NULL;
            }
        }
        if (!c) {
            sv_close(s);
            G.refused++;
            continue;
        }
        c->state = C_GREET;
        c->s = s;
        snprintf(c->addr, sizeof c->addr, "%s", addr);
        ow_link_init(&c->link, OW_ROLE_SERVER);
        ow_reader_init(&c->reader);
        c->opened = c->last_frame = G.now;
        c->player = -1;
        G.c[slot] = c;
    }
}

static void conn_free(int i) {
    conn *c = G.c[i];
    sv_close(c->s);
    ow_link_wipe(&c->link);
    ow_hs_wipe(&c->hs);
    free(c->out);
    free(c);
    G.c[i] = NULL;
}

static void conn_read(conn *c) {
    static uint8_t buf[16384];
    for (int round = 0; round < 4; round++) {
        int k = sv_recv(c->s, buf, sizeof buf);
        if (k == -1) return;
        if (k <= 0) {
            if (c->state == C_CLOSING) c->close_at = 0;
            else conn_end(c, 0, OW_WHY_TIMEOUT, "", c->state == C_LIVE);
            return;
        }
        if (c->state == C_CLOSING) continue; /* drain what a closing peer still sends, then close cleanly */
        const uint8_t *d = buf;
        size_t n = (size_t)k;
        while (n && c->state != C_CLOSING) {
            ow_frame f;
            int r = ow_reader_take(&c->link, &c->reader, &d, &n, &f);
            if (r < 0) {
                protocol_error(c, r);
                break;
            }
            if (r == 0) break;
            on_frame(c, &f);
        }
    }
}

static void conn_flush(conn *c) {
    while (c->out_off < c->out_len) {
        int k = sv_send(c->s, c->out + c->out_off, c->out_len - c->out_off);
        if (k < 0) {
            if (c->state == C_CLOSING) c->close_at = 0;
            else conn_end(c, 0, OW_WHY_TIMEOUT, "", c->state == C_LIVE);
            return;
        }
        if (k == 0) return;
        c->out_off += (size_t)k;
    }
    c->out_off = c->out_len = 0;
    if (c->state == C_CLOSING && !c->shut) {
        sv_shutdown_send(c->s);
        c->shut = 1;
    }
}

/* ---- timers ---- */

static void timers(void) {
    uint64_t now = G.now;
    for (int i = 0; i < SV_MAX_CONNS; i++) {
        conn *c = G.c[i];
        if (!c) continue;
        if ((c->state == C_GREET || c->state == C_PROOF) && now - c->opened > (uint64_t)G.cfg.handshake_seconds * SEC) {
            sv_log("%s: handshake not finished in %d s; closed", c->addr, G.cfg.handshake_seconds);
            conn_end(c, 1, OW_WHY_TIMEOUT, "The handshake took too long.", 0);
        } else if (c->state == C_LIVE && now - c->last_frame > (uint64_t)G.cfg.idle_seconds * SEC) {
            sv_log("%s: no frame for %d s", c->addr, G.cfg.idle_seconds);
            conn_end(c, 1, OW_WHY_TIMEOUT, "Nothing heard for too long.", 1);
        } else if (c->state == C_LIVE && now >= c->next_ping) {
            ow_ping m = { .seq = ++c->ping_seq, .sent_us = now };
            c->ping_sent = now;
            c->next_ping = now + SEC;
            conn_write(c, OW_T_ping, &m);
        }
    }
    for (int i = 0; i < OW_PLAYERS_MAX; i++) {
        player *p = &G.p[i];
        if (p->used && p->conn < 0 && now >= p->held_until) player_remove(p, OW_WHY_TIMEOUT);
    }
    if (now < G.next_second) return;
    G.next_second = now + SEC;
    for (int i = 0; i < OW_PLAYERS_MAX; i++) {
        player *p = &G.p[i];
        if (!live(p)) continue;
        if (p->tp_to && now >= p->tp_deadline) {
            uint64_t t = p->tp_to;
            p->tp_to = 0;
            tp_result(p, t, OW_TP_TIMEOUT);
        }
        static ow_positions pos;
        positions_for(p, &pos);
        send_to(p, OW_T_positions, &pos);
    }
    if (G.cfg.afk_seconds > 0)
        for (int i = 0; i < OW_PLAYERS_MAX; i++) {
            player *p = &G.p[i];
            if (live(p) && !p->afk && now - p->active_at > (uint64_t)G.cfg.afk_seconds * SEC) {
                p->afk = 1;
                G.roster_dirty = 1;
                sv_log("%s is away", p->nick);
            }
        }
    sv_econ_tick(now);
    if (G.roster_soft) G.roster_dirty = 1;
    G.roster_soft = 0;
    if (G.refused && now - G.refused_logged > 10 * SEC) {
        sv_log("refused %u connections over the limits", G.refused);
        G.refused = 0;
        G.refused_logged = now;
    }
}

static void send_dirty(void) {
    sky_update_leader();
    if (G.teams_dirty) {
        static ow_teams t;
        teams_of(&t);
        send_all(OW_T_teams, &t, NULL);
        G.teams_dirty = 0;
    }
    if (G.roster_dirty) {
        static ow_roster r;
        roster_of(&r);
        send_all(OW_T_roster, &r, NULL);
        G.roster_dirty = 0;
    }
}

/* ---- the actors add-on's way out ---- */

static void actors_send(void *ctx, uint64_t account, uint16_t type, const void *msg) {
    (void)ctx;
    player *p = find_player(account);
    if (p && (p->features & OW_FEAT_ACTORS)) send_to(p, type, msg);
}

static void actors_log(void *ctx, const char *line) {
    (void)ctx;
    sv_log("%s", line);
}

/* ---- the loop ---- */

int sv_room_start(const sv_config *cfg) {
    memset(&G, 0, sizeof G);
    G.cfg = *cfg;
    G.next_team = 1;
    for (int i = 0; i < OW_PLAYERS_MAX; i++) G.p[i].conn = -1;
    char err[160];
    G.listener = sv_listen(cfg->bind, cfg->port, err, sizeof err);
    if (G.listener == SV_BAD_SOCK) {
        sv_log("cannot listen: %s", err);
        return -1;
    }
    G.now = sv_now_us();
    sv_econ_open(cfg);
    sva_io io = { .send = actors_send, .log = actors_log };
    sva_init(&io, INTEREST_M);
    return 0;
}

void sv_room_poll(int timeout_ms) {
    static sv_pollfd fds[SV_MAX_CONNS + 1];
    static int idx[SV_MAX_CONNS + 1];
    size_t n = 0;
    sv_pollfd_set(&fds[n], G.listener, 1, 0);
    idx[n++] = -1;
    for (int i = 0; i < SV_MAX_CONNS; i++) {
        conn *c = G.c[i];
        if (!c) continue;
        sv_pollfd_set(&fds[n], c->s, 1, c->out_len > c->out_off);
        idx[n++] = i;
    }
    sv_poll(fds, n, timeout_ms);
    G.now = sv_now_us();
    for (size_t k = 1; k < n; k++) {
        conn *c = G.c[idx[k]];
        if (c && sv_poll_readable(&fds[k])) conn_read(c);
    }
    if (sv_poll_readable(&fds[0])) accept_new();
    timers();
    sva_tick(G.now);
    send_dirty();
    for (int i = 0; i < SV_MAX_CONNS; i++) {
        conn *c = G.c[i];
        if (!c) continue;
        if (c->out_len > c->out_off || (c->state == C_CLOSING && !c->shut)) conn_flush(c);
        if (c->state == C_CLOSING && G.now >= c->close_at) conn_free(i);
    }
}

void sv_room_stop(void) {
    G.now = sv_now_us();
    sva_shutdown();
    for (int i = 0; i < SV_MAX_CONNS; i++) {
        conn *c = G.c[i];
        if (!c) continue;
        if (c->state == C_LIVE) {
            ow_bye b;
            memset(&b, 0, sizeof b);
            b.reason = OW_WHY_SHUTDOWN;
            snprintf(b.detail, sizeof b.detail, "The server is shutting down.");
            conn_write(c, OW_T_bye, &b);
        }
        if (c->state != C_CLOSING) {
            c->player = -1;
            c->state = C_CLOSING;
            c->close_at = G.now + SEC;
        }
    }
    for (int round = 0; round < 100; round++) {
        int open = 0;
        G.now = sv_now_us();
        for (int i = 0; i < SV_MAX_CONNS; i++) {
            conn *c = G.c[i];
            if (!c) continue;
            conn_flush(c);
            if (c->shut || G.now >= c->close_at) conn_free(i);
            else open = 1;
        }
        if (!open) break;
        sv_sleep_ms(10);
    }
    sv_close(G.listener);
    sv_econ_save();
}

/* ---- console ---- */

static char *word(char **s) {
    while (**s == ' ' || **s == '\t') (*s)++;
    if (!**s) return NULL;
    char *w = *s;
    while (**s && **s != ' ' && **s != '\t') (*s)++;
    if (**s) *(*s)++ = 0;
    return w;
}

static char *rest(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

/* An account named by id ("12" or "#12"), its full 64-character key, or a name: online players first, then the
 * last names of all accounts. */
static sv_account *resolve(const char *who) {
    if (!who) return NULL;
    const char *d = who[0] == '#' ? who + 1 : who;
    if (*d && strspn(d, "0123456789") == strlen(d)) return sv_account_by_id(strtoull(d, NULL, 10));
    uint8_t key[32];
    if (strlen(who) == 64 && sv_unhex(key, 32, who) == 0) return sv_account_by_key(key);
    for (int i = 0; i < OW_PLAYERS_MAX; i++)
        if (G.p[i].used && !strcmp(G.p[i].nick, who)) return sv_account_by_id(G.p[i].account);
    sv_account *hit = NULL;
    for (int i = 0; i < g_store.nacc; i++)
        if (!strcmp(g_store.acc[i].nick, who)) {
            if (hit) {
                printf("more than one account was last called %s: use its id\n", who);
                return NULL;
            }
            hit = &g_store.acc[i];
        }
    return hit;
}

static void console_help(void) {
    printf("commands:\n"
           "  status                       the room, its address and the server's key\n"
           "  players                      who is here\n"
           "  say <text>                   a system line in everyone's chat\n"
           "  announce <text>              a server message on everyone's screen\n"
           "  kick <who> [reason]          <who> is an account id, a name or a key\n"
           "  ban <who> [minutes] [reason] no minutes: until lifted\n"
           "  unban <who>\n"
           "  mute <who> [minutes] [reason]\n"
           "  unmute <who>\n"
           "  mod <who> / unmod <who>      make or unmake a moderator\n"
           "  host <who>                   give the host's rank to another account\n"
           "  bans                         list bans and mutes\n"
           "  claim [count]                make one-time claim codes for new players\n"
           "  teleport on|off              allow or forbid teleport requests\n"
           "  positions all|team           who sees whose position\n"
           "  motd <text>                  the welcome message\n"
           "  reload                       re-read bans.txt, mutes.txt and moderators.txt\n"
           "  stop                         say goodbye to everyone and exit\n");
}

static void console_mod(int action, char *args) {
    char *who = word(&args);
    sv_account *a = resolve(who);
    if (!a) {
        /* unban also takes a bare key that has no account here */
        uint8_t key[32];
        if (action == OW_MOD_UNBAN && who && strlen(who) == 64 && sv_unhex(key, 32, who) == 0) {
            printf(sv_sanction_remove(&g_store.bans, key) ? "ban lifted\n" : "that key is not banned\n");
            return;
        }
        printf("no such account: %s\n", who ? who : "(none given)");
        return;
    }
    uint32_t minutes = 0;
    if (action == OW_MOD_BAN || action == OW_MOD_MUTE) {
        args = rest(args);
        size_t digits = strspn(args, "0123456789");
        if (digits && (args[digits] == 0 || args[digits] == ' ')) {
            minutes = (uint32_t)strtoul(args, NULL, 10);
            args += digits;
        }
    }
    int out = mod_apply(NULL, action, a->id, 0, minutes, rest(args));
    static const char *outcome[] = { "done", "not allowed", "unknown or offline", "already so" };
    printf("%s %s: %s\n", action_name(action), a->nick, outcome[out]);
}

int sv_console(const char *line_in) {
    char line[2048];
    snprintf(line, sizeof line, "%s", line_in);
    char *s = line, *cmd = word(&s);
    if (!cmd) return 0;
    G.now = sv_now_us();
    if (!strcmp(cmd, "help") || !strcmp(cmd, "?")) {
        console_help();
    } else if (!strcmp(cmd, "status")) {
        char hex[65];
        sv_hex(hex, g_store.identity.pub, 32);
        printf("%s, port %u, %d of %d players, %s, teleport %s, positions %s\nserver key %s\n", G.cfg.name,
               G.cfg.port, player_count(), G.cfg.max_players,
               !G.cfg.open ? "invite only" : G.cfg.password[0] ? "password" : "open", G.cfg.teleport ? "on" : "off",
               G.cfg.positions_team_only ? "team" : "all", hex);
    } else if (!strcmp(cmd, "players")) {
        int any = 0;
        for (int i = 0; i < OW_PLAYERS_MAX; i++) {
            player *p = &G.p[i];
            if (!p->used) continue;
            char hex[65];
            sv_hex(hex, p->key, 32);
            printf("  #%llu %-20s %-9s team %-5u %s %ums %s key %.16s\n", (unsigned long long)p->account, p->nick,
                   rank_name(p->rank), p->team, live(p) ? "online" : "held  ", p->ping_ms,
                   live(p) ? G.c[p->conn]->addr : "-", hex);
            any = 1;
        }
        if (!any) printf("nobody is here\n");
    } else if (!strcmp(cmd, "say")) {
        char text[OW_TEXT_MAX + 1];
        trimmed(text, OW_TEXT_MAX, rest(s));
        if (text[0]) sys_all("%s", text);
        sv_log("console said: %s", text);
    } else if (!strcmp(cmd, "announce")) {
        char text[OW_NOTICE_MAX + 1];
        trimmed(text, OW_NOTICE_MAX, rest(s));
        if (text[0]) notice_all(OW_NOTICE_ANNOUNCE, text);
        sv_log("console announced: %s", text);
    } else if (!strcmp(cmd, "kick")) {
        console_mod(OW_MOD_KICK, s);
    } else if (!strcmp(cmd, "ban")) {
        console_mod(OW_MOD_BAN, s);
    } else if (!strcmp(cmd, "unban")) {
        console_mod(OW_MOD_UNBAN, s);
    } else if (!strcmp(cmd, "mute")) {
        console_mod(OW_MOD_MUTE, s);
    } else if (!strcmp(cmd, "unmute")) {
        console_mod(OW_MOD_UNMUTE, s);
    } else if (!strcmp(cmd, "mod") || !strcmp(cmd, "unmod")) {
        char *w = word(&s);
        sv_account *acc = resolve(w);
        if (!acc) {
            printf("no such account: %s\n", w ? w : "(none given)");
        } else {
            int out = mod_apply(NULL, OW_MOD_SET_RANK, acc->id, cmd[0] == 'm' ? OW_RANK_MODERATOR : OW_RANK_PLAYER, 0, "");
            static const char *outcome[] = { "done", "not allowed", "unknown", "already so" };
            printf("%s %s: %s\n", cmd, acc->nick, outcome[out]);
        }
    } else if (!strcmp(cmd, "host")) {
        sv_account *a = resolve(word(&s));
        if (g_store.host_from_config) {
            printf("the host is set in server.toml (host = ...): change it there and restart\n");
        } else if (!a) {
            printf("no such account\n");
        } else {
            sv_set_moderator(a->key, 0);
            sv_set_host(a->key);
            for (int i = 0; i < OW_PLAYERS_MAX; i++)
                if (G.p[i].used) G.p[i].rank = (uint8_t)sv_rank_of(G.p[i].key);
            G.roster_dirty = 1;
            sys_all("%s is now the host.", a->nick);
            sv_log("console made %s (account %llu) the host", a->nick, (unsigned long long)a->id);
        }
    } else if (!strcmp(cmd, "bans")) {
        sv_sanctions *lists[2] = { &g_store.bans, &g_store.mutes };
        for (int l = 0; l < 2; l++) {
            uint8_t none[32] = { 0 };
            sv_sanction_find(lists[l], none); /* drops the entries that ended */
            printf("%s: %d\n", l ? "mutes" : "bans", lists[l]->n);
            for (int i = 0; i < lists[l]->n; i++) {
                sv_sanction *e = &lists[l]->v[i];
                sv_account *a = sv_account_by_key(e->key);
                char hex[65], span[48];
                sv_hex(hex, e->key, 32);
                span_text(span, sizeof span, e->until);
                printf("  %s %s %s %s\n", a ? a->nick : "?", hex, span, e->reason);
            }
        }
    } else if (!strcmp(cmd, "claim")) {
        char *w = word(&s);
        int count = w ? atoi(w) : 1;
        if (count < 1) count = 1;
        if (count > 50) count = 50;
        for (int i = 0; i < count; i++) {
            char code[32];
            sv_claim_new(code);
            printf("claim code: %s\n", code);
        }
        sv_log("console made %d claim codes", count);
    } else if (!strcmp(cmd, "teleport")) {
        char *w = word(&s);
        if (!w || (strcmp(w, "on") && strcmp(w, "off"))) {
            printf("teleport is %s; say teleport on or teleport off\n", G.cfg.teleport ? "on" : "off");
        } else {
            G.cfg.teleport = !strcmp(w, "on");
            send_rules_all();
            sv_log("console turned teleport %s", w);
        }
    } else if (!strcmp(cmd, "positions")) {
        char *w = word(&s);
        if (!w || (strcmp(w, "all") && strcmp(w, "team"))) {
            printf("positions: %s; say positions all or positions team\n", G.cfg.positions_team_only ? "team" : "all");
        } else {
            G.cfg.positions_team_only = !strcmp(w, "team");
            sv_log("console set positions to %s", w);
        }
    } else if (!strcmp(cmd, "motd")) {
        trimmed(G.cfg.motd, OW_NOTICE_MAX, rest(s));
        printf("welcome message: %s\n", G.cfg.motd);
    } else if (!strcmp(cmd, "reload")) {
        sv_store_reload();
        for (int i = 0; i < OW_PLAYERS_MAX; i++)
            if (G.p[i].used) G.p[i].rank = (uint8_t)sv_rank_of(G.p[i].key);
        G.roster_dirty = 1;
        printf("reloaded: %d bans, %d mutes, %d moderators\n", g_store.bans.n, g_store.mutes.n, g_store.nmods);
    } else if (!strcmp(cmd, "stop") || !strcmp(cmd, "quit") || !strcmp(cmd, "exit")) {
        return 1;
    } else {
        printf("unknown command %s; type help\n", cmd);
    }
    fflush(stdout);
    send_dirty();
    return 0;
}
