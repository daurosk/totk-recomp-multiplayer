// The online UI: state shared by its parts, the hotkeys, the net events, the chat box, the player list, toasts and the
// confirmation dialog. The wheel is ui/online_wheel.cpp, the F1 tab ui/online_tab.cpp. See ui/online_ui.h.
#include <algorithm>
#include <atomic>
#include <ctype.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#include "imgui.h"
#include "ui/online_ui.h"

extern "C" {
#include "cfg.h"
#include "host.h"
#include "input.h"
#include "input_map.h"
#include "platform/platform.h"
#include "teleport.h"
#include "puppet_self.h"
#include "vk_camera.h"
#include "ui/menu.h"
}

static double now_s() { return (double)plat_now_ns() / 1e9; }

// ---- keys by name (the settings hold names; the window adapters deliver ImGuiKeys)
struct KeyName { ImGuiKey key; const char *name; };
static const KeyName kKeys[] = {
    {ImGuiKey_Enter, "enter"}, {ImGuiKey_Space, "space"}, {ImGuiKey_GraveAccent, "grave"}, {ImGuiKey_Slash, "slash"},
    {ImGuiKey_Backslash, "backslash"}, {ImGuiKey_LeftBracket, "lbracket"}, {ImGuiKey_RightBracket, "rbracket"},
    {ImGuiKey_Semicolon, "semicolon"}, {ImGuiKey_Apostrophe, "apostrophe"}, {ImGuiKey_Comma, "comma"},
    {ImGuiKey_Period, "period"}, {ImGuiKey_Minus, "minus"}, {ImGuiKey_Equal, "equals"}, {ImGuiKey_Insert, "insert"},
    {ImGuiKey_Home, "home"}, {ImGuiKey_End, "end"}, {ImGuiKey_PageUp, "pageup"}, {ImGuiKey_PageDown, "pagedown"},
    {ImGuiKey_LeftAlt, "lalt"}, {ImGuiKey_CapsLock, "capslock"},
};

const char *online_key_name(int key) {
    static char buf[4][8];
    static int next;
    char *b = buf[next++ & 3];
    if (key >= ImGuiKey_A && key <= ImGuiKey_Z) return snprintf(b, 8, "%c", 'a' + key - ImGuiKey_A), b;
    if (key >= ImGuiKey_0 && key <= ImGuiKey_9) return snprintf(b, 8, "%c", '0' + key - ImGuiKey_0), b;
    if (key >= ImGuiKey_F1 && key <= ImGuiKey_F12) return snprintf(b, 8, "f%d", key - ImGuiKey_F1 + 1), b;
    for (const KeyName &k : kKeys)
        if (k.key == key) return k.name;
    return "none";
}

int online_key_from_name(const char *name) {
    if (!name || !*name) return ImGuiKey_None;
    char low[16];
    size_t n = 0;
    for (; name[n] && n < sizeof low - 1; n++) low[n] = (char)tolower((unsigned char)name[n]);
    low[n] = 0;
    for (int k = ImGuiKey_None + 1; k < ImGuiKey_NamedKey_END; k++)
        if (!strcmp(online_key_name(k), low)) return k;
    return ImGuiKey_None;
}

int online_cfg_key(const char *setting, int fallback) {
    const char *v = cfg_get(setting);
    if (!v || !*v) return fallback;
    if (!strcmp(v, "none")) return ImGuiKey_None;
    int k = online_key_from_name(v);
    return k ? k : fallback;
}

// ---- shared state
static std::atomic<int> g_bits;                 // MODAL_*
static std::atomic<int> k_chat{ImGuiKey_Enter}, k_wheel{ImGuiKey_V}, k_list{ImGuiKey_P}, k_yes{ImGuiKey_Y}, k_no{ImGuiKey_N};
static std::atomic<unsigned> k_ver{~0u};
static std::atomic<int> g_wheel_key_down, g_req_chat, g_req_list, g_req_yes, g_req_no, g_req_hop, g_pending_asks;

extern "C" int online_modal(void) { return g_bits.load(std::memory_order_relaxed); }

void online_modal_set(int bit, bool on) {
    int old = on ? g_bits.fetch_or(bit) : g_bits.fetch_and(~bit);
    if (((old & bit) != 0) != on) menu_input_refresh();
}

static void keys_load() {
    unsigned v = cfg_version();
    if (k_ver.exchange(v) == v) return;
    k_chat = online_cfg_key("TOTK_NET_KEY_CHAT", ImGuiKey_Enter);
    k_wheel = online_cfg_key("TOTK_NET_KEY_WHEEL", ImGuiKey_V);
    k_list = online_cfg_key("TOTK_NET_KEY_LIST", ImGuiKey_P);
    k_yes = online_cfg_key("TOTK_NET_KEY_ACCEPT", ImGuiKey_Y);
    k_no = online_cfg_key("TOTK_NET_KEY_REFUSE", ImGuiKey_N);
    online_wheel_pad_chord_changed();
}

static bool fake_net() {
    const char *e = cfg_get("TOTK_NET_FAKE");
    return e && !strcmp(e, "1");
}

extern "C" int online_wants_frame(void) { return net_enabled(); }

