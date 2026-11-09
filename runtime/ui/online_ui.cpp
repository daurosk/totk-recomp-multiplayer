// The online UI: state shared by its parts, the hotkeys, the net events, the chat box, the player list, toasts and the
// confirmation dialog. The wheel is ui/online_wheel.cpp, the F1 tab ui/online_tab.cpp. See ui/online_ui.h.
#include <algorithm>
#include <atomic>
#include <ctype.h>
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

// ---- the game's look
static float g_look_a = 1;   // alpha of everything the look_* calls draw, set around a fade
static void look_alpha(float a) { g_look_a = a; }
static ImU32 fade(ImU32 c) {
    return g_look_a >= 1 ? c : (c & 0x00ffffffu) | ((ImU32)((float)(c >> IM_COL32_A_SHIFT) * g_look_a) << IM_COL32_A_SHIFT);
}

ImFont *look_font() {
    ImFont *f = online_game_font();
    return f ? f : ImGui::GetFont();
}

float look_width(float size, const char *text, const char *end) {
    ImFont *f = look_font();
    return f->CalcTextSizeA(size, FLT_MAX, 0, text, end).x;
}

// Cream text with a dark one-pixel shadow down and to the right, as the game's HUD text has.
void look_text(ImDrawList *dl, float size, ImVec2 pos, ImU32 col, const char *text, const char *end) {
    ImFont *f = look_font();
    float o = fmaxf(1.0f, size / 22.0f);
    dl->AddText(f, size, ImVec2(pos.x + o, pos.y + o), fade(IM_COL32(0, 0, 0, 200)), text, end);
    dl->AddText(f, size, pos, fade(col), text, end);
}

void look_bar(ImDrawList *dl, ImVec2 a, ImVec2 b, int alpha, float fade_w) {
    ImU32 solid = fade(IM_COL32(9, 11, 11, alpha)), clear = fade(IM_COL32(9, 11, 11, 0));
    if (fade_w <= 0 || b.x - a.x <= fade_w) return dl->AddRectFilled(a, b, solid);
    dl->AddRectFilled(a, ImVec2(b.x - fade_w, b.y), solid);
    dl->AddRectFilledMultiColor(ImVec2(b.x - fade_w, a.y), b, solid, clear, clear, solid);
}

void look_square(ImDrawList *dl, ImVec2 a, float side, ImU32 col) {
    dl->AddRectFilled(a, ImVec2(a.x + side, a.y + side), fade(col));
    dl->AddRect(a, ImVec2(a.x + side, a.y + side), fade(IM_COL32(255, 255, 255, 70)));
}

LookStyle::LookStyle() {
    ImGui::PushFont(look_font());
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(LOOK_TEXT));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.035f, 0.043f, 0.043f, 0.86f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.93f, 0.88f, 0.70f, 0.55f));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1, 1, 1, 0.07f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.94f, 0.89f, 0.66f, 0.30f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.94f, 0.89f, 0.66f, 0.45f));
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(1.0f, 0.93f, 0.62f, 0.9f));
}
LookStyle::~LookStyle() {
    ImGui::PopStyleColor(7);
    ImGui::PopStyleVar(2);
    ImGui::PopFont();
}

