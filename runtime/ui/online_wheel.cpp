// The wheel menu: a radial menu opened by holding the wheel key (default V) or the pad chord (default Minus + left
// stick click). Four top-level slices at most (Teleport, Chat, Team, Host); each opens one ring below it; a ring that
// needs a person lists the players as a chooser. The game gets no input while it is open (online_modal_set).
// Choosing: the stick or the mouse aims, the number keys aim at slice 1..n, releasing the hold key picks, A / Enter /
// a click picks at once, Esc / B / a right click goes back. A ring that was opened by releasing stays open
// until a slice is picked. See docs/online-ui.md.
#include <algorithm>
#include <float.h>
#include <functional>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#include "imgui.h"
#include "ui/online_ui.h"

extern "C" {
#include "cfg.h"
#include "input.h"
#include "input_map.h"
#include "platform/platform.h"
#include "ui/menu.h"
}

bool online_wheel_key_down();
void online_answer_ask(bool accept);

static double now_s() { return (double)plat_now_ns() / 1e9; }

enum Icon { IC_PIN, IC_CHAT, IC_TEAM, IC_CROWN, IC_PERSON, IC_PLUS, IC_LEAVE, IC_LIST, IC_MEGAPHONE, IC_KICK, IC_MUTE,
            IC_BAN, IC_UP, IC_DOWN, IC_SUMMON, IC_MORE, IC_ADDON };