extern "C" int online_hotkey(int key, int down) {
    if (!net_enabled() || key == ImGuiKey_None) return 0;
    keys_load();
    int bits = g_bits.load();
    if (key == k_wheel.load()) {
        if (!down) {
            g_wheel_key_down = 0;
            return 0;
        }
        if (menu_is_open() || (bits & (MODAL_CHAT | MODAL_CONFIRM)) || net_state() != NET_ONLINE) return 0;
        g_wheel_key_down = 1;
        return 1;
    }
    if (!down || menu_is_open() || (bits & MODAL_CHAT)) return 0;
    if (key == k_chat.load() && net_state() == NET_ONLINE && !(bits & (MODAL_WHEEL | MODAL_CONFIRM))) {
        g_req_chat = 1;
        online_modal_set(MODAL_CHAT, true);   // at once, so the characters that follow reach the box
        return 1;
    }
    if (key == k_list.load() && net_state() == NET_ONLINE) return g_req_list = 1, 1;
    if (g_pending_asks.load() > 0) {
        if (key == k_yes.load()) return g_req_yes = 1, 1;
        if (key == k_no.load()) return g_req_no = 1, 1;
    }
    if (fake_net() && (key == ImGuiKey_F6 || key == ImGuiKey_F7)) return g_req_hop = key == ImGuiKey_F7 ? 1 : -1, 1;
    return 0;
}

extern "C" void online_focus_lost(void) { g_wheel_key_down = 0; }

bool online_wheel_key_down() { return g_wheel_key_down.load() != 0; }

void online_answer_ask(bool accept) {
    if (g_pending_asks.load() > 0) (accept ? g_req_yes : g_req_no) = 1;
}

// ---- toasts
struct Toast { char text[200]; int kind, what; uint32_t req; double born, ttl; };
static std::vector<Toast> g_toasts;

void online_toast(const char *text, int kind) {
    Toast t = {};
    snprintf(t.text, sizeof t.text, "%s", text);
    t.kind = kind, t.born = now_s(), t.ttl = 5.0;
    g_toasts.push_back(t);
    if (g_toasts.size() > 8) g_toasts.erase(g_toasts.begin());
}

void online_toast_ask(const char *text, int what, uint32_t request) {
    Toast t = {};
    snprintf(t.text, sizeof t.text, "%s", text);
    t.kind = 2, t.what = what, t.req = request, t.born = now_s(), t.ttl = 30.0;
    g_toasts.push_back(t);
    g_pending_asks = 0;
    for (const Toast &x : g_toasts) g_pending_asks += x.what != 0;
}

static void toast_answer(size_t i, bool accept) {
    Toast t = g_toasts[i];
    g_toasts.erase(g_toasts.begin() + (long)i);
    if (t.what == 1) net_teleport_answer(t.req, accept);
    else if (t.what == 2) net_team_answer(t.req, accept);
    g_pending_asks = 0;
    for (const Toast &x : g_toasts) g_pending_asks += x.what != 0;
}

static void toasts_expire(double now) {
    for (size_t i = 0; i < g_toasts.size();) {
        if (now - g_toasts[i].born < g_toasts[i].ttl) {
            i++;
        } else if (g_toasts[i].what) {
            toast_answer(i, false);   // an unanswered request counts as a refusal
        } else {
            g_toasts.erase(g_toasts.begin() + (long)i);
        }
    }
}

void online_toasts_layout(float w, float h, float scale) {
    (void)h;
    double now = now_s();
    float y = 150 * scale, tw = 380 * scale;
    const ImVec4 tint[3] = {ImVec4(0.30f, 0.45f, 0.62f, 1), ImVec4(0.25f, 0.62f, 0.40f, 1), ImVec4(0.85f, 0.65f, 0.25f, 1)};
    for (size_t i = 0; i < g_toasts.size(); i++) {
        const Toast &t = g_toasts[i];
        double age = now - t.born, left = t.ttl - age;
        float a = (float)std::min({1.0, age / 0.2, left / 0.4});
        if (a <= 0) continue;
        char id[24];
        snprintf(id, sizeof id, "##toast%zu", i);
        ImGui::SetNextWindowPos(ImVec2(w - 24 * scale - tw, y));
        ImGui::SetNextWindowSize(ImVec2(tw, 0));
        ImGui::SetNextWindowBgAlpha(0.88f * a);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, a);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16 * scale, 12 * scale));
        ImGui::PushStyleColor(ImGuiCol_Border, tint[t.kind % 3]);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
        bool ask = t.what != 0;
        ImGui::Begin(id, nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | (ask ? 0 : ImGuiWindowFlags_NoInputs));
        ImGui::TextWrapped("%s", t.text);
        if (ask) {
            ImGui::ProgressBar((float)(left / t.ttl), ImVec2(-1, 6 * scale), "");
            float bw = (tw - 32 * scale - ImGui::GetStyle().ItemSpacing.x) / 2;
            char yes[48], no[48];
            snprintf(yes, sizeof yes, "Accept (%s)##y%zu", online_key_name(k_yes.load()), i);
            snprintf(no, sizeof no, "Refuse (%s)##n%zu", online_key_name(k_no.load()), i);
            if (ImGui::Button(yes, ImVec2(bw, 0))) g_req_yes = 1;
            ImGui::SameLine();
            if (ImGui::Button(no, ImVec2(bw, 0))) g_req_no = 1;
        }
        y += ImGui::GetWindowHeight() + 10 * scale;
        ImGui::End();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor();
    }
}

// ---- confirmation
static struct { bool on; char q[200], yes[40]; void (*fn)(void *); void *user; bool first; } g_conf;

