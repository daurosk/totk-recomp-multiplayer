// The online UI on the menu's ImGui context: the chat box, the player list, toasts, the wheel menu and the Online
// tab of the F1 menu. It reads the world through net/net_client.h only. ui/menu.cpp calls online_pump and
// online_layout once per presented frame; the window adapters (menu_win.cpp, menu_sdl.cpp) call online_hotkey and
// friends from the window thread.
//
// Input: while the chat box has the keyboard, the wheel is open or a confirmation is up, online_modal() is non-zero:
// the game gets no input (input_menu, input.h) and the window adapters hand keys, characters and the mouse to ImGui.
// The chat key, the wheel key, the list key and the answer keys are watched by online_hotkey even when the menu is
// closed, so they work while the game has the keyboard.
//
// The wheel menu takes extra slices from add-ons: online_wheel_register (below). docs/online-ui.md describes the
// screens and the settings.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- any thread
int online_modal(void);                        // non-zero while the chat box, the wheel or a confirmation takes input
// ---- window thread. imgui_key: an ImGuiKey. Returns 1 when the key was taken (its character must be dropped).
int online_hotkey(int imgui_key, int down);
void online_focus_lost(void);                  // the window lost the focus: a held wheel key is let go
int online_wants_frame(void);                  // present path: something to lay out although the menu is closed

#ifdef __cplusplus
}

#include "imgui.h"
#include "net/net_client.h"

// ---- present thread (ui/menu.cpp)
void online_pump(uint64_t frame);                                  // every presented frame, before the layout
void online_layout(float w, float h, float scale, bool menu_open); // between NewFrame and Render
const char *online_demo_shot(uint64_t frame);                      // TOTK_NET_DEMO_DIR: a path when this frame is to be saved
void online_tab(float scale, float bottom);                        // the Online tab's page

// ---- shared by the online_*.cpp files
enum { MODAL_CHAT = 1, MODAL_WHEEL = 2, MODAL_CONFIRM = 4 };
void online_modal_set(int bit, bool on);
void online_toast(const char *text, int kind);                     // 0 info, 1 good, 2 warning
void online_toast_ask(const char *text, int what, uint32_t request); // what: 1 teleport request, 2 team invite
void online_chat_open(NetChannel channel, const char *prefill, const char *prompt = nullptr);
void online_chat_server_message(void);                             // the chat box in "server message" mode
void online_list_toggle(void);
void online_confirm(const char *question, const char *yes_label, void (*yes)(void *), void *user);
void online_chat_layout(float w, float h, float scale);
void online_list_layout(float w, float h, float scale);
void online_tags_layout(float w, float h, float scale);
void online_toasts_layout(float w, float h, float scale);
void online_confirm_layout(float w, float h, float scale);
const char *online_key_name(int imgui_key);                        // "V", "Enter"; "none" for ImGuiKey_None
int online_key_from_name(const char *name);
int online_cfg_key(const char *setting, int fallback);             // a bound key from the settings
bool online_player_color(const NetPlayer &p, ImU32 *col);          // role colour of a name

// ---- the game's look (ui/online_ui.cpp): everything the online UI draws over the game uses these. The face is the Switch
// standard shared font read from the player's files (ui/menu.cpp online_game_font; the menu font when there is none),
// text is cream with a one-pixel dark shadow, panels are sharp-cornered translucent dark bars, and a coloured square
// marks a person or a kind, as in the game's own HUD.
ImFont *online_game_font();
ImFont *look_font();
void look_text(ImDrawList *dl, float size, ImVec2 pos, ImU32 col, const char *text, const char *end = nullptr);
float look_width(float size, const char *text, const char *end = nullptr);
void look_bar(ImDrawList *dl, ImVec2 a, ImVec2 b, int alpha, float fade_w = 0);   // fade_w: the right edge fades over it
void look_square(ImDrawList *dl, ImVec2 a, float side, ImU32 col);
struct LookStyle {   // the game's colours and flat corners for ImGui widgets, and its font, until destroyed
    LookStyle();
    ~LookStyle();
};
#define LOOK_TEXT IM_COL32(246, 241, 224, 255)
#define LOOK_DIM IM_COL32(190, 186, 168, 255)
#define LOOK_GOLD IM_COL32(255, 226, 140, 255)

// The wheel (ui/online_wheel.cpp)
bool online_wheel_pump(float w, float h);                          // present thread, once per frame; true while open
void online_wheel_layout(float w, float h, float scale);
void online_wheel_close(void);
bool online_wheel_open(void);
void online_wheel_demo(int what);                                  // scripted screenshots only
bool online_wheel_key_down(void);
void online_wheel_pad_chord_changed(void);
void online_connect_now(void);                                     // connect with the settings
void online_answer_ask(bool accept);                               // answer the oldest teleport or invite request                         // the settings' pad buttons changed

// An add-on's slice on the wheel's top ring (companions, later). draw_icon paints into a square of half-size r around
// c; activate runs after the wheel closed. visible is polled each time the wheel opens (NULL: always).
struct OnlineWheelSlice {
    const char *label;
    const char *detail;                                            // a line in the middle of the wheel when selected
    void (*draw_icon)(ImDrawList *dl, ImVec2 c, float r, ImU32 col);
    void (*activate)(void *user);
    bool (*visible)(void *user);
    void *user;
};
// Up to 2 extra slices; the pointer must stay valid. Returns 1 on success.
int online_wheel_register(const OnlineWheelSlice *slice);

#endif
