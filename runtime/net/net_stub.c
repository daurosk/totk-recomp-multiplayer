// The offline implementation of net/net_client.h. Without TOTK_NET_FAKE=1 nothing is online: net_enabled says 0 and the
// UI stays hidden. With TOTK_NET_FAKE=1 it plays a small server by itself, so the whole UI can be tried with no network:
// the local player plus four invented players, a team, scripted chat, an invite and a teleport request arriving after
// a few seconds, and replies to whispers. TOTK_NET_FAKE_ROLE=player|mod|host (default host) sets the local role.
// The real client replaces this file with the same functions.
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "net/net_client.h"
#include "platform/platform.h"

static int g_fake = -1;
static NetState g_state;
static NetPlayer g_pl[NET_MAX_PLAYERS];
static int g_npl;
static NetTeam g_team[NET_MAX_TEAMS];
static int g_nteam;
static NetChatLine g_chat[NET_CHAT_KEEP];
static int g_nchat;   // total lines ever added; the ring holds the last NET_CHAT_KEEP
static uint32_t g_serial;
static NetEvent g_ev[64];
static int g_nev;
static uint32_t g_self, g_next_id = 10, g_next_req = 1, g_next_team = 2;
static uint64_t g_t0_ns, g_last_ns;
static unsigned g_script;   // next scripted step
static float g_local_pos[3];
static char g_local_area[NET_AREA_MAX] = "Unknown", g_local_char[NET_NAME_MAX] = "Link";
static char g_name[NET_NAME_MAX] = "You";
// The invented players stand within a hundred metres of the local player, so a teleport to one lands on ground the
// game has loaded.
static const float g_off[][3] = {{0, 0, 0}, {8, 2, -45}, {-60, 0, 30}, {1450, 30, 580}, {90, 0, 15}};

static int fake_on(void) {
    if (g_fake < 0) {
        const char *e = cfg_get("TOTK_NET_FAKE");
        g_fake = e && !strcmp(e, "1");
    }
    return g_fake;
}

int net_enabled(void) { return fake_on(); }

static uint64_t now_ms(void) { return plat_unix_time_ns() / 1000000ull; }

static void copy(char *dst, size_t n, const char *s) { snprintf(dst, n, "%s", s ? s : ""); }

static int ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return !*a && !*b;
}

static NetPlayer *find(uint32_t id) {
    for (int i = 0; i < g_npl; i++)
        if (g_pl[i].id == id) return &g_pl[i];
    return NULL;
}

static NetPlayer *add_player(uint32_t id, const char *name, const char *ch, const char *area, float x, float y, float z,
                             NetRole role, uint32_t team, uint32_t ping) {
    if (g_npl >= NET_MAX_PLAYERS) return NULL;
    NetPlayer *p = &g_pl[g_npl++];
    memset(p, 0, sizeof *p);
    p->id = id;
    copy(p->name, sizeof p->name, name);
    copy(p->character, sizeof p->character, ch);
    copy(p->area, sizeof p->area, area);
    p->pos[0] = x, p->pos[1] = y, p->pos[2] = z;
    p->role = role, p->team = team, p->ping_ms = ping;
    p->hearts_max_q = 32, p->hearts_q = (uint8_t)(6 + (id * 7) % 27);   // a scripted health for the pictures
    return p;
}

static void remove_player(NetPlayer *p) {
    memmove(p, p + 1, (size_t)(g_pl + g_npl - p - 1) * sizeof *p);
    g_npl--;
}

static void chat_add(NetChannel ch, uint32_t from, const char *from_name, const char *to_name, const char *text) {
    NetChatLine *l = &g_chat[g_nchat % NET_CHAT_KEEP];
    memset(l, 0, sizeof *l);
    l->time_ms = now_ms();
    l->channel = ch, l->from = from;
    copy(l->from_name, sizeof l->from_name, from_name);
    copy(l->to_name, sizeof l->to_name, to_name);
    copy(l->text, sizeof l->text, text);
    g_nchat++;
    g_serial++;
}

static NetEvent *ev_add(NetEventType type) {
    if (g_nev == (int)(sizeof g_ev / sizeof *g_ev)) memmove(g_ev, g_ev + 1, (size_t)(--g_nev) * sizeof *g_ev);
    NetEvent *e = &g_ev[g_nev++];
    memset(e, 0, sizeof *e);
    e->type = type;
    return e;
}

