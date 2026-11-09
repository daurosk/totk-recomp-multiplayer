// The update check at start: totk-update.exe (installer/updater/update.c, docs/updater.md), which the setup puts beside
// totk.exe, asks GitHub for a newer release with a short timeout and shows the update prompt. It exits 0 for the game
// to start (no newer release, no network, Later, Skip this version, update_check = 0) and EXIT_UPDATING when it
// started the update or one is already running: the game then exits, and the updater starts it again when it is done.
// Before that it restores an install whose last update was cut off. main.c calls this for a normal start only: not
// for the tool modes (--prepare-shaders and the probes), a scripted run (--seconds) or a restart from the settings menu.
// Nor when the updater itself starts the game after an update, done or refused (TOTK_UPDATER_STARTED, set by
// update.c start_game): asking again would show a refused release's prompt again, in a loop.
#include "update_start.h"

#include "host.h"
#include "platform/platform.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wchar.h>

#define EXIT_UPDATING 10

void update_at_start(void) {
    wchar_t mark[4];
    if (GetEnvironmentVariableW(L"TOTK_UPDATER_STARTED", mark, 4)) {
        SetEnvironmentVariableW(L"TOTK_UPDATER_STARTED", NULL);   // a later restart of this game checks as usual
        rt_log("update: started by the updater after an update; no check");
        return;
    }
    wchar_t dir[MAX_PATH * 2], exe[MAX_PATH * 2 + 32], cmd[MAX_PATH * 6];
    DWORD n = GetModuleFileNameW(NULL, dir, MAX_PATH * 2);
    if (!n || n >= MAX_PATH * 2) return;
    while (n > 0 && dir[n - 1] != L'\\') n--;
    if (!n) return;
    dir[n - 1] = 0;
    swprintf(exe, sizeof exe / sizeof *exe, L"%ls\\totk-update.exe", dir);
    if (GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES) return;
    // the folder never ends in a backslash here, so the quotes need no escaping
    swprintf(cmd, sizeof cmd / sizeof *cmd, L"\"%ls\" --check \"%ls\" --pid %lu", exe, dir, GetCurrentProcessId());
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) {
        rt_log("update: totk-update.exe did not start (error %lu)", GetLastError());
        return;
    }
    CloseHandle(pi.hThread);
    // The updater bounds its own network wait (CHECK_TIMEOUT_MS); a longer wait is the player reading the prompt.
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    if (code == EXIT_UPDATING) {
        rt_log("update: the updater runs; the game exits and starts again after the update");
        prof_write();   // plat_exit runs no atexit handlers
        plat_exit(0);
    }
}
#else
void update_at_start(void) {}
#endif