// ---- icons, drawn with the draw list so no font or texture is needed. c: centre, r: half size.
static void icon_draw(ImDrawList *dl, int ic, ImVec2 c, float r, ImU32 col, const OnlineWheelSlice *addon) {
    auto P = [&](float x, float y) { return ImVec2(c.x + x * r, c.y + y * r); };
    float th = fmaxf(2.0f, r * 0.14f);
    switch (ic) {
    case IC_PIN:
        dl->AddCircleFilled(P(0, -0.25f), r * 0.55f, col, 28);
        dl->AddTriangleFilled(P(-0.48f, 0.0f), P(0.48f, 0.0f), P(0, 0.95f), col);
        dl->AddCircleFilled(P(0, -0.25f), r * 0.2f, IM_COL32(20, 30, 26, 255), 16);
        break;
    case IC_CHAT:
        dl->AddRectFilled(P(-0.95f, -0.7f), P(0.95f, 0.45f), col, r * 0.3f);
        dl->AddTriangleFilled(P(-0.5f, 0.35f), P(-0.1f, 0.35f), P(-0.6f, 0.9f), col);
        for (int i = -1; i <= 1; i++) dl->AddCircleFilled(P(i * 0.4f, -0.12f), r * 0.11f, IM_COL32(20, 30, 26, 255), 10);
        break;
    case IC_TEAM:
        for (int s = -1; s <= 1; s += 2) {
            dl->AddCircleFilled(P(s * 0.5f, -0.4f), r * 0.32f, col, 20);
            dl->AddRectFilled(P(s * 0.5f - 0.5f, 0.05f), P(s * 0.5f + 0.5f, 0.85f), col, r * 0.4f, ImDrawFlags_RoundCornersTop);
        }
        break;
    case IC_CROWN: {
        ImVec2 pts[] = {P(-0.95f, 0.65f), P(-0.95f, -0.5f), P(-0.45f, 0.05f), P(0, -0.8f), P(0.45f, 0.05f), P(0.95f, -0.5f),
                        P(0.95f, 0.65f)};
        dl->AddConcavePolyFilled(pts, 7, col);
        break;
    }
    case IC_PERSON:
        dl->AddCircleFilled(P(0, -0.45f), r * 0.38f, col, 24);
        dl->AddRectFilled(P(-0.7f, 0.0f), P(0.7f, 0.9f), col, r * 0.45f, ImDrawFlags_RoundCornersTop);
        break;
    case IC_PLUS:
        dl->AddRectFilled(P(-0.85f, -0.16f), P(0.85f, 0.16f), col, 3);
        dl->AddRectFilled(P(-0.16f, -0.85f), P(0.16f, 0.85f), col, 3);
        break;
    case IC_LEAVE:
        dl->AddRect(P(-0.9f, -0.8f), P(0.1f, 0.8f), col, 4, 0, th);
        dl->AddLine(P(-0.2f, 0), P(0.95f, 0), col, th);
        dl->AddTriangleFilled(P(0.95f, -0.4f), P(0.95f, 0.4f), P(1.35f, 0), col);
        break;
    case IC_LIST:
        for (int i = -1; i <= 1; i++) {
            dl->AddCircleFilled(P(-0.75f, i * 0.6f), r * 0.12f, col, 10);
            dl->AddRectFilled(P(-0.4f, i * 0.6f - 0.1f), P(0.95f, i * 0.6f + 0.1f), col, 2);
        }
        break;
    case IC_MEGAPHONE:
        dl->AddQuadFilled(P(-0.9f, -0.3f), P(0.0f, -0.3f), P(0.9f, -0.85f), P(0.9f, 0.85f), col);
        dl->AddTriangleFilled(P(-0.9f, -0.3f), P(0, -0.3f), P(0, 0.35f), col);
        dl->AddRectFilled(P(-0.9f, -0.3f), P(-0.2f, 0.35f), col, 3);
        dl->AddRectFilled(P(-0.7f, 0.3f), P(-0.35f, 0.9f), col, 3);
        break;
    case IC_KICK:
        dl->AddLine(P(-0.75f, -0.75f), P(0.75f, 0.75f), col, th * 1.6f);
        dl->AddLine(P(0.75f, -0.75f), P(-0.75f, 0.75f), col, th * 1.6f);
        break;
    case IC_MUTE:
        dl->AddRectFilled(P(-0.85f, -0.3f), P(-0.4f, 0.3f), col, 2);
        dl->AddTriangleFilled(P(-0.45f, -0.3f), P(-0.45f, 0.3f), P(0.15f, 0.85f), col);
        dl->AddTriangleFilled(P(-0.45f, -0.3f), P(0.15f, -0.85f), P(0.15f, 0.85f), col);
        dl->AddLine(P(0.4f, -0.5f), P(0.95f, 0.5f), col, th);
        dl->AddLine(P(0.95f, -0.5f), P(0.4f, 0.5f), col, th);
        break;
    case IC_BAN:
        dl->AddCircle(P(0, 0), r * 0.85f, col, 32, th * 1.4f);
        dl->AddLine(P(-0.6f, -0.6f), P(0.6f, 0.6f), col, th * 1.4f);
        break;
    case IC_UP:
        dl->AddTriangleFilled(P(0, -0.9f), P(-0.85f, 0.2f), P(0.85f, 0.2f), col);
        dl->AddRectFilled(P(-0.35f, 0.2f), P(0.35f, 0.85f), col, 2);
        break;
    case IC_DOWN:
        dl->AddTriangleFilled(P(0, 0.9f), P(-0.85f, -0.2f), P(0.85f, -0.2f), col);
        dl->AddRectFilled(P(-0.35f, -0.85f), P(0.35f, -0.2f), col, 2);
        break;
    case IC_SUMMON:
        dl->AddCircle(P(0, 0), r * 0.85f, col, 32, th);
        dl->AddCircleFilled(P(0, 0), r * 0.3f, col, 20);
        for (int i = 0; i < 4; i++) {
            float a = (float)i * 1.5708f + 0.7854f;
            dl->AddTriangleFilled(P(cosf(a) * 1.1f, sinf(a) * 1.1f), P(cosf(a + 0.35f) * 0.62f, sinf(a + 0.35f) * 0.62f),
                                  P(cosf(a - 0.35f) * 0.62f, sinf(a - 0.35f) * 0.62f), col);
        }
        break;
    case IC_MORE:
        for (int i = -1; i <= 1; i++) dl->AddCircleFilled(P(i * 0.6f, 0), r * 0.17f, col, 12);
        break;
    case IC_ADDON:
        if (addon && addon->draw_icon) addon->draw_icon(dl, c, r, col);
        break;
    }
}

