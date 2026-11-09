// The Online tab of the F1 menu: the player's name, the server address, the key bindings of the chat, the wheel, the
// player list and the answers to requests, the pad buttons of the wheel, and the wheel and chat options. Plain ImGui
// widgets over cfg_get / cfg_set (TOTK_NET_*); the keys are saved to totk.ini a moment after the last change.
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "imgui.h"
#include "ui/online_ui.h"
#include "ui/option_rows.h"

extern "C" {
#include "cfg.h"
#include "input.h"
#include "input_map.h"
}

static bool g_capture_typing;   // a text field or a key capture has the keyboard: the menu keeps Esc, Q and E for it

extern "C" int online_tab_modal(void) { return g_capture_typing; }

static const char *cfg_or(const char *key, const char *def) {
    const char *v = cfg_get(key);
    return v && *v ? v : def;
}

static void set(const char *key, const char *value) {
    cfg_set(key, value);
    opt_save_soon();
}

static void text_row(const char *label, const char *key, const char *def, const char *hint, bool *typing,
                     ImGuiInputTextFlags flags = 0) {
    static char buf[128];
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(ImGui::GetWindowWidth() * 0.34f);
    ImGui::SetNextItemWidth(ImGui::GetWindowWidth() * 0.42f);
    char id[64];
    snprintf(id, sizeof id, "##%s", key);
    snprintf(buf, sizeof buf, "%s", cfg_or(key, def));
    if (ImGui::InputTextWithHint(id, hint, buf, sizeof buf, flags)) set(key, buf);
    if (ImGui::IsItemActive()) *typing = true;
}

static const char *chord_label(const char *v, char *out, size_t n) {
    static const struct { const char *k, *label; } names[] = {
        {"a", "A"}, {"b", "B"}, {"x", "X"}, {"y", "Y"}, {"lstick", "Left stick click"}, {"rstick", "Right stick click"},
        {"l", "L"}, {"r", "R"}, {"zl", "ZL"}, {"zr", "ZR"}, {"plus", "Plus"}, {"start", "Plus"}, {"minus", "Minus"},
        {"back", "Minus"}, {"left", "D-pad left"}, {"up", "D-pad up"}, {"right", "D-pad right"}, {"down", "D-pad down"}};
    if (!strcmp(v, "none")) return "Off";
    char buf[96];
    snprintf(buf, sizeof buf, "%s", v);
    out[0] = 0;
    for (char *t = strtok(buf, "+, "); t; t = strtok(nullptr, "+, "))
        for (auto &nm : names)
            if (!strcmp(t, nm.k)) {
                if (out[0]) strncat(out, " + ", n - strlen(out) - 1);
                strncat(out, nm.label, n - strlen(out) - 1);
            }
    return out;
}

static const struct { int bit; const char *name; } kPadNames[] = {
    {B_A, "a"}, {B_B, "b"}, {B_X, "x"}, {B_Y, "y"}, {B_STICK_L, "lstick"}, {B_STICK_R, "rstick"}, {B_L, "l"}, {B_R, "r"},
    {B_ZL, "zl"}, {B_ZR, "zr"}, {B_PLUS, "plus"}, {B_MINUS, "minus"}, {B_LEFT, "left"}, {B_UP, "up"}, {B_RIGHT, "right"},
    {B_DOWN, "down"}};