// ---- toasts: notice rows on the left, as the game shows its own pickup notices; a coloured square marks the kind
// (blue-grey info, green good, amber warning). A request (teleport, team invite) has a second row with the two answers.
void online_toasts_layout(float w, float h, float scale) {
    (void)w;
    double now = now_s();
    float x = 10 * scale, y = h * 0.34f, fs = 24 * scale, row_h = 42 * scale;
    const ImU32 tint[3] = {IM_COL32(140, 168, 208, 255), IM_COL32(125, 208, 140, 255), IM_COL32(244, 190, 92, 255)};
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    ImFont *font = look_font();
    for (size_t i = 0; i < g_toasts.size(); i++) {
        const Toast &t = g_toasts[i];
        double age = now - t.born, left = t.ttl - age;
        float a = (float)std::min({1.0, age / 0.2, left / 0.4});
        if (a <= 0) continue;
        look_alpha(a);
        bool ask = t.what != 0;
        // wrap to at most two rows of 640 px
        float maxw = 640 * scale;
        const char *s = t.text, *end = t.text + strlen(t.text);
        std::vector<std::pair<const char *, const char *>> rows;
        while (s < end && rows.size() < 3) {
            const char *brk = font->CalcWordWrapPositionA(fs / font->FontSize, s, end, maxw);
            if (brk <= s) brk = s + 1;
            rows.push_back({s, brk});
            s = brk;
            while (s < end && *s == ' ') s++;
        }
        float tw = 0;
        for (auto &r : rows) tw = fmaxf(tw, look_width(fs, r.first, r.second));
        float bh = (float)rows.size() * row_h + (ask ? row_h + 8 * scale : 0), bw = tw + 40 * scale + 72 * scale;
        if (ask) bw = fmaxf(bw, 470 * scale);
        look_bar(dl, ImVec2(x, y), ImVec2(x + bw, y + bh - 3 * scale), 125, 64 * scale);
        look_square(dl, ImVec2(x + 8 * scale, y + 8 * scale), 26 * scale, tint[t.kind % 3]);
        float ty = y + 5 * scale;
        for (auto &r : rows) {
            look_text(dl, fs, ImVec2(x + 40 * scale, ty), LOOK_TEXT, r.first, r.second);
            ty += row_h;
        }
        if (ask) {
            float by = y + (float)rows.size() * row_h + 2 * scale;
            dl->AddRectFilled(ImVec2(x + 40 * scale, by + row_h - 6 * scale), ImVec2(x + 40 * scale + (bw - 80 * scale) * (float)(left / t.ttl), by + row_h - 3 * scale),
                              fade(LOOK_GOLD));
            char yes[48], no[48];
            snprintf(yes, sizeof yes, "%s  Accept", online_key_name(k_yes.load()));
            snprintf(no, sizeof no, "%s  Refuse", online_key_name(k_no.load()));
            float bx = x + 40 * scale;
            char id[24];
            snprintf(id, sizeof id, "##toast%zu", i);
            ImGui::SetNextWindowPos(ImVec2(bx, by));
            ImGui::SetNextWindowSize(ImVec2(bw - 40 * scale, row_h - 8 * scale));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
            ImGui::Begin(id, nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
            const char *lab[2] = {yes, no};
            float bx2 = bx;
            for (int k = 0; k < 2; k++) {
                float lw = look_width(fs * 0.85f, lab[k]) + 26 * scale;
                ImGui::SetCursorScreenPos(ImVec2(bx2, by));
                ImGui::PushID(k);
                bool hit = ImGui::InvisibleButton("##ans", ImVec2(lw, row_h - 10 * scale));
                ImGui::PopID();
                bool hov = ImGui::IsItemHovered();
                ImVec2 p0 = ImGui::GetItemRectMin();
                dl->AddRectFilled(p0, ImVec2(p0.x + lw, p0.y + row_h - 10 * scale), fade(hov ? IM_COL32(240, 228, 170, 90) : IM_COL32(255, 255, 255, 28)));
                look_text(dl, fs * 0.85f, ImVec2(p0.x + 13 * scale, p0.y + 3 * scale), k ? LOOK_DIM : LOOK_GOLD, lab[k]);
                if (hit) (k ? g_req_no : g_req_yes) = 1;
                bx2 += lw + 12 * scale;
            }
            ImGui::End();
            ImGui::PopStyleVar(2);
        }
        y += bh + 4 * scale;
    }
    look_alpha(1);
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
    float pw = 560 * scale;
    ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2(0, 0), ImVec2(w, h), IM_COL32(0, 0, 0, 110));
    LookStyle look;
    ImGui::SetNextWindowPos(ImVec2(w / 2, h / 2), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(pw, 0));
    ImGui::SetNextWindowFocus();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22 * scale, 18 * scale));
    ImGui::Begin("##online_confirm", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextWrapped("%s", g_conf.q);
    ImGui::Spacing();
    float bw = (pw - 44 * scale - ImGui::GetStyle().ItemSpacing.x) / 2;
    bool yes = false, no = false;
    if (ImGui::Button(g_conf.yes, ImVec2(bw, 0))) yes = true;
    ImGui::SameLine();
    if (g_conf.first) ImGui::SetKeyboardFocusHere();   // Cancel starts focused: an accidental A or Enter does nothing
    if (ImGui::Button("Cancel", ImVec2(bw, 0))) no = true;
    g_conf.first = false;
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(LOOK_DIM));
    ImGui::TextUnformatted("Enter or A to choose, Esc or B to cancel");
    ImGui::PopStyleColor();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) no = true;
    ImGui::End();
    ImGui::PopStyleVar();
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
    int scroll;
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

