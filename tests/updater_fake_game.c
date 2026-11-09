/* A stand-in for totk.exe in tests/updater_test.py, linked with the game's own runtime/update_start.c: it runs the
   update check the way the game does at start and, when the game would go on, appends "started <FAKE_VERSION>" to
   game-starts.txt beside it and exits. FAKE_VERSION comes from the compiler's command line. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdarg.h>
#include <stdio.h>
#include <wchar.h>

#include "update_start.h"

#ifndef FAKE_VERSION
#define FAKE_VERSION "0.0.0"
#endif

static FILE *open_beside(const wchar_t *name) {
    wchar_t p[4096];
    DWORD n = GetModuleFileNameW(NULL, p, 4096);
    while (n && p[n - 1] != L'\\') n--;
    p[n] = 0;
    wcscat(p, name);
    return _wfopen(p, L"ab");
}

/* the runtime functions update_start.c calls */
void rt_log(const char *fmt, ...) {
    FILE *f = open_beside(L"game-log.txt");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}
void prof_write(void) {}
_Noreturn void plat_exit(int code) { ExitProcess((UINT)code); }

int WINAPI WinMain(HINSTANCE a, HINSTANCE b, LPSTR c, int d) {
    (void)a, (void)b, (void)c, (void)d;
    update_at_start();
    FILE *f = open_beside(L"game-starts.txt");
    if (f) fprintf(f, "started %s\n", FAKE_VERSION), fclose(f);
    return 0;
}