static void system_line(const char *text) { chat_add(NET_CHAT_SYSTEM, 0, "", "", text); }

static void system_fmt(const char *fmt, const char *a) {
    char t[NET_TEXT_MAX];
    snprintf(t, sizeof t, fmt, a ? a : "");
    system_line(t);
}

static void world_init(void) {
    NetRole role = NET_ROLE_HOST;
    const char *r = cfg_get("TOTK_NET_FAKE_ROLE");
    if (r && !strcmp(r, "player")) role = NET_ROLE_PLAYER;
    else if (r && !strcmp(r, "mod")) role = NET_ROLE_MOD;
    g_npl = g_nchat = g_nteam = g_nev = 0;
    g_self = 1;
    add_player(1, g_name, g_local_char, g_local_area, g_local_pos[0], g_local_pos[1], g_local_pos[2], role, 0, 0);
    add_player(2, "Alice", "Zelda", "Hyrule Castle", 0, 0, 0, NET_ROLE_PLAYER, 1, 28);
    add_player(3, "Bao", "Link", "Lookout Landing", 0, 0, 0, NET_ROLE_MOD, 0, 45);
    add_player(4, "Cleo", "Sidon", "Zora's Domain", 0, 0, 0, NET_ROLE_PLAYER, 0, 96);
    g_team[g_nteam++] = (NetTeam){1, "Red Squad", 2};
    g_t0_ns = g_last_ns = 0;   // the scripted traffic starts when the player is in the world (net_set_local)
    g_script = 0;
    system_fmt("Connected to the practice server as %s.", g_name);
    chat_add(NET_CHAT_ALL, 3, "Bao", "", "Welcome in! Hold the wheel key for the wheel menu.");
}

int net_connect(const NetConfig *cfg) {
    if (!fake_on()) return 0;
    if (cfg && cfg->name[0]) copy(g_name, sizeof g_name, cfg->name);
    g_state = NET_ONLINE;
    world_init();
    ev_add(NET_EV_STATE);
    return 1;
}

void net_disconnect(void) {
    if (g_state == NET_OFFLINE) return;
    g_state = NET_OFFLINE;
    g_npl = 0;
    ev_add(NET_EV_STATE);
}

NetState net_state(void) { return g_state; }
uint32_t net_self_id(void) { return g_state == NET_ONLINE ? g_self : 0; }

static void whisper_reply(const char *from, const char *text) {
    char t[NET_TEXT_MAX];
    snprintf(t, sizeof t, "got it: \"%.120s\"", text);
    for (int i = 0; i < g_npl; i++)
        if (ieq(g_pl[i].name, from)) chat_add(NET_CHAT_WHISPER, g_pl[i].id, g_pl[i].name, g_name, t);
}

// scripted traffic: seconds after the connection -> what happens
static void script_step(double t) {
    switch (g_script) {
    case 0:
        if (t < 3) return;
        chat_add(NET_CHAT_ALL, 2, "Alice", "", "anyone near the Great Plateau?");
        break;
    case 1:
        if (t < 6) return;
        chat_add(NET_CHAT_ALL, 4, "Cleo", "", "heading to Hateno, see you there");
        break;
    case 2:
        if (t < 8) return;
        chat_add(NET_CHAT_TEAM, 2, "Alice", "", "team chat works too, Red Squad checking in");
        break;
    case 3: {
        if (t < 10) return;
        NetEvent *e = ev_add(NET_EV_TEAM_INVITE);
        e->player = 4, e->request = g_next_req++;
        copy(e->name, sizeof e->name, "Cleo"), copy(e->text, sizeof e->text, "Zora Runners");
        break;
    }
    case 4: {
        if (t < 14) return;
        NetEvent *e = ev_add(NET_EV_TELEPORT_REQUEST);
        e->player = 2, e->request = g_next_req++;
        copy(e->name, sizeof e->name, "Alice");
        break;
    }
    case 5: {
        if (t < 20) return;
        NetPlayer *p = add_player(g_next_id++, "Dax", "Link", "Gerudo Town", 0, 0, 0, NET_ROLE_PLAYER, 0, 70);
        if (p) {
            NetEvent *e = ev_add(NET_EV_PLAYER_JOINED);
            e->player = p->id, copy(e->name, sizeof e->name, p->name);
        }
        system_line("Dax joined.");
        break;
    }
    case 6: {
        if (t < 30) return;
        NetPlayer *p = find(4);
        if (p) {
            NetEvent *e = ev_add(NET_EV_PLAYER_LEFT);
            e->player = 4, copy(e->name, sizeof e->name, "Cleo");
            remove_player(p);
        }
        system_line("Cleo left.");
        break;
    }
    default: return;
    }
    g_script++;
}