void online_confirm(const char *question, const char *yes_label, void (*yes)(void *), void *user) {
    snprintf(g_conf.q, sizeof g_conf.q, "%s", question);
    snprintf(g_conf.yes, sizeof g_conf.yes, "%s", yes_label);
    g_conf.fn = yes, g_conf.user = user, g_conf.on = true, g_conf.first = true;
    online_modal_set(MODAL_CONFIRM, true);
}

static void confirm_close() {
    g_conf.on = false;
    online_modal_set(MODAL_CONFIRM, false);
}

void online_confirm_layout(float w, float h, float scale) {
    if (!g_conf.on) return;
    ImGui::SetNextWindowPos(ImVec2(w / 2, h / 2), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(520 * scale, 0));
    ImGui::SetNextWindowFocus();
    ImGui::Begin("##online_confirm", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextWrapped("%s", g_conf.q);
    ImGui::Spacing();
    float bw = (520 * scale - 56 * scale - ImGui::GetStyle().ItemSpacing.x) / 2;
    bool yes = false, no = false;
    if (ImGui::Button(g_conf.yes, ImVec2(bw, 0))) yes = true;
    ImGui::SameLine();
    if (g_conf.first) ImGui::SetKeyboardFocusHere();   // Cancel starts focused: an accidental A or Enter does nothing
    if (ImGui::Button("Cancel", ImVec2(bw, 0))) no = true;
    g_conf.first = false;
    ImGui::TextDisabled("Left and right to switch, Enter or A to choose, Esc or B to cancel");
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) no = true;
    ImGui::End();
    if (yes) {
        void (*fn)(void *) = g_conf.fn;
        void *u = g_conf.user;
        confirm_close();
        if (fn) fn(u);
    } else if (no) {
        confirm_close();
    }
}

// ---- chat
static struct {
    bool typing, prefilled, follow;
    char buf[NET_TEXT_MAX];
    NetChannel channel;
    char whisper_to[NET_NAME_MAX], reply_to[NET_NAME_MAX], prompt[48];
    double last_activity;
    uint32_t serial_seen;
    bool server_msg;
} g_chat;

void online_chat_open(NetChannel channel, const char *prefill, const char *prompt) {
    g_chat.typing = true, g_chat.prefilled = false, g_chat.follow = true;
    g_chat.channel = channel == NET_CHAT_WHISPER && !g_chat.whisper_to[0] ? NET_CHAT_ALL : channel;
    snprintf(g_chat.buf, sizeof g_chat.buf, "%s", prefill ? prefill : "");
    snprintf(g_chat.prompt, sizeof g_chat.prompt, "%s", prompt ? prompt : "");
    g_chat.server_msg = false;
    online_modal_set(MODAL_CHAT, true);
}

void online_chat_server_message(void) {
    online_chat_open(NET_CHAT_ALL, "", "Server message");
    g_chat.server_msg = true;
}

static void chat_close() {
    g_chat.typing = false;
    g_chat.buf[0] = 0;
    g_chat.last_activity = now_s();
    online_modal_set(MODAL_CHAT, false);
}

static void trim(char *s) {
    size_t n = strlen(s), a = 0;
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
    while (s[a] && isspace((unsigned char)s[a])) a++;
    if (a) memmove(s, s + a, n - a + 1);
}

static void chat_submit() {
    trim(g_chat.buf);
    char *t = g_chat.buf;
    if (!*t) return chat_close();
    if (g_chat.server_msg) {
        if (!net_host_action(NET_HOST_MESSAGE, 0, t)) online_toast("Only the host and moderators can send server messages.", 2);
        return chat_close();
    }
    NetChannel ch = g_chat.channel;
    char who[NET_NAME_MAX] = "";
    if (*t == '/') {
        char cmd[16] = "";
        int used = 0;
        sscanf(t + 1, "%15s%n", cmd, &used);
        const char *rest = t + 1 + used;
        while (*rest == ' ') rest++;
        if (!strcmp(cmd, "w") || !strcmp(cmd, "whisper") || !strcmp(cmd, "msg")) {
            int u2 = 0;
            sscanf(rest, "%31s%n", who, &u2);
            rest += u2;
            while (*rest == ' ') rest++;
            if (!*who || !*rest) {
                online_toast("Whisper: /w name message", 2);
                return;
            }
            ch = NET_CHAT_WHISPER;
        } else if (!strcmp(cmd, "r") || !strcmp(cmd, "reply")) {
            if (!g_chat.reply_to[0]) {
                online_toast("Nobody to reply to yet.", 2);
                return;
            }
            snprintf(who, sizeof who, "%s", g_chat.reply_to);
            ch = NET_CHAT_WHISPER;
        } else if (!strcmp(cmd, "t") || !strcmp(cmd, "team")) {
            ch = NET_CHAT_TEAM;
        } else if (!strcmp(cmd, "a") || !strcmp(cmd, "all") || !strcmp(cmd, "say")) {
            ch = NET_CHAT_ALL;
        } else if (!strcmp(cmd, "me") || !strcmp(cmd, "who") || !strcmp(cmd, "teams") || !strcmp(cmd, "motd") ||
                   !strcmp(cmd, "help")) {
            // Sent whole on the all channel: net_chat_send turns "/me text" into an action line, and the server
            // answers its own commands (/who, /teams, /motd, /help) with a system line to this player alone.
            net_chat_send(NET_CHAT_ALL, nullptr, t);
            return chat_close();
        } else {
            online_toast("Commands: /w name msg, /r msg, /t msg, /a msg", 2);
            return;
        }
        memmove(g_chat.buf, rest, strlen(rest) + 1);
        t = g_chat.buf;
    } else if (ch == NET_CHAT_WHISPER) {
        snprintf(who, sizeof who, "%s", g_chat.whisper_to);
    }
    if (*t) {
        if (ch == NET_CHAT_WHISPER) snprintf(g_chat.whisper_to, sizeof g_chat.whisper_to, "%s", who);
        net_chat_send(ch, ch == NET_CHAT_WHISPER ? who : nullptr, t);
    }
    chat_close();
}

