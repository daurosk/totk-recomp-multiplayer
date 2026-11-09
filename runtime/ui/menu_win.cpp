// Win32 adapter of the in-game menu: window messages to menu events (menu.h). The only Win32 code of the menu; the SDL3
// window has its own (ui/menu_sdl.cpp). Window thread.
//
// F1 opens and closes the menu, Esc opens it (with the menu open, Esc goes to the menu, which closes). Esc also releases
// the cursor (input_window_msg runs first; closing the menu gives the capture back). While the menu is open, its keys,
// the mouse and focus changes go to it; the game polls the keyboard on its own (input_win.c) and gets nothing while the
// menu is open (input_menu).
#include <windows.h>
#include <windowsx.h>

#include "imgui.h"

extern "C" {
#include "input.h"
#include "input_win.h"
#include "ui/menu.h"
#include "ui/online_ui.h"
}

static bool g_drop_char;   // the key the online UI took as a hotkey: its character is not typed anywhere
static unsigned g_high_surrogate;

static ImGuiKey key_of(WPARAM vk) {
    switch (vk) {
    case VK_UP: return ImGuiKey_UpArrow;
    case VK_DOWN: return ImGuiKey_DownArrow;
    case VK_LEFT: return ImGuiKey_LeftArrow;
    case VK_RIGHT: return ImGuiKey_RightArrow;
    case VK_RETURN: return ImGuiKey_Enter;
    case VK_SPACE: return ImGuiKey_Space;
    case VK_ESCAPE: return ImGuiKey_Escape;
    case VK_TAB: return ImGuiKey_Tab;
    case VK_BACK: return ImGuiKey_Backspace;
    case VK_DELETE: return ImGuiKey_Delete;   // clears a cell of the remapping rows
    case VK_HOME: return ImGuiKey_Home;
    case VK_END: return ImGuiKey_End;
    case VK_PRIOR: return ImGuiKey_PageUp;
    case VK_NEXT: return ImGuiKey_PageDown;
    case VK_SHIFT: return ImGuiKey_LeftShift;
    case VK_CONTROL: return ImGuiKey_LeftCtrl;
    case VK_INSERT: return ImGuiKey_Insert;
    case VK_MENU: return ImGuiKey_LeftAlt;
    case VK_CAPITAL: return ImGuiKey_CapsLock;
    case VK_OEM_3: return ImGuiKey_GraveAccent;
    case VK_OEM_2: return ImGuiKey_Slash;
    case VK_OEM_5: return ImGuiKey_Backslash;
    case VK_OEM_4: return ImGuiKey_LeftBracket;
    case VK_OEM_6: return ImGuiKey_RightBracket;
    case VK_OEM_1: return ImGuiKey_Semicolon;
    case VK_OEM_7: return ImGuiKey_Apostrophe;
    case VK_OEM_COMMA: return ImGuiKey_Comma;
    case VK_OEM_PERIOD: return ImGuiKey_Period;
    case VK_OEM_MINUS: return ImGuiKey_Minus;
    case VK_OEM_PLUS: return ImGuiKey_Equal;
    default:
        if (vk >= 'A' && vk <= 'Z') return (ImGuiKey)(ImGuiKey_A + (vk - 'A'));   // Q and E also switch menu tabs
        if (vk >= '0' && vk <= '9') return (ImGuiKey)(ImGuiKey_0 + (vk - '0'));
        if (vk >= VK_F2 && vk <= VK_F12 && vk != VK_F10 && vk != VK_F11) return (ImGuiKey)(ImGuiKey_F1 + (vk - VK_F1));
        return ImGuiKey_None;
    }
}

extern "C" int menu_window_msg(HWND h, UINT m, WPARAM w, LPARAM l, LRESULT *res) {
    (void)h;
    switch (m) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        // bit 30: auto-repeat
        // Esc with the menu open (opened by F1 too) is not a toggle here: it goes to the menu, which closes itself
        if (!(l & (1 << 30)) && (w == VK_F1 || (w == VK_ESCAPE && !menu_wants_input()))) {
            menu_toggle();
            *res = 0;
            return 1;
        }
        if (w == VK_F10) {   // also keeps F10 from opening the window menu
            if (!(l & (1 << 30))) menu_perf_cycle();
            *res = 0;
            return 1;
        }
        [[fallthrough]];
    case WM_KEYUP:
    case WM_SYSKEYUP: {
        ImGuiKey k = key_of(w);
        bool down = m == WM_KEYDOWN || m == WM_SYSKEYDOWN;
        // the online UI's hotkeys (chat, wheel, list, answers) also work with the menu closed; a key it takes has no
        // character for a text field (the WM_CHAR that follows is dropped)
        if (k && !(l & (1 << 30)) && online_hotkey(k, down) && down) g_drop_char = true;
        if (!menu_wants_input()) return 0;
        if (k) menu_event_key(k, down);
        return 0;   // F11 and the system keys keep working
    }
    case WM_CHAR:
        if (g_drop_char) {
            g_drop_char = false;
            return 0;
        }
        if (!menu_wants_input() || w < 32 || w == 127) return 0;
        if (w >= 0xD800 && w < 0xDC00) {   // the first half of a character outside the BMP
            g_high_surrogate = (unsigned)w;
        } else if (w >= 0xDC00 && w < 0xE000) {
            if (g_high_surrogate) menu_event_char(0x10000 + ((g_high_surrogate - 0xD800) << 10) + ((unsigned)w - 0xDC00));
            g_high_surrogate = 0;
        } else {
            menu_event_char((unsigned)w);
        }
        return 0;
    case WM_MOUSEMOVE:
        if (menu_wants_input()) menu_event_mouse_pos((float)GET_X_LPARAM(l), (float)GET_Y_LPARAM(l));
        return 0;
    case WM_LBUTTONDOWN: case WM_LBUTTONUP:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: {
        if (!menu_wants_input()) return 0;
        int b = m == WM_LBUTTONDOWN || m == WM_LBUTTONUP ? 0 : m == WM_RBUTTONDOWN || m == WM_RBUTTONUP ? 1 : 2;
        int down = m == WM_LBUTTONDOWN || m == WM_RBUTTONDOWN || m == WM_MBUTTONDOWN;
        menu_event_mouse_pos((float)GET_X_LPARAM(l), (float)GET_Y_LPARAM(l));
        menu_event_mouse_button(b, down);
        return 0;
    }
    case WM_MOUSEWHEEL:
        if (menu_wants_input()) menu_event_wheel(0, (float)GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA);
        return 0;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        if (m == WM_KILLFOCUS) online_focus_lost();
        menu_event_focus(m == WM_SETFOCUS);
        return 0;
    }
    return 0;
}
