/* A stand-in for a release's python\python.exe in tests/updater_test.py: it answers `python.exe -m installer --dest
   DIR --root ROOT [--base F --update F --keys F --firmware D] [--dlss]` the way the setup's command line does
   (installer/cli.py, installer/events.py: one JSON event per line, exit 0 done, 1 failed) and changes the install
   folder the way an update does, without game files or a compiler:

     compile   ROOT\payload\totk.exe replaces DIR\totk.exe (through a temp name, as installer/steps.py replace_file);
               DIR\romfs-index.bin is rewritten
     prepare   DIR\romfs-patches is removed and written again, DIR\romfs-patches-new\ is new, DIR\pipeline-cache.bin is
               rewritten in place (as totk.exe does), DIR\shader-cache\new.spv is added
     cleanup   DIR\totk-update.exe from ROOT\bin, DIR\build-cache\rebuild.txt naming ROOT, DIR\totk-install.json with
               ROOT\MP_VERSION as the release

   ROOT\fake-setup.txt steers it, one word and a value per line: "delay_ms N" between events, "fail_after STEP" (an
   error event with id compile.failed after that step, exit 1), "hang_after STEP" (sleeps until killed),
   "need_inputs 1" (without --base: the error check.inputs_needed, exit 1). The arguments it got are appended to
   ROOT\fake-setup-args.txt. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static const char *steps[] = {"check", "fonts", "extract", "lift", "compile", "prepare", "dlss", "cleanup"};
static wchar_t dest[4096], root[4096];
static int delay_ms = 150, need_inputs, have_base;
static char fail_after[32], hang_after[32];

static void event(const char *json) {
    printf("%s\n", json);
    fflush(stdout);
}

static void path(wchar_t *out, const wchar_t *dir, const wchar_t *name) { swprintf(out, 4096, L"%ls\\%ls", dir, name); }

static void put_wide(FILE *f, const wchar_t *w) {   /* as UTF-8, like the setup's own files */
    char u[16384];
    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, u, sizeof u, NULL, NULL)) fputs(u, f);
}

static int write_text(const wchar_t *p, const char *text) {
    FILE *f = _wfopen(p, L"wb");
    if (!f) return 0;
    fputs(text, f);
    return fclose(f) == 0;
}

static int replace_file(const wchar_t *src, const wchar_t *dst) {
    wchar_t tmp[4096];
    swprintf(tmp, 4096, L"%ls.tmp", dst);
    return CopyFileW(src, tmp, FALSE) && MoveFileExW(tmp, dst, MOVEFILE_REPLACE_EXISTING);
}

static void config(void) {
    wchar_t p[4096];
    path(p, root, L"fake-setup.txt");
    FILE *f = _wfopen(p, L"rb");
    char key[64], value[64];
    if (!f) return;
    while (fscanf(f, "%63s %63s", key, value) == 2) {
        if (!strcmp(key, "delay_ms")) delay_ms = atoi(value);
        else if (!strcmp(key, "fail_after")) snprintf(fail_after, sizeof fail_after, "%s", value);
        else if (!strcmp(key, "hang_after")) snprintf(hang_after, sizeof hang_after, "%s", value);
        else if (!strcmp(key, "need_inputs")) need_inputs = atoi(value);
    }
    fclose(f);
}