static int chat_cb(ImGuiInputTextCallbackData *d) {   // Tab: the next channel
    if (d->EventFlag == ImGuiInputTextFlags_CallbackCompletion) {
        int next = ((int)g_chat.channel + 1) % 3;
        if (next == NET_CHAT_WHISPER && !g_chat.whisper_to[0]) next = NET_CHAT_ALL;
        g_chat.channel = (NetChannel)next;
    }
    return 0;
}

static ImVec4 chat_color(NetChannel c) {
    switch (c) {
    case NET_CHAT_TEAM: return ImVec4(0.45f, 0.88f, 0.55f, 1);
    case NET_CHAT_WHISPER: return ImVec4(0.86f, 0.62f, 0.96f, 1);
    case NET_CHAT_SYSTEM: return ImVec4(0.96f, 0.80f, 0.35f, 1);
    default: return ImVec4(0.96f, 0.94f, 0.88f, 1);
    }
}

static void chat_line_text(const NetChatLine &l, char *out, size_t n) {
    NetPlayer self;
    bool mine = net_player_by_id(net_self_id(), &self) && l.from == self.id;
    switch (l.channel) {
    case NET_CHAT_SYSTEM: snprintf(out, n, "* %s", l.text); break;
    case NET_CHAT_TEAM: snprintf(out, n, "[Team] %s: %s", l.from_name, l.text); break;
    case NET_CHAT_WHISPER:
        if (mine) snprintf(out, n, "[To %s] %s", l.to_name, l.text);
        else snprintf(out, n, "[From %s] %s", l.from_name, l.text);
        break;
    default: snprintf(out, n, "%s: %s", l.from_name, l.text);
    }
}

void online_chat_layout(float w, float h, float scale) {
    double now = now_s();
    uint32_t serial = net_chat_serial();
    if (serial != g_chat.serial_seen) {
        g_chat.serial_seen = serial;
        g_chat.last_activity = now;
        g_chat.follow = true;
        int n = net_chat_count();   // remember who whispered last, for /r
        NetChatLine l;
        if (n && net_chat_get(n - 1, &l) && l.channel == NET_CHAT_WHISPER && l.from != net_self_id())
            snprintf(g_chat.reply_to, sizeof g_chat.reply_to, "%s", l.from_name);
    }
    float fade_s = (float)atof(cfg_get("TOTK_NET_CHAT_FADE") ? cfg_get("TOTK_NET_CHAT_FADE") : "10");
    if (fade_s < 2) fade_s = 10;
    float alpha = g_chat.typing ? 1.0f : (float)std::min(1.0, std::max(0.0, (g_chat.last_activity + fade_s + 1.5 - now) / 1.5));
    if (alpha <= 0.01f || net_state() != NET_ONLINE) {
        if (g_chat.typing && net_state() != NET_ONLINE) chat_close();
        return;
    }
    int lines = 10;
    const char *cl = cfg_get("TOTK_NET_CHAT_LINES");
    if (cl && atoi(cl) >= 4 && atoi(cl) <= 30) lines = atoi(cl);
    float cw = fminf(w * 0.40f, 760 * scale), lh = ImGui::GetTextLineHeightWithSpacing();
    const ImGuiStyle &st = ImGui::GetStyle();
    float log_h = lines * lh, input_h = g_chat.typing ? ImGui::GetFrameHeight() + st.ItemSpacing.y + 24 * scale : 0;
    float total = log_h + input_h + st.WindowPadding.y * 2;
    ImGui::SetNextWindowPos(ImVec2(22 * scale, h - 120 * scale - total));
    ImGui::SetNextWindowSize(ImVec2(cw, total));
    ImGui::SetNextWindowBgAlpha(g_chat.typing ? 0.72f : 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12 * scale, 10 * scale));
    ImGuiWindowFlags f = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoScrollWithMouse;
    if (!g_chat.typing) f |= ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing;
    else ImGui::SetNextWindowFocus();
    ImGui::Begin("##online_chat", nullptr, f);
    ImGui::BeginChild("##chatlog", ImVec2(0, log_h), ImGuiChildFlags_None,
                      g_chat.typing ? ImGuiWindowFlags_None : (ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoInputs));
    int count = net_chat_count();
    for (int i = 0; i < count; i++) {
        NetChatLine l;
        if (!net_chat_get(i, &l)) continue;
        char text[NET_TEXT_MAX + 96];
        chat_line_text(l, text, sizeof text);
        ImGui::PushStyleColor(ImGuiCol_Text, chat_color(l.channel));
        ImGui::TextWrapped("%s", text);
        ImGui::PopStyleColor();
    }
    if (!g_chat.typing || g_chat.follow) ImGui::SetScrollHereY(1.0f);
    else if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 2) g_chat.follow = true;
    if (g_chat.typing && ImGui::GetIO().MouseWheel != 0) g_chat.follow = false;
    ImGui::EndChild();
    if (g_chat.typing) {
        const char *chname = g_chat.server_msg ? "Server message" : g_chat.channel == NET_CHAT_TEAM ? "Team" :
                             g_chat.channel == NET_CHAT_WHISPER ? g_chat.whisper_to : "All";
        char label[64];
        snprintf(label, sizeof label, "%s", chname);
        ImVec4 c = chat_color(g_chat.server_msg ? NET_CHAT_SYSTEM : g_chat.channel);
        ImGui::PushStyleColor(ImGuiCol_Text, c);
        ImGui::AlignTextToFramePadding();
        ImGui::Text("[%s]", label);
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1);
        if (!g_chat.prefilled) {
            ImGui::SetKeyboardFocusHere();
            g_chat.prefilled = true;
        }
        if (ImGui::InputText("##msg", g_chat.buf, sizeof g_chat.buf,
                             ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackCompletion, chat_cb))
            chat_submit();
        ImGui::TextDisabled("Enter sends, Tab changes channel, Esc closes. /w name message whispers.");
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) chat_close();
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

