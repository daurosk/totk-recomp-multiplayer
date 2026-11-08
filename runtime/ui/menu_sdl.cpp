// SDL3 adapter of the in-game menu: the window's events to menu events (menu.h), as ui/menu_win.cpp does for Win32.
// Window thread (gfx_window_sdl.c's event loop, after input_sdl_event).
//
// F1 opens and closes the menu, Esc opens it (with the menu open, Esc goes to the menu, which closes). Esc also releases
// the cursor (input_sdl.c sees the event first; closing the menu gives the capture back). While the menu is open, its
// keys, the mouse and focus changes go to it; the game reads the keyboard through input_sdl.c and gets nothing while
// the menu is open (input_menu). Mouse positions come in window units and go to the menu in pixels.
#include <SDL3/SDL.h>

#include "imgui.h"

extern "C" {
#include "input.h"
#include "input_sdl.h"
#include "ui/menu.h"
#include "ui/online_ui.h"
}

static bool g_drop_char;   // the key the online UI took as a hotkey: its text is not typed anywhere
static bool g_text_on;     // SDL text input started for the window

static ImGuiKey key_of(SDL_Scancode sc) {
    switch (sc) {
    case SDL_SCANCODE_UP: return ImGuiKey_UpArrow;
    case SDL_SCANCODE_DOWN: return ImGuiKey_DownArrow;
    case SDL_SCANCODE_LEFT: return ImGuiKey_LeftArrow;
    case SDL_SCANCODE_RIGHT: return ImGuiKey_RightArrow;
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return ImGuiKey_Enter;
    case SDL_SCANCODE_SPACE: return ImGuiKey_Space;
    case SDL_SCANCODE_ESCAPE: return ImGuiKey_Escape;
    case SDL_SCANCODE_TAB: return ImGuiKey_Tab;
    case SDL_SCANCODE_BACKSPACE: return ImGuiKey_Backspace;
    case SDL_SCANCODE_DELETE: return ImGuiKey_Delete;   // clears a cell of the remapping rows
    case SDL_SCANCODE_HOME: return ImGuiKey_Home;
    case SDL_SCANCODE_END: return ImGuiKey_End;
    case SDL_SCANCODE_PAGEUP: return ImGuiKey_PageUp;
    case SDL_SCANCODE_PAGEDOWN: return ImGuiKey_PageDown;
    case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT: return ImGuiKey_LeftShift;
    case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: return ImGuiKey_LeftCtrl;
    case SDL_SCANCODE_INSERT: return ImGuiKey_Insert;
    case SDL_SCANCODE_LALT: return ImGuiKey_LeftAlt;
    case SDL_SCANCODE_CAPSLOCK: return ImGuiKey_CapsLock;
    case SDL_SCANCODE_GRAVE: return ImGuiKey_GraveAccent;
    case SDL_SCANCODE_SLASH: return ImGuiKey_Slash;
    case SDL_SCANCODE_BACKSLASH: return ImGuiKey_Backslash;
    case SDL_SCANCODE_LEFTBRACKET: return ImGuiKey_LeftBracket;
    case SDL_SCANCODE_RIGHTBRACKET: return ImGuiKey_RightBracket;
    case SDL_SCANCODE_SEMICOLON: return ImGuiKey_Semicolon;
    case SDL_SCANCODE_APOSTROPHE: return ImGuiKey_Apostrophe;
    case SDL_SCANCODE_COMMA: return ImGuiKey_Comma;
    case SDL_SCANCODE_PERIOD: return ImGuiKey_Period;
    case SDL_SCANCODE_MINUS: return ImGuiKey_Minus;
    case SDL_SCANCODE_EQUALS: return ImGuiKey_Equal;
    default:
        if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) return (ImGuiKey)(ImGuiKey_A + (sc - SDL_SCANCODE_A));   // Q and E also switch menu tabs
        if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return (ImGuiKey)(ImGuiKey_1 + (sc - SDL_SCANCODE_1));
        if (sc == SDL_SCANCODE_0) return ImGuiKey_0;
        if (sc >= SDL_SCANCODE_F2 && sc <= SDL_SCANCODE_F12 && sc != SDL_SCANCODE_F10 && sc != SDL_SCANCODE_F11)
            return (ImGuiKey)(ImGuiKey_F1 + (sc - SDL_SCANCODE_F1));
        return ImGuiKey_None;
    }
}

extern "C" void menu_sdl_event(const SDL_Event *e) {
    float d = (float)gfx_window_sdl_density();
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN:
        // Esc with the menu open (opened by F1 too) is not a toggle here: it goes to the menu, which closes itself
        if (!e->key.repeat && (e->key.scancode == SDL_SCANCODE_F1 ||
                               (e->key.scancode == SDL_SCANCODE_ESCAPE && !menu_wants_input()))) {
            menu_toggle();
            return;
        }
        if (e->key.scancode == SDL_SCANCODE_F10) {
            if (!e->key.repeat) menu_perf_cycle();
            return;
        }
        [[fallthrough]];
    case SDL_EVENT_KEY_UP: {
        ImGuiKey k = key_of(e->key.scancode);
        bool down = e->type == SDL_EVENT_KEY_DOWN;
        if (k && !e->key.repeat && online_hotkey(k, down) && down) g_drop_char = true;
        bool want = menu_wants_input() != 0;
        SDL_Window *win = SDL_GetWindowFromID(e->key.windowID);
        if (win && want != g_text_on) {   // text events only while a text field may take them
            if (want) SDL_StartTextInput(win);
            else SDL_StopTextInput(win);
            g_text_on = want;
        }
        if (!want) return;
        if (k) menu_event_key(k, down);
        return;
    }
    case SDL_EVENT_TEXT_INPUT: {
        if (g_drop_char) {
            g_drop_char = false;
            return;
        }
        if (!menu_wants_input()) return;
        for (const unsigned char *p = (const unsigned char *)e->text.text; *p;) {   // UTF-8 to code points
            unsigned c = *p, n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
            c = n == 1 ? c : c & (0xFF >> (n + 1));
            for (unsigned i = 1; i < n && p[i]; i++) c = c << 6 | (p[i] & 0x3F);
            p += n;
            if (c >= 32 && c != 127) menu_event_char(c);
        }
        return;
    }
    case SDL_EVENT_MOUSE_MOTION:
        if (menu_wants_input()) menu_event_mouse_pos(e->motion.x * d, e->motion.y * d);
        return;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (!menu_wants_input()) return;
        int b = e->button.button == SDL_BUTTON_LEFT ? 0 : e->button.button == SDL_BUTTON_RIGHT ? 1
              : e->button.button == SDL_BUTTON_MIDDLE ? 2 : -1;
        if (b < 0) return;
        menu_event_mouse_pos(e->button.x * d, e->button.y * d);
        menu_event_mouse_button(b, e->type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        return;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        if (menu_wants_input()) menu_event_wheel(e->wheel.x, e->wheel.y);
        return;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        if (e->type == SDL_EVENT_WINDOW_FOCUS_LOST) online_focus_lost();
        menu_event_focus(e->type == SDL_EVENT_WINDOW_FOCUS_GAINED);
        return;
    }
}