static void do_step(const char *step) {
    wchar_t a[4096], b[4096], dir[4096];
    if (!strcmp(step, "compile")) {
        path(a, root, L"payload\\totk.exe");
        path(b, dest, L"totk.exe");
        replace_file(a, b);
        path(b, dest, L"romfs-index.bin");
        write_text(b, "romfs index of the new version\n");
    } else if (!strcmp(step, "prepare")) {
        path(dir, dest, L"romfs-patches");
        path(a, dir, L"manifest.tsv");
        DeleteFileW(a);
        RemoveDirectoryW(dir);
        CreateDirectoryW(dir, NULL);
        write_text(a, "patches of the new version\n");
        path(dir, dest, L"romfs-patches-new");
        CreateDirectoryW(dir, NULL);
        path(a, dir, L"x.txt");
        write_text(a, "new\n");
        path(a, dest, L"pipeline-cache.bin");
        FILE *f = _wfopen(a, L"r+b");   /* in place: the same file, new bytes */
        if (f) fputs("PIPELINE CACHE OF THE NEW VERSION", f), fclose(f);
        path(a, dest, L"shader-cache\\new.spv");
        write_text(a, "a translation of the new translator\n");
    } else if (!strcmp(step, "cleanup")) {
        path(a, root, L"bin\\totk-update.exe");
        path(b, dest, L"totk-update.exe");
        replace_file(a, b);
        path(a, root, L"MP_VERSION");
        FILE *f = _wfopen(a, L"rb");
        char version[32] = "?";
        if (f) fscanf(f, "%31s", version), fclose(f);
        char json[256];
        snprintf(json, sizeof json, "{\"format\": 1, \"product\": \"totk-recomp-multiplayer\", \"release\": \"%s\", "
                                    "\"steps\": {}}\n", version);
        path(b, dest, L"totk-install.json");
        write_text(b, json);
        path(b, dest, L"build-cache\\rebuild.txt");
        FILE *r = _wfopen(b, L"wb");
        if (r) {
            fputs("python\n-c\nboot\n", r), put_wide(r, root), fputs("\n--rebuild\n", r), put_wide(r, dest);
            fputs("\n", r), fclose(r);
        }
    }
}

int wmain(int argc, wchar_t **argv) {
    for (int i = 1; i < argc; i++) {
        if (!wcscmp(argv[i], L"--dest") && i + 1 < argc) swprintf(dest, 4096, L"%ls", argv[++i]);
        else if (!wcscmp(argv[i], L"--root") && i + 1 < argc) swprintf(root, 4096, L"%ls", argv[++i]);
        else if (!wcscmp(argv[i], L"--base")) have_base = 1;
    }
    if (!dest[0] || !root[0]) {
        event("{\"type\": \"error\", \"id\": \"cli.usage\", \"params\": {\"detail\": \"--dest and --root\"}}");
        return 2;
    }
    config();
    wchar_t args[4096];
    path(args, root, L"fake-setup-args.txt");
    FILE *f = _wfopen(args, L"ab");
    if (f) {
        for (int i = 1; i < argc; i++) fputs(i > 1 ? " " : "", f), put_wide(f, argv[i]);
        fputs("\n", f);
        fclose(f);
    }
    event("{\"type\": \"start\", \"release\": \"fake\", \"mode\": \"update\", \"steps\": [\"check\", \"fonts\", "
          "\"extract\", \"lift\", \"compile\", \"prepare\", \"dlss\", \"cleanup\"]}");
    if (need_inputs && !have_base) {
        event("{\"type\": \"error\", \"id\": \"check.inputs_needed\", \"params\": {}, \"step\": \"check\"}");
        event("{\"type\": \"end\", \"result\": \"failed\", \"seconds\": 0}");
        return 1;
    }
    char line[512];
    for (int s = 0; s < 8; s++) {
        snprintf(line, sizeof line, "{\"type\": \"step\", \"step\": \"%s\", \"state\": \"started\"}", steps[s]);
        event(line);
        for (int k = 1; k <= 4; k++) {
            snprintf(line, sizeof line, "{\"type\": \"progress\", \"step\": \"%s\", \"phase\": \"x\", \"done\": %d, "
                                        "\"total\": 4, \"unit\": \"objects\"}", steps[s], k);
            event(line);
            Sleep((DWORD)delay_ms);
        }
        do_step(steps[s]);
        snprintf(line, sizeof line, "{\"type\": \"step\", \"step\": \"%s\", \"state\": \"done\", \"seconds\": 1}", steps[s]);
        event(line);
        if (!strcmp(steps[s], hang_after)) Sleep(INFINITE);
        if (!strcmp(steps[s], fail_after)) {
            event("{\"type\": \"error\", \"id\": \"compile.failed\", \"params\": {\"code\": 3, \"log\": "
                  "\"C:/fake/compile.log\"}, \"step\": \"compile\"}");
            event("{\"type\": \"end\", \"result\": \"failed\", \"seconds\": 1}");
            return 1;
        }
    }
    event("{\"type\": \"end\", \"result\": \"ok\", \"seconds\": 2}");
    return 0;
}