// ---- player list
static bool g_list_on;
void online_list_toggle(void) { g_list_on = !g_list_on; }

bool online_player_color(const NetPlayer &p, ImU32 *col) {
    *col = p.role == NET_ROLE_HOST ? IM_COL32(240, 195, 80, 255) : p.role == NET_ROLE_MOD ? IM_COL32(120, 190, 255, 255)
                                                                                         : IM_COL32(245, 240, 225, 255);
    return p.role != NET_ROLE_PLAYER;
}

void online_list_layout(float w, float h, float scale) {
    if (!g_list_on || net_state() != NET_ONLINE) return;
    NetPlayer pl[NET_MAX_PLAYERS];
    NetTeam teams[NET_MAX_TEAMS];
    int n = net_players(pl, NET_MAX_PLAYERS), nt = net_teams(teams, NET_MAX_TEAMS);
    float ww = fminf(w * 0.55f, 900 * scale);
    ImGui::SetNextWindowPos(ImVec2(w / 2, h * 0.42f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(ww, 0));
    ImGui::SetNextWindowBgAlpha(0.86f);
    ImGui::Begin("##online_list", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::Text("Players online: %d", n);
    ImGui::SameLine(ww - ImGui::CalcTextSize("Press the list key again to close").x - 40 * scale);
    ImGui::TextDisabled("Press the list key again to close");
    if (ImGui::BeginTable("##players", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Name", 0, 2.4f);
        ImGui::TableSetupColumn("Character", 0, 1.4f);
        ImGui::TableSetupColumn("Area", 0, 2.2f);
        ImGui::TableSetupColumn("Team", 0, 1.6f);
        ImGui::TableSetupColumn("Ping", 0, 0.8f);
        ImGui::TableSetupColumn("##flags", 0, 1.2f);
        ImGui::TableHeadersRow();
        for (int i = 0; i < n; i++) {
            const NetPlayer &p = pl[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImU32 col;
            online_player_color(p, &col);
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            ImGui::Text("%s%s", p.name, p.id == net_self_id() ? " (you)" : "");
            ImGui::PopStyleColor();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(p.character);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(p.area);
            ImGui::TableNextColumn();
            const char *tn = "-";
            for (int k = 0; k < nt; k++)
                if (teams[k].id == p.team) tn = teams[k].name;
            ImGui::TextUnformatted(tn);
            ImGui::TableNextColumn();
            if (p.id == net_self_id()) {
                ImGui::TextDisabled("-");
            } else {
                ImVec4 pc = p.ping_ms < 60 ? ImVec4(0.45f, 0.9f, 0.5f, 1) : p.ping_ms < 120 ? ImVec4(0.95f, 0.85f, 0.35f, 1)
                                                                                           : ImVec4(0.95f, 0.45f, 0.35f, 1);
                ImGui::TextColored(pc, "%u ms", p.ping_ms);
            }
            ImGui::TableNextColumn();
            if (p.role == NET_ROLE_HOST) ImGui::TextColored(ImVec4(0.94f, 0.76f, 0.31f, 1), "HOST");
            else if (p.role == NET_ROLE_MOD) ImGui::TextColored(ImVec4(0.47f, 0.75f, 1.0f, 1), "MOD");
            if (p.muted) {
                ImGui::SameLine();
                ImGui::TextDisabled("muted");
            }
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

// ---- net events and the per-frame pump
static void on_events() {
    static char kicked[NET_TEXT_MAX];   // the reason of the last NET_EV_KICKED, until its NET_EV_STATE arrives
    NetEvent ev[16];
    int n;
    while ((n = net_poll_events(ev, 16)) > 0) {
        for (int i = 0; i < n; i++) {
            const NetEvent &e = ev[i];
            char t[256];
            switch (e.type) {
            case NET_EV_PLAYER_JOINED: snprintf(t, sizeof t, "%s joined", e.name), online_toast(t, 1); break;
            case NET_EV_PLAYER_LEFT: snprintf(t, sizeof t, "%s left", e.name), online_toast(t, 0); break;
            case NET_EV_TEAM_INVITE:
                snprintf(t, sizeof t, "%s invites you to the team \"%s\"", e.name, e.text);
                online_toast_ask(t, 2, e.request);
                break;
            case NET_EV_TELEPORT_REQUEST:
                snprintf(t, sizeof t, "%s asks to teleport to you", e.name);
                online_toast_ask(t, 1, e.request);
                break;
            case NET_EV_TELEPORT_ANSWER:
                snprintf(t, sizeof t, e.accepted ? "%s accepted your teleport request" : "%s refused your teleport request", e.name);
                online_toast(t, e.accepted ? 1 : 2);
                break;
            case NET_EV_TELEPORT_GO:
            case NET_EV_SUMMON: {
                float p[3] = {e.pos[0], e.pos[1] + 1.0f, e.pos[2]};
                if (teleport_to(p)) snprintf(t, sizeof t, e.type == NET_EV_SUMMON ? "Summoned by the host" : "Teleporting to %s", e.name);
                else snprintf(t, sizeof t, "Can't teleport right now");
                online_toast(t, 0);
                break;
            }
            case NET_EV_KICKED:
                snprintf(t, sizeof t, "You were removed: %s", e.text), online_toast(t, 2);
                snprintf(kicked, sizeof kicked, "%s", e.text);
                break;
            case NET_EV_MUTED: online_toast(e.accepted ? "You were muted" : "You were unmuted", 2); break;
            case NET_EV_ROLE_CHANGED: online_toast(e.accepted ? "You are now a moderator" : "You are no longer a moderator", 0); break;
            case NET_EV_SERVER_MESSAGE: online_toast(e.text, 2); break;
            case NET_EV_STATE:   // a reason (full, banned, wrong password, lost...) replaces the bare state
                // a kick or ban ends the connection with the reason the "You were removed" toast already showed
                if (e.text[0] && !strcmp(e.text, kicked)) kicked[0] = 0;
                else if (e.text[0]) online_toast(e.text, 2);
                else online_toast(net_state() == NET_ONLINE ? "Connected" : net_state() == NET_CONNECTING ? "Connecting..." : "Disconnected", 0);
                break;
            }
        }
    }
}

// The area the invented and the real client report for the local player: from the height, until the map tiles are named.
static void report_local() {
    static uint64_t last;
    uint64_t now = plat_now_ns();
    if (now - last < 200000000ull) return;
    last = now;
    float p[3];
    if (!teleport_player_pos(p)) return;
    char area[NET_AREA_MAX];
    snprintf(area, sizeof area, "%s (%d, %d)", p[1] < -100 ? "Depths" : p[1] > 600 ? "Sky" : "Surface", (int)p[0], (int)p[2]);
    net_set_local(area, p, "Link");
}

static void connect_now() {
    NetConfig c = {};
    const char *nm = cfg_get("TOTK_NET_NAME"), *ad = cfg_get("TOTK_NET_ADDRESS"), *pw = cfg_get("TOTK_NET_PASSWORD");
    snprintf(c.name, sizeof c.name, "%s", nm && *nm ? nm : "Player");
    snprintf(c.address, sizeof c.address, "%s", ad ? ad : "");
    snprintf(c.token, sizeof c.token, "%s", pw ? pw : "");
    net_connect(&c);
}

void online_connect_now(void) { connect_now(); }

// ---- TOTK_TEST_INPUT=<file>: test input without the window's focus. A test script appends lines to the file; the game
// reads the lines added since the last look, a few times a second, and acts on each as the player's keys would, so a
// test game (TOTK_TEST_WINDOW) never has to be brought to the front. A line not yet ended by a newline waits.
//   type <text>     open the chat box on the all channel (if closed) and put <text> in it
//   enter           send what the chat box holds, through the same path as Enter (commands such as /w, /me, /who)
//   chat <text>     type, then enter
//   esc             close the chat box, the wheel and the confirmation dialog
//   wheel-down      the wheel key pressed (held), wheel-up released: the same calls as the key itself
//   wheel <n>       the wheel opened on a scripted screen (online_wheel_demo: 0 closes it)
//   confirm         choose the confirmation dialog's yes button
//   list            show or hide the player list
//   yes, no         answer the newest question toast
static void test_input_line(char *l) {
    trim(l);
    char cmd[16] = "";
    int used = 0;
    sscanf(l, "%15s%n", cmd, &used);
    const char *arg = l + used;
    while (*arg == ' ') arg++;
    rt_log("online: test input: %s", l);
    if (!strcmp(cmd, "type") || !strcmp(cmd, "chat")) {
        if (!g_chat.typing) online_chat_open(NET_CHAT_ALL, "");
        snprintf(g_chat.buf, sizeof g_chat.buf, "%s", arg);
        if (!strcmp(cmd, "chat")) chat_submit();
    } else if (!strcmp(cmd, "enter")) {
        if (g_chat.typing) chat_submit();
    } else if (!strcmp(cmd, "esc")) {
        if (g_chat.typing) chat_close();
        if (online_wheel_open()) online_wheel_close();
        if (g_conf.on) confirm_close();
    } else if (!strcmp(cmd, "wheel-down") || !strcmp(cmd, "wheel-up")) {
        online_hotkey(k_wheel.load(), !strcmp(cmd, "wheel-down"));
    } else if (!strcmp(cmd, "wheel")) {
        online_wheel_demo(atoi(arg));
    } else if (!strcmp(cmd, "confirm")) {
        if (g_conf.on) {
            void (*fn)(void *) = g_conf.fn;
            void *u = g_conf.user;
            confirm_close();
            if (fn) fn(u);
        }
    } else if (!strcmp(cmd, "list")) {
        online_list_toggle();
    } else if (!strcmp(cmd, "yes") || !strcmp(cmd, "no")) {
        online_answer_ask(!strcmp(cmd, "yes"));
    } else if (*cmd) {
        rt_log("online: test input: unknown command %s", cmd);
    }
}

static void test_input_poll() {
    static const char *path = cfg_get("TOTK_TEST_INPUT");
    static long offset;
    static double next;
    if (!path || !*path) return;
    double now = now_s();
    if (now < next) return;
    next = now + 0.2;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char buf[4096];
    if (fseek(f, offset, SEEK_SET) == 0) {
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        buf[n] = 0;
        char *start = buf, *nl;
        while ((nl = (char *)memchr(start, '\n', (size_t)(buf + n - start)))) {
            *nl = 0;
            offset += nl - start + 1;
            test_input_line(start);
            start = nl + 1;
        }
    }
    fclose(f);
}

void online_pump(uint64_t frame) {
    (void)frame;
    if (!net_enabled()) return;
    keys_load();
    static bool auto_done;
    if (!auto_done && net_state() == NET_OFFLINE) {
        auto_done = true;
        const char *ac = cfg_get("TOTK_NET_AUTOCONNECT");
        if (fake_net() || (ac && !strcmp(ac, "1"))) connect_now();
    }
    net_tick();
    on_events();
    report_local();
    puppet_self_frame();
    test_input_poll();
    double now = now_s();
    toasts_expire(now);
    if (menu_is_open()) {   // the menu has the screen: close what holds the game's input
        if (online_wheel_open()) online_wheel_close();
        if (g_chat.typing) chat_close();
        if (g_conf.on) confirm_close();
    }
    if (g_req_chat.exchange(0) && !menu_is_open()) {
        if (net_state() == NET_ONLINE && !g_chat.typing) online_chat_open(NET_CHAT_ALL, "");
        else if (!g_chat.typing) online_modal_set(MODAL_CHAT, false);
    }
    if (g_req_list.exchange(0)) online_list_toggle();
    if (int hop = g_req_hop.exchange(0)) {   // debug: a small hop, behind TOTK_NET_FAKE
        float p[3];
        if (teleport_player_pos(p)) {
            p[0] += 25.0f * (float)hop;
            online_toast(teleport_to(p) ? "Debug teleport" : "Debug teleport refused", 0);
        } else {
            online_toast("Debug teleport: no player yet", 2);
        }
    }
    bool yes = g_req_yes.exchange(0) != 0, no = g_req_no.exchange(0) != 0;
    if (yes || no)
        for (size_t i = 0; i < g_toasts.size(); i++)
            if (g_toasts[i].what) {
                toast_answer(i, yes);
                break;
            }
}

// ---- name tags over the other players' puppets
// Each puppet's name is drawn behind every window, over the point TAG_HEIGHT above its feet, projected with the scene
// camera's view-projection (vk_camera.h). The clip w of a perspective projection is the depth in front of the camera,
// so a point with w at or under TAG_NEAR is behind the camera and gets no tag, and w also sets the fade: full up to
// TAG_FULL_M, gone at TAG_FAR_M, the text a little smaller with distance. No tag while the scene camera is stale (a
// menu, a loading screen: no scene was drawn for TAG_CAM_STALE_NS). The game draws with an upper-left window origin
// (docs/maxwell.md 1.15), so NDC y -1 is the top of the screen; TOTK_PUPPET_TAG_FLIPY=1 turns that round.
#define TAG_HEIGHT 2.05f
#define TAG_NEAR 0.3f
#define TAG_FULL_M 40.0f
#define TAG_FAR_M 120.0f
#define TAG_CAM_STALE_NS 250000000ull

void online_tags_layout(float w, float h, float scale) {
    if (net_state() != NET_ONLINE) return;
    float m[16];
    uint64_t age = 0;
    int cam = vk_camera_viewproj(m, &age);
    static const int flip = rt_knob("PUPPET_TAG_FLIPY", "0");
    NetPuppet pp[NET_MAX_PLAYERS];
    int n = net_puppets(pp, NET_MAX_PLAYERS);
    static double next_log;   // every 5 s: the camera and each puppet's projection, for checking tags in a log
    if (now_s() >= next_log) {
        next_log = now_s() + 5;
        char line[512];
        int o = snprintf(line, sizeof line, "puppets: tags: camera %d (%.0f ms old), %d puppets:", cam, (double)age / 1e6, n);
        for (int i = 0; i < n && i < 6 && cam && o < (int)sizeof line - 64; i++) {
            float c[4];
            for (int r = 0; r < 4; r++)
                c[r] = m[r * 4] * pp[i].pos[0] + m[r * 4 + 1] * (pp[i].pos[1] + TAG_HEIGHT) + m[r * 4 + 2] * pp[i].pos[2] + m[r * 4 + 3];
            o += snprintf(line + o, sizeof line - o, " [%s w %.1f ndc %.2f %.2f]", pp[i].name, c[3], c[3] ? c[0] / c[3] : 0.0f,
                          c[3] ? c[1] / c[3] : 0.0f);
        }
        rt_log("%s", line);
    }
    if (!cam || age > TAG_CAM_STALE_NS) return;
    if (n <= 0) return;
    ImDrawList *dl = ImGui::GetBackgroundDrawList();
    ImFont *font = ImGui::GetFont();
    float base = ImGui::GetFontSize();
    NetPlayer pl;
    for (int i = 0; i < n; i++) {
        const NetPuppet &p = pp[i];
        float x = p.pos[0], y = p.pos[1] + TAG_HEIGHT, z = p.pos[2], c[4];
        for (int r = 0; r < 4; r++) c[r] = m[r * 4] * x + m[r * 4 + 1] * y + m[r * 4 + 2] * z + m[r * 4 + 3];
        if (!(c[3] > TAG_NEAR) || c[3] > TAG_FAR_M) continue;
        float nx = c[0] / c[3], ny = c[1] / c[3];
        if (fabsf(nx) > 1.2f || fabsf(ny) > 1.2f) continue;
        if (flip) ny = -ny;
        float sx = (nx * 0.5f + 0.5f) * w, sy = (ny * 0.5f + 0.5f) * h;
        float fade = c[3] <= TAG_FULL_M ? 1.0f : (TAG_FAR_M - c[3]) / (TAG_FAR_M - TAG_FULL_M);
        float a = fade * p.alpha;
        if (a <= 0.02f) continue;
        float size = base * (c[3] < 10 ? 1.0f : fmaxf(0.7f, 1.0f - (c[3] - 10) * 0.004f));
        ImU32 col = IM_COL32(245, 240, 225, 255);
        if (net_player_by_id(p.id, &pl)) online_player_color(pl, &col);
        ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0, p.name);
        ImVec2 at(sx - ts.x / 2, sy - ts.y);
        int ia = (int)(a * 255);
        float pad = 4 * scale;
        dl->AddRectFilled(ImVec2(at.x - pad, at.y - pad / 2), ImVec2(at.x + ts.x + pad, at.y + ts.y + pad / 2),
                          IM_COL32(0, 0, 0, ia * 110 / 255), 4 * scale);
        dl->AddText(font, size, ImVec2(at.x + 1, at.y + 1), IM_COL32(0, 0, 0, ia * 200 / 255), p.name);
        dl->AddText(font, size, at, (col & ~IM_COL32_A_MASK) | ((ImU32)ia << IM_COL32_A_SHIFT), p.name);
    }
}

void online_layout(float w, float h, float scale, bool menu_open) {
    if (!net_enabled() || menu_open) return;
    online_tags_layout(w, h, scale);
    online_chat_layout(w, h, scale);
    online_list_layout(w, h, scale);
    online_toasts_layout(w, h, scale);
    online_wheel_layout(w, h, scale);
    online_confirm_layout(w, h, scale);
}

// ---- scripted screenshots: with TOTK_NET_DEMO_DIR set (and TOTK_NET_FAKE=1) the UI walks through its screens by itself,
// once the player is in the world, and the present path saves the swapchain image after each step as <dir>/<name>.png.
// For the screenshots of docs/online-ui.md; no input is needed.
const char *online_demo_shot(uint64_t frame) {
    static const char *dir = cfg_get("TOTK_NET_DEMO_DIR");
    if (!dir || !*dir || !fake_net() || net_state() != NET_ONLINE) return nullptr;
    static uint64_t base;
    static int step = -1;
    static char path[600];
    static const char *pending;
    static uint64_t shot_at;
    float p[3];
    if (!base) {
        if (frame_step_world_us() < 0 || !teleport_player_pos(p)) return nullptr;   // the save has loaded
        base = frame + 300;   // let the world settle
        return nullptr;
    }
    if (pending && frame >= shot_at) {
        snprintf(path, sizeof path, "%s/%s.png", dir, pending);
        pending = nullptr;
        return path;
    }
    static const struct { int at; const char *name; int what; } script[] = {
        {0, "01-chat-open", 1}, {50, "02-chat-idle", 2}, {100, "03-player-list", 3}, {150, nullptr, 4},
        {200, "05-wheel-top", 5}, {250, "06-wheel-teleport-pick", 6}, {300, "07-wheel-team", 7},
        {350, "08-wheel-host", 8}, {400, "09-confirm-kick", 9}, {470, "10-teleported", 10}, {520, nullptr, 11}, {800, "11-toasts", 12},
    };
    if (frame < base || step + 1 >= (int)(sizeof script / sizeof *script)) return nullptr;
    if (frame < base + (uint64_t)script[step + 1].at) return nullptr;
    step++;
    switch (script[step].what) {
    case 1:
        online_chat_open(NET_CHAT_ALL, "Hello everyone, the chat box works!");
        break;
    case 2:
        net_chat_send(NET_CHAT_ALL, nullptr, g_chat.buf), chat_close();
        break;
    case 3: online_list_toggle(); break;
    case 4:
        online_list_toggle();   // closes the list; the fake server's requests are on screen by now
        break;
    case 5: online_wheel_demo(1); break;
    case 6: online_wheel_demo(2); break;
    case 7: online_wheel_demo(3); break;
    case 8: online_wheel_demo(4); break;
    case 9:
        online_wheel_demo(5);
        break;
    case 10:
        if (g_conf.on) confirm_close();
        g_req_hop = 1;
        break;
    case 11:
        if (g_conf.on) confirm_close();
        online_wheel_close();
        break;
    }
    if (script[step].name) pending = script[step].name, shot_at = frame + 8;
    return nullptr;
}