void net_tick(void) {
    if (g_state != NET_ONLINE) return;
    if (!g_t0_ns) return;
    uint64_t now = plat_now_ns();
    script_step((now - g_t0_ns) / 1e9);
    if (now - g_last_ns > 500000000ull) {   // their pings drift
        g_last_ns = now;
        for (int i = 1; i < g_npl; i++) g_pl[i].ping_ms = 20 + (uint32_t)((now / 700000000ull + (uint64_t)i * 13) % 90);
    }
    for (int i = 1; i < g_npl; i++) {   // and they keep their place around the local player
        int k = (int)(g_pl[i].id < 6 ? g_pl[i].id : 5) - 1;
        for (int c = 0; c < 3; c++) g_pl[i].pos[c] = g_local_pos[c] + g_off[k][c];
    }
}

int net_players(NetPlayer *out, int max) {
    if (g_state != NET_ONLINE) return 0;
    int n = g_npl < max ? g_npl : max;
    if (n > 0) memcpy(out, g_pl, (size_t)n * sizeof *out);
    return n;
}

int net_player_by_id(uint32_t id, NetPlayer *out) {
    NetPlayer *p = g_state == NET_ONLINE ? find(id) : NULL;
    if (p && out) *out = *p;
    return p != NULL;
}

int net_player_by_name(const char *name, NetPlayer *out) {
    if (g_state != NET_ONLINE || !name) return 0;
    for (int i = 0; i < g_npl; i++)
        if (ieq(g_pl[i].name, name)) {
            if (out) *out = g_pl[i];
            return 1;
        }
    return 0;
}

int net_teams(NetTeam *out, int max) {
    if (g_state != NET_ONLINE) return 0;
    int n = g_nteam < max ? g_nteam : max;
    if (n > 0) memcpy(out, g_team, (size_t)n * sizeof *out);
    return n;
}

void net_set_local(const char *area, const float pos[3], const char *character) {
    if (pos) memcpy(g_local_pos, pos, sizeof g_local_pos);
    if (pos && !g_t0_ns) g_t0_ns = g_last_ns = plat_now_ns();
    if (area) copy(g_local_area, sizeof g_local_area, area);
    if (character) copy(g_local_char, sizeof g_local_char, character);
    NetPlayer *p = g_state == NET_ONLINE ? find(g_self) : NULL;
    if (p) {
        memcpy(p->pos, g_local_pos, sizeof p->pos);
        copy(p->area, sizeof p->area, g_local_area);
        copy(p->character, sizeof p->character, g_local_char);
    }
}

int net_chat_count(void) { return g_nchat < NET_CHAT_KEEP ? g_nchat : NET_CHAT_KEEP; }

int net_chat_get(int index, NetChatLine *out) {
    int n = net_chat_count();
    if (index < 0 || index >= n) return 0;
    *out = g_chat[(g_nchat - n + index) % NET_CHAT_KEEP];
    return 1;
}

uint32_t net_chat_serial(void) { return g_serial; }

int net_chat_send(NetChannel channel, const char *whisper_to, const char *text) {
    if (g_state != NET_ONLINE || !text || !*text) return 0;
    NetPlayer *me = find(g_self);
    if (channel == NET_CHAT_WHISPER) {
        NetPlayer t;
        if (!whisper_to || !net_player_by_name(whisper_to, &t)) {
            system_fmt("No player called \"%s\" is here.", whisper_to);
            return 0;
        }
        chat_add(NET_CHAT_WHISPER, g_self, g_name, t.name, text);
        whisper_reply(t.name, text);
    } else if (channel == NET_CHAT_TEAM) {
        if (!me || !me->team) {
            system_line("You are not in a team.");
            return 0;
        }
        chat_add(NET_CHAT_TEAM, g_self, g_name, "", text);
    } else {
        chat_add(NET_CHAT_ALL, g_self, g_name, "", text);
    }
    return 1;
}