// ---- add-on slices
static const OnlineWheelSlice *g_addons[2];
int online_wheel_register(const OnlineWheelSlice *s) {
    for (auto &a : g_addons)
        if (!a) return a = s, 1;
    return 0;
}

// ---- state
enum RingKind { R_TOP, R_TEAM, R_HOST, R_PICK };
enum Purpose { P_TELEPORT, P_INVITE, P_KICK, P_MUTE, P_BAN, P_GIVE, P_TAKE };
struct RingDesc { RingKind kind; Purpose purpose; int page; };
struct Slice {
    std::string label, detail;
    int icon = IC_PERSON;
    bool enabled = true, opens_ring = false;
    ImU32 tint = 0;
    std::function<void()> fn;
    const OnlineWheelSlice *addon = nullptr;
};

static struct {
    bool open, held_mode, armed = true, prev_hold, pad_opened, mouse_used, stick_used;
    std::vector<RingDesc> rings;
    int sel = -1;
    double t_open;
    uint64_t pad_prev;
} W;


bool online_wheel_open() { return W.open; }

void online_wheel_close() {
    if (!W.open) return;
    W.open = false;
    W.rings.clear();
    online_modal_set(MODAL_WHEEL, false);
}

// ---- pad chord setting: names joined by '+'
static uint64_t parse_chord(const char *s) {
    static const struct { const char *n; int bit; } names[] = {
        {"a", B_A}, {"b", B_B}, {"x", B_X}, {"y", B_Y}, {"lstick", B_STICK_L}, {"rstick", B_STICK_R}, {"l", B_L},
        {"r", B_R}, {"zl", B_ZL}, {"zr", B_ZR}, {"plus", B_PLUS}, {"start", B_PLUS}, {"minus", B_MINUS},
        {"back", B_MINUS}, {"left", B_LEFT}, {"up", B_UP}, {"right", B_RIGHT}, {"down", B_DOWN}};
    uint64_t m = 0;
    char buf[96];
    snprintf(buf, sizeof buf, "%s", s ? s : "");
    for (char *tok = strtok(buf, "+, "); tok; tok = strtok(nullptr, "+, "))
        for (auto &n : names)
            if (!strcmp(tok, n.n)) m |= 1ull << n.bit;
    return m;
}

void online_wheel_pad_chord_changed(void) {
    const char *v = cfg_get("TOTK_NET_PAD_WHEEL");
    uint64_t m = v && *v ? (strcmp(v, "none") ? parse_chord(v) : 0) : parse_chord("minus+lstick");
    input_wheel_chord(m);
}

// ---- rings
static NetPlayer g_pl[NET_MAX_PLAYERS];
static int g_npl;
static NetPlayer g_self;
static bool g_have_self;

static void refresh_world() {
    g_npl = net_players(g_pl, NET_MAX_PLAYERS);
    g_have_self = net_player_by_id(net_self_id(), &g_self);
}

static bool is_staff() { return g_have_self && g_self.role != NET_ROLE_PLAYER; }

struct Act { int kind; uint32_t id; char name[NET_NAME_MAX]; char text[24]; };
static Act g_act;

static void run_act(void *) {
    switch (g_act.kind) {
    case NET_HOST_SUMMON_ALL: case NET_HOST_KICK: case NET_HOST_BAN: case NET_HOST_MUTE: case NET_HOST_GRANT_MOD:
    case NET_HOST_REVOKE_MOD:
        if (!net_host_action((NetHostAction)g_act.kind, g_act.id, g_act.text))
            online_toast("The server refused that action.", 2);
        break;
    }
}