void online_tab(float scale, float bottom) {
    (void)scale;
    g_capture_typing = false;
    static int capturing = -1;      // which key row waits for a key; 100 the pad chord
    static uint64_t chord_seen;      // buttons seen held during a pad capture
    ImGui::BeginChild("##online_page", ImVec2(0, -bottom), ImGuiChildFlags_None, ImGuiWindowFlags_None);

    // ---- connection
    NetState st = net_state();
    ImGui::TextColored(ImVec4(0.93f, 0.76f, 0.33f, 1), "Connection");
    if (!net_enabled()) {
        ImGui::TextWrapped("Enter a server address below to play online. Without one the game stays single-player.");
    } else {
        ImGui::Text("Status: %s", st == NET_ONLINE ? "online" : st == NET_CONNECTING ? "connecting..." : "offline");
        ImGui::SameLine();
        if (st == NET_OFFLINE) {
            if (ImGui::Button("Connect")) online_connect_now();
        } else if (ImGui::Button("Disconnect")) {
            net_disconnect();
        }
        const char *why = net_status_text();
        if (st == NET_OFFLINE && why && *why) ImGui::TextColored(ImVec4(0.96f, 0.80f, 0.35f, 1), "%s", why);
    }
    text_row("Your name", "TOTK_NET_NAME", "Player", "shown to other players", &g_capture_typing);
    text_row("Server address", "TOTK_NET_ADDRESS", "", "host:port", &g_capture_typing);
    text_row("Password", "TOTK_NET_PASSWORD", "", "the room's password or invite code, if it has one", &g_capture_typing,
             ImGuiInputTextFlags_Password);
    bool ac = cfg_or("TOTK_NET_AUTOCONNECT", "0")[0] == '1';
    if (ImGui::Checkbox("Connect when the game starts", &ac)) set("TOTK_NET_AUTOCONNECT", ac ? "1" : "0");

    // ---- keys
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.93f, 0.76f, 0.33f, 1), "Keys");
    static const struct { const char *label, *key, *def, *about; } rows[] = {
        {"Open chat", "TOTK_NET_KEY_CHAT", "enter", "Press it, type, Enter sends, Esc closes."},
        {"Wheel menu (hold)", "TOTK_NET_KEY_WHEEL", "v", "Hold it, aim, let go to choose."},
        {"Player list", "TOTK_NET_KEY_LIST", "p", "Shows or hides the list of players."},
        {"Accept a request", "TOTK_NET_KEY_ACCEPT", "y", "Answers the oldest teleport or team request."},
        {"Refuse a request", "TOTK_NET_KEY_REFUSE", "n", "Refuses it."},
    };
    for (int i = 0; i < 5; i++) {
        ImGui::PushID(i);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(rows[i].label);
        ImGui::SameLine(ImGui::GetWindowWidth() * 0.34f);
        char shown[48];
        const char *cur = cfg_or(rows[i].key, rows[i].def);
        if (capturing == i) snprintf(shown, sizeof shown, "Press a key...");
        else snprintf(shown, sizeof shown, "%s", !strcmp(cur, "none") ? "Unbound" : cur);
        if (capturing != i) for (char *p = shown; *p; p++) *p = (char)toupper((unsigned char)*p);
        if (ImGui::Button(shown, ImVec2(ImGui::GetWindowWidth() * 0.2f, 0))) capturing = capturing == i ? -1 : i;
        ImGui::SameLine();
        ImGui::TextDisabled("%s", rows[i].about);
        if (capturing == i) {
            g_capture_typing = true;
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                capturing = -1;
            } else if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) {
                set(rows[i].key, "none");
                capturing = -1;
            } else {
                for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; k++) {
                    if (!strcmp(online_key_name(k), "none") || !ImGui::IsKeyPressed((ImGuiKey)k, false)) continue;
                    set(rows[i].key, online_key_name(k));
                    capturing = -1;
                    break;
                }
            }
        }
        ImGui::PopID();
    }
    // the pad buttons of the wheel
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Wheel on a controller");
        ImGui::SameLine(ImGui::GetWindowWidth() * 0.34f);
        char lab[160];
        const char *cur = cfg_or("TOTK_NET_PAD_WHEEL", "minus+lstick");
        if (capturing == 100) snprintf(lab, sizeof lab, "Hold the buttons...");
        else chord_label(cur, lab, sizeof lab);
        if (ImGui::Button(lab, ImVec2(ImGui::GetWindowWidth() * 0.2f, 0))) {
            capturing = capturing == 100 ? -1 : 100;
            chord_seen = 0;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Hold these buttons together. Click, hold them, let go.");
        if (capturing == 100) {
            g_capture_typing = true;
            uint64_t b = 0;
            int32_t lx, ly;
            input_pad_read(&b, &lx, &ly);
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                capturing = -1;
            } else if (b) {
                chord_seen |= b;
            } else if (chord_seen) {   // everything let go: the buttons held together are the chord
                char v[96] = "";
                for (auto &pn : kPadNames)
                    if (chord_seen & (1ull << pn.bit)) {
                        if (v[0]) strncat(v, "+", sizeof v - strlen(v) - 1);
                        strncat(v, pn.name, sizeof v - strlen(v) - 1);
                    }
                set("TOTK_NET_PAD_WHEEL", v);
                capturing = -1;
            }
        }
    }
    if (ImGui::Button("Reset keys")) {
        for (auto &r : rows) set(r.key, r.def);
        set("TOTK_NET_PAD_WHEEL", "minus+lstick");
        capturing = -1;
    }
    ImGui::TextDisabled("While a key is waiting, Delete clears it and Esc cancels. The wheel key never reaches the game while online.");

    // ---- options
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.93f, 0.76f, 0.33f, 1), "Wheel and chat");
    int ws = atoi(cfg_or("TOTK_NET_WHEEL_SCALE", "100"));
    ImGui::SetNextItemWidth(ImGui::GetWindowWidth() * 0.42f);
    if (ImGui::SliderInt("Wheel size", &ws, 60, 150, "%d %%")) {
        char v[16];
        snprintf(v, sizeof v, "%d", ws);
        set("TOTK_NET_WHEEL_SCALE", v);
    }
    int fade = atoi(cfg_or("TOTK_NET_CHAT_FADE", "10"));
    ImGui::SetNextItemWidth(ImGui::GetWindowWidth() * 0.42f);
    if (ImGui::SliderInt("Chat fades after", &fade, 3, 30, "%d s")) {
        char v[16];
        snprintf(v, sizeof v, "%d", fade);
        set("TOTK_NET_CHAT_FADE", v);
    }
    int lines = atoi(cfg_or("TOTK_NET_CHAT_LINES", "10"));
    ImGui::SetNextItemWidth(ImGui::GetWindowWidth() * 0.42f);
    if (ImGui::SliderInt("Chat lines shown", &lines, 4, 30)) {
        char v[16];
        snprintf(v, sizeof v, "%d", lines);
        set("TOTK_NET_CHAT_LINES", v);
    }
    const char *fake = cfg_get("TOTK_NET_FAKE");
    if (fake && !strcmp(fake, "1")) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.62f, 0.64f, 0.58f, 1),
                           "TOTK_NET_FAKE=1: an invented server is playing. F7 and F6 hop the player 25 m to the east or west.");
    }
    ImGui::EndChild();
}