int net_teleport_request(uint32_t player) {
    NetPlayer *p = find(player);
    if (g_state != NET_ONLINE || !p || player == g_self) return 0;
    uint32_t req = g_next_req++;
    system_fmt("Teleport request sent to %s.", p->name);
    // the invented player accepts at once: the answer and the move are queued for the UI
    NetEvent *a = ev_add(NET_EV_TELEPORT_ANSWER);
    a->player = player, a->request = req, a->accepted = 1;
    copy(a->name, sizeof a->name, p->name);
    NetEvent *go = ev_add(NET_EV_TELEPORT_GO);
    go->player = player, go->request = req, go->accepted = 1;
    memcpy(go->pos, p->pos, sizeof go->pos);
    copy(go->name, sizeof go->name, p->name);
    return 1;
}

void net_teleport_answer(uint32_t request, int accept) {
    (void)request;
    system_line(accept ? "You accepted the teleport request." : "You refused the teleport request.");
}

int net_team_create(const char *name) {
    NetPlayer *me = find(g_self);
    if (g_state != NET_ONLINE || !me || me->team || g_nteam >= NET_MAX_TEAMS) return 0;
    NetTeam *t = &g_team[g_nteam++];
    t->id = g_next_team++, t->leader = g_self;
    copy(t->name, sizeof t->name, name && *name ? name : "My Team");
    me->team = t->id;
    system_fmt("Team \"%s\" created.", t->name);
    return 1;
}

int net_team_invite(uint32_t player) {
    NetPlayer *p = find(player);
    if (g_state != NET_ONLINE || !p) return 0;
    system_fmt("Invite sent to %s.", p->name);
    return 1;
}

void net_team_answer(uint32_t invite, int accept) {
    (void)invite;
    NetPlayer *me = find(g_self);
    if (!me) return;
    if (accept) {
        me->team = 1;   // joins Red Squad, the invented team
        system_line("You joined the team.");
    } else {
        system_line("You declined the team invite.");
    }
}

void net_team_leave(void) {
    NetPlayer *me = find(g_self);
    if (!me || !me->team) return;
    me->team = 0;
    system_line("You left the team.");
}

int net_host_action(NetHostAction action, uint32_t target, const char *text) {
    NetPlayer *me = find(g_self), *p = find(target);
    if (g_state != NET_ONLINE || !me || me->role == NET_ROLE_PLAYER) return 0;
    char t[NET_TEXT_MAX];
    switch (action) {
    case NET_HOST_SUMMON_ALL:
        system_line("Summoned everyone.");
        break;
    case NET_HOST_MESSAGE:
        system_line(text ? text : "");
        break;
    case NET_HOST_KICK:
    case NET_HOST_BAN: {
        if (!p || p == me) return 0;
        snprintf(t, sizeof t, action == NET_HOST_KICK ? "%s was kicked." : "%s was banned.", p->name);
        system_line(t);
        NetEvent *e = ev_add(NET_EV_PLAYER_LEFT);
        e->player = p->id, copy(e->name, sizeof e->name, p->name);
        remove_player(p);
        break;
    }
    case NET_HOST_MUTE:
        if (!p) return 0;
        p->muted = text && *text == '0' ? 0 : 1;
        snprintf(t, sizeof t, p->muted ? "%s was muted." : "%s was unmuted.", p->name);
        system_line(t);
        break;
    case NET_HOST_GRANT_MOD:
    case NET_HOST_REVOKE_MOD:
        if (!p || me->role != NET_ROLE_HOST) return 0;
        p->role = action == NET_HOST_GRANT_MOD ? NET_ROLE_MOD : NET_ROLE_PLAYER;
        snprintf(t, sizeof t, action == NET_HOST_GRANT_MOD ? "%s is now a moderator." : "%s is no longer a moderator.", p->name);
        system_line(t);
        break;
    }
    return 1;
}

int net_poll_events(NetEvent *out, int max) {
    int n = g_nev < max ? g_nev : max;
    if (n > 0) memcpy(out, g_ev, (size_t)n * sizeof *out);
    memmove(g_ev, g_ev + n, (size_t)(g_nev - n) * sizeof *g_ev);
    g_nev -= n;
    return n;
}