static ImU32 chat_color(NetChannel c) {
    switch (c) {
    case NET_CHAT_TEAM: return IM_COL32(176, 232, 150, 255);
    case NET_CHAT_WHISPER: return IM_COL32(232, 178, 240, 255);
    case NET_CHAT_SYSTEM: return LOOK_GOLD;
    default: return LOOK_TEXT;
    }
}

static void chat_line_text(const NetChatLine &l, char *out, size_t n, size_t *prefix) {
    NetPlayer self;
    bool mine = net_player_by_id(net_self_id(), &self) && l.from == self.id;
    int pre = 0;
    switch (l.channel) {
    case NET_CHAT_SYSTEM: snprintf(out, n, "%s", l.text); break;
    case NET_CHAT_TEAM: snprintf(out, n, "[Team] %s: %n%s", l.from_name, &pre, l.text); break;
    case NET_CHAT_WHISPER:
        if (mine) snprintf(out, n, "[To %s] %n%s", l.to_name, &pre, l.text);
        else snprintf(out, n, "[From %s] %n%s", l.from_name, &pre, l.text);
        break;
    default: snprintf(out, n, "%s: %n%s", l.from_name, &pre, l.text);
    }
    *prefix = (size_t)pre;
}

// One wrapped row of the chat log: its text, the length of the sender's tag at its start, the channel.
struct ChatRow { std::string text; size_t prefix; NetChannel channel; };