static void ask_host(NetHostAction a, const NetPlayer *p, const char *question, const char *yes, const char *text = "") {
    g_act = {};
    g_act.kind = a;
    if (p) g_act.id = p->id, snprintf(g_act.name, sizeof g_act.name, "%s", p->name);
    snprintf(g_act.text, sizeof g_act.text, "%s", text);
    char q[160];
    snprintf(q, sizeof q, question, p ? p->name : "");
    online_confirm(q, yes, run_act, nullptr);
}

static const char *title_of(const RingDesc &r) {
    switch (r.kind) {
    case R_TOP: return "Wheel menu";
    case R_TEAM: return "Team tools";
    case R_HOST: return "Host tools";
    case R_PICK:
        switch (r.purpose) {
        case P_TELEPORT: return "Teleport to...";
        case P_INVITE: return "Invite to your team";
        case P_KICK: return "Kick...";
        case P_MUTE: return "Mute or unmute...";
        case P_BAN: return "Ban...";
        case P_GIVE: return "Make moderator...";
        case P_TAKE: return "Take moderator...";
        }
    }
    return "";
}

static void push_ring(RingKind k, Purpose p = P_TELEPORT) {
    W.rings.push_back({k, p, 0});
    W.sel = -1;
}

static Slice mk(const char *label, int icon, const char *detail, std::function<void()> fn, bool enabled = true,
                bool ring = false) {
    Slice s;
    s.label = label, s.icon = icon, s.detail = detail, s.fn = std::move(fn), s.enabled = enabled, s.opens_ring = ring;
    return s;
}

static void pick_done(Purpose p, const NetPlayer pl) {
    char t[160];
    switch (p) {
    case P_TELEPORT:
        if (net_teleport_request(pl.id)) snprintf(t, sizeof t, "Teleport request sent to %s", pl.name), online_toast(t, 0);
        break;
    case P_INVITE:
        if (net_team_invite(pl.id)) snprintf(t, sizeof t, "Invite sent to %s", pl.name), online_toast(t, 0);
        break;
    case P_KICK: ask_host(NET_HOST_KICK, &pl, "Kick %s from the server?", "Kick"); break;
    case P_BAN: ask_host(NET_HOST_BAN, &pl, "Ban %s from the server? They cannot come back.", "Ban"); break;
    case P_MUTE:
        ask_host(NET_HOST_MUTE, &pl, pl.muted ? "Unmute %s?" : "Mute %s?", pl.muted ? "Unmute" : "Mute", pl.muted ? "0" : "1");
        break;
    case P_GIVE: ask_host(NET_HOST_GRANT_MOD, &pl, "Make %s a moderator?", "Make moderator"); break;
    case P_TAKE: ask_host(NET_HOST_REVOKE_MOD, &pl, "Take moderator from %s?", "Take moderator"); break;
    }
}