// The chat box: the game's notice rows at the bottom left, newest last; a bar and a text field under them while typing.
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
    look_alpha(alpha);
    float fs = 24 * scale, row_h = 42 * scale, x0 = 10 * scale, tx = 40 * scale;
    float cw = fminf(w * 0.44f, 820 * scale);

    // wrap every kept line into rows of at most cw
    ImFont *font = look_font();
    std::vector<ChatRow> rows;
    int count = net_chat_count();
    for (int i = std::max(0, count - 60); i < count; i++) {
        NetChatLine l;
        if (!net_chat_get(i, &l)) continue;
        char text[NET_TEXT_MAX + 96];
        size_t prefix;
        chat_line_text(l, text, sizeof text, &prefix);
        const char *s = text, *end = text + strlen(text);
        bool first = true;
        while (s < end) {
            const char *brk = font->CalcWordWrapPositionA(fs / font->FontSize, s, end, cw - tx - 24 * scale);
            if (brk <= s) brk = s + 1;
            ChatRow r;
            r.text.assign(s, brk);
            r.prefix = first ? std::min(prefix, r.text.size()) : 0;
            r.channel = l.channel;
            rows.push_back(r);
            s = brk;
            while (s < end && *s == ' ') s++;
            first = false;
        }
    }
    int total = (int)rows.size();
    if (g_chat.typing && ImGui::GetIO().MouseWheel != 0) g_chat.follow = false;
    if (g_chat.follow) g_chat.scroll = 0;
    if (g_chat.typing) g_chat.scroll = std::max(0, std::min(g_chat.scroll + (int)ImGui::GetIO().MouseWheel, std::max(0, total - lines)));
    int last = total - g_chat.scroll, first_row = std::max(0, last - lines);
    float input_h = g_chat.typing ? 74 * scale : 0;
    float bottom = h - 86 * scale - input_h;
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    float y = bottom - (float)(last - first_row) * row_h;
    for (int i = first_row; i < last; i++, y += row_h) {
        const ChatRow &r = rows[(size_t)i];
        float tw = look_width(fs, r.text.c_str());
        look_bar(dl, ImVec2(x0, y), ImVec2(x0 + tx + tw + 64 * scale, y + row_h - 3 * scale), 120, 64 * scale);
        ImU32 tag = chat_color(r.channel);
        const char *s = r.text.c_str();
        float px = x0 + tx;
        if (r.prefix) {
            look_text(dl, fs, ImVec2(px, y + 5 * scale), tag, s, s + r.prefix);
            px += look_width(fs, s, s + r.prefix);
        }
        look_text(dl, fs, ImVec2(px, y + 5 * scale), r.channel == NET_CHAT_ALL ? LOOK_TEXT : tag, s + r.prefix);
    }
    if (g_chat.typing) {
        float iy = bottom + 6 * scale;
        const char *chname = g_chat.server_msg ? "Server message" : g_chat.channel == NET_CHAT_TEAM ? "Team" :
                             g_chat.channel == NET_CHAT_WHISPER ? g_chat.whisper_to : "All";
        char label[64];
        snprintf(label, sizeof label, "%s", chname);
        ImU32 c = chat_color(g_chat.server_msg ? NET_CHAT_SYSTEM : g_chat.channel);
        float lw = look_width(fs, label) + 40 * scale;
        look_bar(dl, ImVec2(x0, iy), ImVec2(x0 + cw, iy + row_h), 175);
        look_square(dl, ImVec2(x0 + 8 * scale, iy + 8 * scale), 26 * scale, c);
        look_text(dl, fs, ImVec2(x0 + tx + 4 * scale, iy + 5 * scale), c, label);
        const char *hint = "Enter sends, Tab changes channel, Esc closes. /w name message whispers.";
        look_bar(dl, ImVec2(x0, iy + row_h + 3 * scale), ImVec2(x0 + 18 * scale + look_width(16 * scale, hint) + 40 * scale, iy + row_h + 3 * scale + 28 * scale), 100, 40 * scale);
        look_text(dl, 16 * scale, ImVec2(x0 + 14 * scale, iy + row_h + 7 * scale), LOOK_DIM, hint);
        // the text field: a transparent ImGui field over the bar, in the game's font and colours
        ImGui::SetNextWindowPos(ImVec2(x0 + tx + lw, iy));
        ImGui::SetNextWindowSize(ImVec2(cw - tx - lw, row_h));
        ImGui::SetNextWindowFocus();
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4 * scale, 7 * scale));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(LOOK_TEXT));
        ImGui::PushFont(font);
        ImGui::Begin("##online_chat", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::SetNextItemWidth(-1);
        if (!g_chat.prefilled) {
            ImGui::SetKeyboardFocusHere();
            g_chat.prefilled = true;
        }
        if (ImGui::InputText("##msg", g_chat.buf, sizeof g_chat.buf,
                             ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackCompletion, chat_cb))
            chat_submit();
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) chat_close();
        ImGui::End();
        ImGui::PopFont();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(3);
    }
    look_alpha(1);
}

// ---- player list: the game's HUD rows at the top right, one per player (a coloured square for the team, the name, the
// role and ping on the right, the character and place small underneath), under a small "Players online" line.
static bool g_list_on;
void online_list_toggle(void) { g_list_on = !g_list_on; }

bool online_player_color(const NetPlayer &p, ImU32 *col) {
    *col = p.role == NET_ROLE_HOST ? LOOK_GOLD : p.role == NET_ROLE_MOD ? IM_COL32(150, 205, 255, 255) : LOOK_TEXT;
    return p.role != NET_ROLE_PLAYER;
}

static ImU32 team_color(uint32_t team) {
    static const ImU32 pal[] = {IM_COL32(140, 165, 205, 255), IM_COL32(125, 205, 140, 255), IM_COL32(240, 185, 90, 255),
                                IM_COL32(215, 140, 200, 255), IM_COL32(120, 210, 210, 255), IM_COL32(230, 120, 110, 255)};
    return team ? pal[(team - 1) % (sizeof pal / sizeof *pal)] : IM_COL32(170, 178, 190, 255);
}

void online_list_layout(float w, float h, float scale) {
    (void)h;
    if (!g_list_on || net_state() != NET_ONLINE) return;
    NetPlayer pl[NET_MAX_PLAYERS];
    NetTeam teams[NET_MAX_TEAMS];
    int n = net_players(pl, NET_MAX_PLAYERS), nt = net_teams(teams, NET_MAX_TEAMS);
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    float row_h = 58 * scale, big = 25 * scale, small = 17 * scale, rw = 430 * scale, x = w - 14 * scale - rw, y = 78 * scale;
    char head[48];
    snprintf(head, sizeof head, "Players online  %d", n);
    look_bar(dl, ImVec2(x, y), ImVec2(x + rw, y + 34 * scale), 130);
    look_text(dl, small * 1.1f, ImVec2(x + 12 * scale, y + 6 * scale), LOOK_GOLD, head);
    char hint[40];
    snprintf(hint, sizeof hint, "%s: close", online_key_name(k_list.load()));
    look_text(dl, small * 0.9f, ImVec2(x + rw - 10 * scale - look_width(small * 0.9f, hint), y + 9 * scale), LOOK_DIM, hint);
    y += 38 * scale;
    for (int i = 0; i < n; i++) {
        const NetPlayer &p = pl[i];
        bool self = p.id == net_self_id();
        look_bar(dl, ImVec2(x, y), ImVec2(x + rw, y + row_h - 4 * scale), self ? 150 : 105);
        look_square(dl, ImVec2(x + 10 * scale, y + 9 * scale), 30 * scale, team_color(p.team));
        ImU32 col;
        online_player_color(p, &col);
        char nm[NET_NAME_MAX + 8];
        snprintf(nm, sizeof nm, "%s", p.name);
        look_text(dl, big, ImVec2(x + 52 * scale, y + 3 * scale), col, nm);
        const char *tn = nullptr;
        for (int k = 0; k < nt; k++)
            if (teams[k].id == p.team) tn = teams[k].name;
        char sub[160];
        snprintf(sub, sizeof sub, "%s  %s%s%s%s", p.character, p.area, tn ? "  Team " : "", tn ? tn : "", p.muted ? "  muted" : "");
        look_text(dl, small, ImVec2(x + 52 * scale, y + 31 * scale), LOOK_DIM, sub);
        char right[40] = "";
        if (self) snprintf(right, sizeof right, "You");
        else if (p.role == NET_ROLE_HOST) snprintf(right, sizeof right, "Host  %u ms", p.ping_ms);
        else if (p.role == NET_ROLE_MOD) snprintf(right, sizeof right, "Mod  %u ms", p.ping_ms);
        else snprintf(right, sizeof right, "%u ms", p.ping_ms);
        ImU32 pc = self ? LOOK_DIM : p.ping_ms < 60 ? IM_COL32(170, 230, 160, 255) : p.ping_ms < 120 ? LOOK_GOLD : IM_COL32(240, 140, 110, 255);
        look_text(dl, small * 1.05f, ImVec2(x + rw - 12 * scale - look_width(small * 1.05f, right), y + 8 * scale), pc, right);
        y += row_h;
    }
}

// ---- net events and the per-frame pump
static void on_events() {
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
            case NET_EV_KICKED: snprintf(t, sizeof t, "You were removed: %s", e.text), online_toast(t, 2); break;
            case NET_EV_MUTED: online_toast(e.accepted ? "You were muted" : "You were unmuted", 2); break;
            case NET_EV_ROLE_CHANGED: online_toast(e.accepted ? "You are now a moderator" : "You are no longer a moderator", 0); break;
            case NET_EV_SERVER_MESSAGE: online_toast(e.text, 2); break;
            case NET_EV_STATE:
                online_toast(net_state() == NET_ONLINE ? "Connected" : net_state() == NET_CONNECTING ? "Connecting..." : "Disconnected", 0);
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
    const char *nm = cfg_get("TOTK_NET_NAME"), *ad = cfg_get("TOTK_NET_ADDRESS");
    snprintf(c.name, sizeof c.name, "%s", nm && *nm ? nm : "Player");
    snprintf(c.address, sizeof c.address, "%s", ad ? ad : "");
    net_connect(&c);
}

void online_connect_now(void) { connect_now(); }

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

void online_layout(float w, float h, float scale, bool menu_open) {
    if (!net_enabled() || menu_open) return;
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
        {0, "01-chat-open", 1}, {50, "02-chat-idle", 2}, {100, "03-player-list", 3}, {150, "04-toasts", 4},
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
        online_list_toggle();   // closes the list
        online_toast("Dax joined the server", 1);   // one of each kind, for the screenshot
        online_toast("Bao is far away: the teleport may take a while", 2);
        online_toast_ask("Alice asks to teleport to you", 1, 0);
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