static std::vector<Slice> build(const RingDesc &r) {
    std::vector<Slice> v;
    switch (r.kind) {
    case R_TOP: {
        v.push_back(mk("Teleport", IC_PIN, "Go to another player", [] { push_ring(R_PICK, P_TELEPORT); }, g_npl > 1, true));
        v.push_back(mk("Chat", IC_CHAT, "Open the chat box", [] { online_chat_open(NET_CHAT_ALL, ""); }));
        v.push_back(mk("Team", IC_TEAM, "Create, invite, leave", [] { push_ring(R_TEAM); }, true, true));
        if (is_staff()) v.push_back(mk("Host", IC_CROWN, "Host and moderator tools", [] { push_ring(R_HOST); }, true, true));
        for (const OnlineWheelSlice *a : g_addons)
            if (a && (!a->visible || a->visible(a->user))) {
                Slice s = mk(a->label, IC_ADDON, a->detail ? a->detail : "", [a] { if (a->activate) a->activate(a->user); });
                s.addon = a;
                v.push_back(s);
            }
        break;
    }
    case R_TEAM: {
        bool in_team = g_have_self && g_self.team != 0;
        v.push_back(mk("Create", IC_PLUS, in_team ? "You are in a team already" : "Start a team", [] {
            char nm[NET_NAME_MAX + 8];
            snprintf(nm, sizeof nm, "%s's team", g_self.name);
            if (net_team_create(nm)) online_toast("Team created", 1);
        }, !in_team));
        v.push_back(mk("Invite", IC_PERSON, in_team ? "Ask a player to join" : "Create a team first",
                       [] { push_ring(R_PICK, P_INVITE); }, in_team, true));
        v.push_back(mk("Leave", IC_LEAVE, in_team ? "Leave your team" : "You are in no team", [] { net_team_leave(); }, in_team));
        v.push_back(mk("Players", IC_LIST, "Show the player list", [] { online_list_toggle(); }));
        v.push_back(mk("Team chat", IC_CHAT, in_team ? "Talk to your team" : "You are in no team",
                       [] { online_chat_open(NET_CHAT_TEAM, ""); }, in_team));
        break;
    }
    case R_HOST: {
        bool host = g_have_self && g_self.role == NET_ROLE_HOST;
        v.push_back(mk("Summon all", IC_SUMMON, "Bring everyone to you",
                       [] { ask_host(NET_HOST_SUMMON_ALL, nullptr, "Summon every player to your position?", "Summon"); }));
        v.push_back(mk("Kick", IC_KICK, "Remove a player", [] { push_ring(R_PICK, P_KICK); }, g_npl > 1, true));
        v.push_back(mk("Mute", IC_MUTE, "Silence a player", [] { push_ring(R_PICK, P_MUTE); }, g_npl > 1, true));
        v.push_back(mk("Ban", IC_BAN, "Remove for good", [] { push_ring(R_PICK, P_BAN); }, g_npl > 1, true));
        v.push_back(mk("Message", IC_MEGAPHONE, "Tell the whole server", [] { online_chat_server_message(); }));
        v.push_back(mk("Give mod", IC_UP, host ? "Make a player a moderator" : "Only the host can do this",
                       [] { push_ring(R_PICK, P_GIVE); }, host && g_npl > 1, true));
        v.push_back(mk("Take mod", IC_DOWN, host ? "Take moderator away" : "Only the host can do this",
                       [] { push_ring(R_PICK, P_TAKE); }, host && g_npl > 1, true));
        break;
    }
    case R_PICK: {
        std::vector<NetPlayer> cand;
        for (int i = 0; i < g_npl; i++) {
            const NetPlayer &p = g_pl[i];
            if (p.id == net_self_id()) continue;
            bool ok = true;
            switch (r.purpose) {
            case P_INVITE: ok = !g_have_self || !p.team || p.team != g_self.team; break;
            case P_KICK: case P_BAN: ok = p.role != NET_ROLE_HOST && (g_self.role == NET_ROLE_HOST || p.role == NET_ROLE_PLAYER); break;
            case P_GIVE: ok = p.role == NET_ROLE_PLAYER; break;
            case P_TAKE: ok = p.role == NET_ROLE_MOD; break;
            default: break;
            }
            if (ok) cand.push_back(p);
        }
        const int per = cand.size() > 8 ? 7 : 8;
        int pages = ((int)cand.size() + per - 1) / per;
        int page = pages ? r.page % pages : 0;
        for (int i = page * per; i < (int)cand.size() && i < (page + 1) * per; i++) {
            NetPlayer p = cand[(size_t)i];
            Purpose pu = r.purpose;
            Slice s = mk(p.name, IC_PERSON, p.area, [pu, p] { pick_done(pu, p); });
            ImU32 col;
            if (online_player_color(p, &col)) s.tint = col;
            v.push_back(s);
        }
        if (pages > 1) v.push_back(mk("More", IC_MORE, "Next players", [] { W.rings.back().page++; W.sel = -1; }, true, true));
        if (v.empty()) v.push_back(mk("Nobody", IC_PERSON, "Nobody fits here", [] {}, false));
        break;
    }
    }
    return v;
}

// ---- the frame
static int sector_of(float ang_from_up, int n) {
    float step = 6.2831853f / (float)n, a = ang_from_up + step * 0.5f;
    a = fmodf(a + 6.2831853f * 4, 6.2831853f);
    int i = (int)(a / step);
    return i >= n ? n - 1 : i;
}

static void activate(const std::vector<Slice> &v, int i) {
    if (i < 0 || i >= (int)v.size()) return;
    const Slice &s = v[(size_t)i];
    if (!s.enabled) return;
    auto fn = s.fn;
    if (s.opens_ring) {
        fn();
        return;
    }
    online_wheel_close();   // first, so the action may open the chat box or a confirmation
    if (fn) fn();
}

static bool allowed_to_open() {
    return net_state() == NET_ONLINE && !menu_is_open() && !(online_modal() & ~MODAL_WHEEL);
}

bool online_wheel_pump(float w, float h) {
    (void)w, (void)h;
    return W.open;
}

void online_wheel_layout(float w, float h, float scale) {
    refresh_world();
    bool hold = online_wheel_key_down() || input_wheel_held();
    bool rising = hold && !W.prev_hold;
    bool released = !hold && W.prev_hold;
    W.prev_hold = hold;
    if (!hold) W.armed = true;
    if (!W.open) {
        if (rising && W.armed && allowed_to_open()) {
            W.open = true, W.held_mode = true, W.sel = -1, W.t_open = now_s();
            W.pad_opened = !online_wheel_key_down();
            W.mouse_used = W.stick_used = false;
            W.rings.assign(1, {R_TOP, P_TELEPORT, 0});
            online_modal_set(MODAL_WHEEL, true);
        }
        return;
    }
    ImGuiIO &io = ImGui::GetIO();
    std::vector<Slice> sl = build(W.rings.back());
    int n = (int)sl.size();
    ImVec2 c(w / 2, h / 2);
    float wscale = 1.0f;
    if (const char *e = cfg_get("TOTK_NET_WHEEL_SCALE")) wscale = fmaxf(0.6f, fminf(1.5f, (float)atof(e) / 100.0f));
    double age = now_s() - W.t_open;
    float anim = (float)fminf(1.0, age / 0.14), ease = 1 - (1 - anim) * (1 - anim);
    float R = fminf(w, h) * 0.30f * wscale * (0.88f + 0.12f * ease), r0 = R * 0.38f;

    // aim: stick, then mouse, then number keys
    uint64_t b = 0;
    int32_t lx = 0, ly = 0;
    bool have_pad = input_pad_read(&b, &lx, &ly) != 0;
    float sx = lx / 32767.0f, sy = ly / 32767.0f;
    if (have_pad && hypotf(sx, sy) > 0.55f) {
        W.sel = sector_of(atan2f(sx, sy), n);   // 0 is up, clockwise
        W.stick_used = true, W.mouse_used = false;
    }
    if (io.MouseDelta.x != 0 || io.MouseDelta.y != 0) {
        float dx = io.MousePos.x - c.x, dy = io.MousePos.y - c.y;
        if (hypotf(dx, dy) > r0 * 0.7f) W.sel = sector_of(atan2f(dx, -dy), n), W.mouse_used = true, W.stick_used = false;
    }
    bool pick_now = !W.held_mode;
    for (int i = 0; i < n && i < 9; i++)
        if (ImGui::IsKeyPressed((ImGuiKey)(ImGuiKey_1 + i), false)) {
            W.sel = i;
            if (!W.held_mode) pick_now = true, activate(sl, i);
            W.stick_used = W.mouse_used = false;
        }
    if (W.sel >= n) W.sel = -1;
    bool confirm_key = ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false) || ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                       ImGui::IsKeyPressed(ImGuiKey_Space, false) || ImGui::IsMouseClicked(0);
    bool back = ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) ||
                ImGui::IsMouseClicked(1);
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false)) online_answer_ask(true);    // X
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft, false)) online_answer_ask(false);  // Y
    (void)pick_now;
    if (back) {
        if (W.rings.size() > 1) W.rings.pop_back(), W.sel = -1;
        else online_wheel_close();
        if (!W.open) W.armed = !hold;
        if (!W.open) return;
    } else if (released && W.held_mode) {
        // letting go of the hold key picks the aimed slice; a ring it opens stays until a slice is chosen
        W.held_mode = false;
        if (W.sel < 0) return online_wheel_close();
        activate(sl, W.sel);
        if (!W.open) return;
    } else if (confirm_key && W.sel >= 0) {
        activate(sl, W.sel);
        if (!W.open) {
            W.armed = !hold;
            return;
        }
    }
    if (!W.open) return;

    // ---- draw
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(w, h), IM_COL32(0, 0, 0, (int)(130 * ease)));
    const float TAU = 6.2831853f, gap = 0.035f;
    float step = TAU / (float)n;
    ImFont *font = look_font();
    float fs = 24 * scale;
    const ImU32 cream = IM_COL32(246, 241, 224, 255), gold = IM_COL32(255, 226, 140, 255);
    for (int i = 0; i < n; i++) {
        float mid = -TAU / 4 + step * (float)i;   // up, clockwise
        float a0 = mid - step / 2 + gap / (n > 1 ? 1.0f : 100.0f), a1 = mid + step / 2 - gap / (n > 1 ? 1.0f : 100.0f);
        bool sel = i == W.sel, en = sl[(size_t)i].enabled;
        ImVec2 dir(cosf(mid), sinf(mid)), off = sel ? ImVec2(dir.x * 10 * scale, dir.y * 10 * scale) : ImVec2(0, 0);
        ImVec2 ci(c.x + off.x, c.y + off.y);
        // dark translucent slices like the game's own panels; the aimed one is lit warm cream with a bright edge
        ImU32 fill = !en ? IM_COL32(14, 16, 16, 175) : sel ? IM_COL32(88, 82, 58, 215) : IM_COL32(9, 11, 11, 185);
        dl->PathArcTo(ci, R, a0, a1, 28);
        dl->PathArcTo(ci, r0, a1, a0, 20);
        dl->PathFillConcave(fill);
        dl->PathArcTo(ci, R, a0, a1, 28);
        dl->PathArcTo(ci, r0, a1, a0, 20);
        dl->PathStroke(sel ? IM_COL32(255, 240, 180, 255) : IM_COL32(200, 194, 168, 110), ImDrawFlags_Closed, sel ? 3.0f * scale : 1.5f * scale);
        float rm = (r0 + R) * 0.5f;
        ImVec2 mc(ci.x + dir.x * rm, ci.y + dir.y * rm);
        float ir = R * 0.14f;
        ImU32 ic = !en ? IM_COL32(120, 120, 110, 255) : sl[(size_t)i].tint ? sl[(size_t)i].tint : sel ? IM_COL32(255, 250, 225, 255) : IM_COL32(232, 228, 210, 255);
        icon_draw(dl, sl[(size_t)i].icon, ImVec2(mc.x, mc.y - fs * 0.55f), ir, ic, sl[(size_t)i].addon);
        float lfs = fs;
        std::string lab = sl[(size_t)i].label;
        float maxw = R * 0.62f * fminf(1.0f, 4.2f / (float)(n > 4 ? n - 1 : 3));
        while (lab.size() > 3 && look_width(lfs, lab.c_str()) > maxw) lab.resize(lab.size() - 2), lab += ".";
        float lw = look_width(lfs, lab.c_str());
        look_text(dl, lfs, ImVec2(mc.x - lw / 2, mc.y + ir * 0.9f), en ? cream : IM_COL32(135, 135, 125, 255), lab.c_str());
        if (n <= 9) {   // the number key of the slice, in a small square like the game's button prompts
            char nb[4];
            snprintf(nb, sizeof nb, "%d", i + 1);
            ImVec2 nc(ci.x + dir.x * (R - 20 * scale), ci.y + dir.y * (R - 20 * scale));
            look_square(dl, ImVec2(nc.x - 12 * scale, nc.y - 12 * scale), 24 * scale, IM_COL32(20, 22, 22, 220));
            float nw = look_width(fs * 0.8f, nb);
            look_text(dl, fs * 0.8f, ImVec2(nc.x - nw / 2, nc.y - 11 * scale), gold, nb);
        }
    }
    // centre: the ring's title and the aimed slice's detail
    dl->AddCircleFilled(c, r0 - 8 * scale, IM_COL32(9, 11, 11, 215), 48);
    dl->AddCircle(c, r0 - 8 * scale, IM_COL32(200, 194, 168, 130), 48, 1.5f * scale);
    const char *title = title_of(W.rings.back());
    float tfs = fs * 1.1f;
    float cw = (r0 - 18 * scale) * 2;
    ImVec2 tz = font->CalcTextSizeA(tfs, FLT_MAX, cw, title);
    look_text(dl, tfs, ImVec2(c.x - tz.x / 2, c.y - tz.y - 4 * scale), gold, title);
    const char *det = W.sel >= 0 ? sl[(size_t)W.sel].detail.c_str() : "Aim at a slice";
    float dfs = fs * 0.78f;
    const char *s = det, *dend = det + strlen(det);
    float dy = c.y + 6 * scale;
    for (int line = 0; s < dend && line < 3; line++) {   // wrapped to the centre disc
        const char *brk = font->CalcWordWrapPositionA(dfs / font->FontSize, s, dend, cw);
        if (brk <= s) brk = s + 1;
        float lw = look_width(dfs, s, brk);
        look_text(dl, dfs, ImVec2(c.x - lw / 2, dy), IM_COL32(214, 210, 192, 255), s, brk);
        dy += dfs * 1.2f;
        s = brk;
        while (s < dend && *s == ' ') s++;
    }

    char hint[200];
    bool padhint = W.pad_opened || W.stick_used;
    if (W.held_mode)
        snprintf(hint, sizeof hint, padhint ? "Aim with the left stick, let go to choose. B to cancel." :
                                              "Move the mouse or press 1-%d, let go of the key to choose. Esc to cancel.", n);
    else
        snprintf(hint, sizeof hint, padhint ? "Aim with the left stick and press A. B to go back." :
                                              "Click or press 1-%d to choose. Esc or right click to go back.", n);
    float hfs = 18 * scale, hz = look_width(hfs, hint);
    float hy = c.y + R + 22 * scale;
    look_bar(dl, ImVec2(c.x - hz / 2 - 40 * scale, hy), ImVec2(c.x + hz / 2 + 40 * scale, hy + hfs + 14 * scale), 150);
    look_text(dl, hfs, ImVec2(c.x - hz / 2, hy + 6 * scale), LOOK_DIM, hint);
}

// ---- scripted screenshots (TOTK_NET_DEMO_DIR, ui/online_ui.cpp online_demo): puts the wheel into a given state
void online_wheel_demo(int what) {
    if (what == 0) return online_wheel_close();
    refresh_world();
    if (!W.open) {
        W.open = true, W.held_mode = what == 1, W.pad_opened = false, W.t_open = now_s() - 1;
        online_modal_set(MODAL_WHEEL, true);
    }
    W.rings.assign(1, {R_TOP, P_TELEPORT, 0});
    W.sel = 0;
    switch (what) {
    case 2: push_ring(R_PICK, P_TELEPORT), W.sel = 1; break;
    case 3: push_ring(R_TEAM), W.sel = 0; break;
    case 4: push_ring(R_HOST), W.sel = 2; break;
    case 5: {
        for (int i = 0; i < g_npl; i++)
            if (g_pl[i].id == 3) pick_done(P_KICK, g_pl[i]);   // opens the kick confirmation
        online_wheel_close();
        break;
    }
    default: W.sel = 2; break;
    }
}
