/* totk-update.exe: the game's updater (docs/updater.md). It sits beside totk.exe in the install folder; the setup's
   cleanup step copies it there (installer/steps.py, Cleanup).

     totk-update.exe --check DIR --pid PID     totk.exe runs this at start and waits (runtime/update_start.c)
     totk-update.exe --apply DIR --pid PID     the update itself, started by --check from a copy in DIR\update\run
     totk-update.exe --selftest                checks of the parts below that need no network

   --check finishes an update that was cut off (power loss, a killed updater) by restoring the files it had saved,
   then, unless update_check = 0, asks the GitHub release API for the latest release in a background thread and gives
   up after CHECK_TIMEOUT_MS. No network, any error, or no newer release: exit 0 and the game starts. A newer release
   that carries the signed update manifest and is not the update_skip version opens the prompt: Update, Later, Skip
   this version. Update starts --apply and exits EXIT_UPDATING, and totk.exe exits.

   --apply shows a progress window and:
     1. downloads the manifest (TotKMP-Update-windows.json) and its signature, checks the Ed25519 signature against
        the key built in (key.h), that the manifest is for this product and Windows, and that its version is the
        release's and newer than this updater's own (TOTKMP_VERSION, the version of the install);
     2. downloads the package (the release's setup zip) and checks its size and SHA-256 against the manifest;
     3. unpacks it with Windows' own tar.exe into DIR\update\stage and checks every file against the manifest: same
        size, same SHA-256, nothing missing, nothing extra, no link; then moves it to DIR\update\setup-<version>;
     4. saves the install folder's files (DIR\update\backup: top-level files and build-cache\ copied, the other folders
        as hard links, which cost no space) except profile\, mods\ and logs\, which the setup never writes, and marks
        the journal (DIR\update\journal.txt);
     5. runs the new setup's command line on the install folder (python\python.exe -m installer --dest DIR), which
        keeps the game data, saves and settings and rebuilds totk.exe and the shaders only when what they are built
        from changed (docs/installer.md, "Updating the port"). When it needs the game files again (check.inputs_needed)
        it runs once more with the ones the setup remembered (%LOCALAPPDATA%\TotK Recomp Multiplayer\setup.json);
     6. on success removes the backup and the older setup folders and starts the new game; on any failure after step 4
        puts every saved file back, removes what the setup added, and starts the old game with a message.
   Until step 4 nothing in the install folder changes and Cancel stops the update. The setup's process runs in a job
   that ends with this process, so a killed updater never leaves a setup writing into the folder; the next start's
   --check then restores the backup.

   The updater trusts only the signature: the release page, its notes and the download addresses are unauthenticated
   and only shown or followed. Plain http is allowed only for a loopback address given in update_url (the tests). */
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <bcrypt.h>
#include <commctrl.h>
#include <shellapi.h>
#include <winhttp.h>

#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "json.h"
#include "key.h"
#include "monocypher-ed25519.h"

#ifndef TOTKMP_VERSION
#define TOTKMP_VERSION "0.0.0"   /* make_release.py and the tests pass MP_VERSION */
#endif

#define PRODUCT_ID "totk-recomp-multiplayer"
#define PRODUCT_NAME L"TotK Recomp Multiplayer"
#define API_URL "https://api.github.com/repos/daurosk/totk-recomp-multiplayer/releases/latest"
#define MANIFEST_NAME "TotKMP-Update-windows.json"
#define SIGNATURE_NAME MANIFEST_NAME ".sig"
#define USER_AGENT L"TotK-Recomp-Multiplayer-Updater/" TOTKMP_VERSION_W
#define WIDEN2(s) L##s
#define WIDEN(s) WIDEN2(s)
#define TOTKMP_VERSION_W WIDEN(TOTKMP_VERSION)

#define EXIT_CONTINUE 0    /* the game starts */
#define EXIT_UPDATING 10   /* an updater runs: the game exits */
#define CHECK_TIMEOUT_MS 4000
#define MAX_API_BYTES (4u << 20)
#define MAX_MANIFEST_BYTES (64u << 20)
#define MAX_FILES 200000
#define PATHW 32768

/* ---- state ------------------------------------------------------------------------------------------------- */

static wchar_t g_dir[PATHW];        /* the install folder, absolute, no trailing backslash */
static wchar_t g_upd[PATHW];        /* g_dir\update */
static int g_allow_http;            /* update_url names a loopback http address */
static char g_api_url[2048];
static FILE *g_log;
static BCRYPT_ALG_HANDLE g_sha_alg;

/* ---- text helpers ------------------------------------------------------------------------------------------ */

static wchar_t *wide(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = (wchar_t *)malloc((size_t)(n > 0 ? n : 1) * sizeof *w);
    if (!w) abort();
    if (n <= 0 || !MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n)) w[0] = 0;
    return w;
}

static char *utf8(const wchar_t *s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
    char *u = (char *)malloc((size_t)(n > 0 ? n : 1));
    if (!u) abort();
    if (n <= 0 || !WideCharToMultiByte(CP_UTF8, 0, s, -1, u, n, NULL, NULL)) u[0] = 0;
    return u;
}

static void logf_(const char *fmt, ...) {
    if (!g_log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "%04d-%02d-%02d %02d:%02d:%02d [%lu] ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
            GetCurrentProcessId());
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}
#define LOG(...) logf_(__VA_ARGS__)

static void log_open(void) {
    wchar_t path[PATHW];
    CreateDirectoryW(g_upd, NULL);
    swprintf(path, PATHW, L"%ls\\update.log", g_upd);
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (GetFileAttributesExW(path, GetFileExInfoStandard, &a) && a.nFileSizeLow > (4u << 20) && !a.nFileSizeHigh) {
        wchar_t old[PATHW];
        swprintf(old, PATHW, L"%ls\\update.1.log", g_upd);
        MoveFileExW(path, old, MOVEFILE_REPLACE_EXISTING);
    }
    g_log = _wfopen(path, L"ab");
}

static void copy_str(char *out, size_t cap, const char *s) {
    snprintf(out, cap, "%s", s ? s : "");
}

static int is_hex(const char *s, size_t n) {
    if (!s || strlen(s) != n) return 0;
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return 0;
    return 1;
}

static int unhex(const char *s, uint8_t *out, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        if (sscanf(s + 2 * i, "%2x", &v) != 1) return 0;
        out[i] = (uint8_t)v;
    }
    return 1;
}

static void hex(const uint8_t *b, size_t n, char *out) {
    for (size_t i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", b[i]);
}

/* ---- versions ---------------------------------------------------------------------------------------------- */

/* "0.1.2" or "v0.1.2": one to four numbers joined by dots, each at most six digits. 0 for anything else (a
   pre-release tag such as 0.2.0-rc1 is never offered). */
static int parse_version(const char *s, int v[4]) {
    int n = 0;
    if (!s) return 0;
    if (*s == 'v' || *s == 'V') s++;
    memset(v, 0, 4 * sizeof *v);
    for (;;) {
        int digits = 0, x = 0;
        while (*s >= '0' && *s <= '9') {
            if (++digits > 6) return 0;
            x = x * 10 + (*s++ - '0');
        }
        if (!digits || n == 4) return 0;
        v[n++] = x;
        if (!*s) return n;
        if (*s++ != '.') return 0;
    }
}

/* <0, 0, >0 as a is older, the same or newer than b; -2 when either is not a version. */
static int compare_versions(const char *a, const char *b) {
    int x[4], y[4];
    if (!parse_version(a, x) || !parse_version(b, y)) return -2;
    for (int i = 0; i < 4; i++)
        if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    return 0;
}

static const char *bare_version(const char *tag) {
    return tag && (*tag == 'v' || *tag == 'V') ? tag + 1 : tag;
}

/* ---- paths and files --------------------------------------------------------------------------------------- */

/* p with the \\?\ prefix, so the file calls below take paths longer than MAX_PATH (a setup's deepest file under a
   long install folder). p is absolute. */
static const wchar_t *longp(const wchar_t *p, wchar_t *buf) {
    if (!wcsncmp(p, L"\\\\?\\", 4)) swprintf(buf, PATHW, L"%ls", p);
    else if (p[0] == L'\\' && p[1] == L'\\') swprintf(buf, PATHW, L"\\\\?\\UNC\\%ls", p + 2);
    else swprintf(buf, PATHW, L"\\\\?\\%ls", p);
    return buf;
}

static DWORD attrs(const wchar_t *p) {
    wchar_t b[PATHW];
    return GetFileAttributesW(longp(p, b));
}

static int exists(const wchar_t *p) { return attrs(p) != INVALID_FILE_ATTRIBUTES; }

static int make_dir(const wchar_t *p) {
    wchar_t b[PATHW];
    return CreateDirectoryW(longp(p, b), NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

static int move_path(const wchar_t *from, const wchar_t *to, int replace) {
    wchar_t a[PATHW], b[PATHW];
    return MoveFileExW(longp(from, a), longp(to, b), MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0));
}

static int copy_file(const wchar_t *from, const wchar_t *to) {
    wchar_t a[PATHW], b[PATHW];
    return CopyFileW(longp(from, a), longp(to, b), FALSE);
}

/* Deletes p and, for a folder, everything in it. A link (junction or symbolic link) is removed itself, never what
   it points to: a setup folder's temp\ is a junction to the shared temp folder. Read-only files are deleted too.
   1 when p is gone. */
static int delete_tree(const wchar_t *p) {
    wchar_t b[PATHW];
    DWORD a = GetFileAttributesW(longp(p, b));
    if (a == INVALID_FILE_ATTRIBUTES) return GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND;
    if (a & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(b, a & ~FILE_ATTRIBUTE_READONLY);
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) return DeleteFileW(b);
    if (!(a & FILE_ATTRIBUTE_REPARSE_POINT)) {
        wchar_t pat[PATHW], child[PATHW];
        WIN32_FIND_DATAW fd;
        swprintf(pat, PATHW, L"%ls\\*", b);
        HANDLE h = FindFirstFileExW(pat, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
                swprintf(child, PATHW, L"%ls\\%ls", p, fd.cFileName);
                delete_tree(child);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    return RemoveDirectoryW(b);
}

static char *read_file(const wchar_t *p, size_t max, size_t *len) {
    wchar_t b[PATHW];
    HANDLE h = CreateFileW(longp(p, b), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER size;
    char *data = NULL;
    DWORD got = 0;
    if (GetFileSizeEx(h, &size) && (unsigned long long)size.QuadPart <= max &&
        (data = (char *)malloc((size_t)size.QuadPart + 1)) &&
        (size.QuadPart == 0 || ReadFile(h, data, (DWORD)size.QuadPart, &got, NULL)) && got == (DWORD)size.QuadPart) {
        data[got] = 0;
        if (len) *len = got;
    } else {
        free(data);
        data = NULL;
    }
    CloseHandle(h);
    return data;
}

/* Writes data to p through p.tmp and a rename, flushed to disk first: p is either the old file or the new one. */
static int write_file(const wchar_t *p, const char *data, size_t len) {
    wchar_t tmp[PATHW], b[PATHW];
    swprintf(tmp, PATHW, L"%ls.tmp", p);
    HANDLE h = CreateFileW(longp(tmp, b), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD put = 0;
    int ok = (len == 0 || WriteFile(h, data, (DWORD)len, &put, NULL)) && put == len && FlushFileBuffers(h);
    CloseHandle(h);
    return ok && move_path(tmp, p, 1);
}

/* ---- SHA-256 (Windows CNG) ----------------------------------------------------------------------------------- */

typedef struct { BCRYPT_HASH_HANDLE h; } Sha;

static int sha_begin(Sha *s) {
    if (!g_sha_alg && BCryptOpenAlgorithmProvider(&g_sha_alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0) return 0;
    return BCryptCreateHash(g_sha_alg, &s->h, NULL, 0, NULL, 0, 0) >= 0;
}
static void sha_add(Sha *s, const void *p, size_t n) {
    while (n) {
        ULONG k = n > (1u << 30) ? (1u << 30) : (ULONG)n;
        BCryptHashData(s->h, (PUCHAR)p, k, 0);
        p = (const char *)p + k, n -= k;
    }
}
static void sha_end(Sha *s, char out_hex[65]) {
    uint8_t d[32];
    BCryptFinishHash(s->h, d, 32, 0);
    BCryptDestroyHash(s->h);
    hex(d, 32, out_hex);
}

typedef void (*BytesProgress)(unsigned long long done, void *user);

static int file_sha256(const wchar_t *p, char out_hex[65], unsigned long long *size, BytesProgress cb, void *user) {
    wchar_t b[PATHW];
    HANDLE h = CreateFileW(longp(p, b), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    static __thread char *buf;
    if (!buf) buf = (char *)malloc(1 << 20);
    Sha s;
    unsigned long long total = 0;
    DWORD got;
    int ok = buf && sha_begin(&s);
    while (ok && ReadFile(h, buf, 1 << 20, &got, NULL) && got) {
        sha_add(&s, buf, got);
        total += got;
        if (cb) cb(got, user);
    }
    CloseHandle(h);
    if (!ok) return 0;
    sha_end(&s, out_hex);
    if (size) *size = total;
    return 1;
}

/* ---- totk.ini ---------------------------------------------------------------------------------------------- */
/* The game's rules (runtime/cfg.c): "key = value" lines, [sections] only group, a key may carry the TOTK_ prefix and
   is case-insensitive, the later line wins, a value may be in double quotes, and the environment (TOTK_<KEY>) wins
   over the file. */

static int key_matches(const char *line_key, size_t n, const char *key) {
    if (n > 5 && !_strnicmp(line_key, "totk_", 5)) line_key += 5, n -= 5;
    return n == strlen(key) && !_strnicmp(line_key, key, n);
}

/* The value of key (lower case, no prefix) into out; 0 when it is unset or empty. */
static int ini_get(const char *key, char *out, size_t cap) {
    char env[64];
    snprintf(env, sizeof env, "TOTK_%s", key);
    for (char *p = env; *p; p++) *p = (char)toupper((unsigned char)*p);
    wchar_t wenv[64], wval[2048], path[PATHW];
    MultiByteToWideChar(CP_UTF8, 0, env, -1, wenv, 64);
    DWORD n = GetEnvironmentVariableW(wenv, wval, 2048);
    if (n && n < 2048) {
        char *v = utf8(wval);
        copy_str(out, cap, v);
        free(v);
        return out[0] != 0;
    }
    swprintf(path, PATHW, L"%ls\\totk.ini", g_dir);
    char *text = read_file(path, 4u << 20, NULL);
    int found = 0;
    out[0] = 0;
    for (char *line = text; line && *line;) {
        char *end = strchr(line, '\n'), *next = end ? end + 1 : NULL;
        if (end) *end = 0;
        while (*line == ' ' || *line == '\t') line++;
        char *eq = strchr(line, '=');
        if (*line != '#' && *line != ';' && *line != '[' && eq) {
            char *ke = eq;
            while (ke > line && (ke[-1] == ' ' || ke[-1] == '\t')) ke--;
            if (key_matches(line, (size_t)(ke - line), key)) {
                char *v = eq + 1;
                while (*v == ' ' || *v == '\t') v++;
                size_t vl = strlen(v);
                while (vl && (v[vl - 1] == ' ' || v[vl - 1] == '\t' || v[vl - 1] == '\r')) v[--vl] = 0;
                if (vl >= 2 && v[0] == '"' && v[vl - 1] == '"') v[vl - 1] = 0, v++;
                copy_str(out, cap, v);
                found = out[0] != 0;
            }
        }
        line = next;
    }
    free(text);
    return found;
}

/* Sets key = value in totk.ini: replaces the key's line where it is, else adds it under [update] (a new section at
   the end when the file has none), keeping every other line and the file's line endings. */
static int ini_set_in(const wchar_t *path, const char *key, const char *value) {
    size_t len = 0;
    char *text = read_file(path, 4u << 20, &len);
    if (!text) text = calloc(1, 1), len = 0;
    const char *nl = strstr(text, "\r\n") || !len ? "\r\n" : "\n";
    size_t cap = len + strlen(key) + strlen(value) + 64;
    char *out = (char *)malloc(cap), *o = out;
    int done = 0;
    char *after_section = NULL;   /* where in out the line after "[update]" starts */
    for (char *line = text; line < text + len;) {
        char *end = memchr(line, '\n', (size_t)(text + len - line)), *next = end ? end + 1 : text + len;
        char *p = line;
        while (p < next && (*p == ' ' || *p == '\t')) p++;
        char *eq = memchr(p, '=', (size_t)(next - p));
        int replace = 0;
        if (!done && *p != '#' && *p != ';' && *p != '[' && eq) {
            char *ke = eq;
            while (ke > p && (ke[-1] == ' ' || ke[-1] == '\t')) ke--;
            replace = key_matches(p, (size_t)(ke - p), key);
        }
        if (replace) {
            o += sprintf(o, "%s = %s%s", key, value, nl);
            done = 1;
        } else {
            memcpy(o, line, (size_t)(next - line));
            o += next - line;
            if (!end) o += sprintf(o, "%s", nl);   /* a last line without its line break gets one */
            if (!_strnicmp(p, "[update]", 8)) after_section = o;
        }
        line = next;
    }
    if (!done && after_section) {
        size_t tail = (size_t)(o - after_section);
        char line[512];
        int n = snprintf(line, sizeof line, "%s = %s%s", key, value, nl);
        memmove(after_section + n, after_section, tail);
        memcpy(after_section, line, (size_t)n);
        o += n;
    } else if (!done) {
        o += sprintf(o, "%s[update]%s%s = %s%s", o > out ? nl : "", nl, key, value, nl);
    }
    int ok = write_file(path, out, (size_t)(o - out));
    free(out);
    free(text);
    return ok;
}

static int ini_set(const char *key, const char *value) {
    wchar_t path[PATHW];
    swprintf(path, PATHW, L"%ls\\totk.ini", g_dir);
    return ini_set_in(path, key, value);
}

/* ---- HTTP (WinHTTP) ---------------------------------------------------------------------------------------- */

static int loopback_http(const char *url) {
    const char *h = url + 7;
    return !_strnicmp(url, "http://", 7) &&
           (!strncmp(h, "127.0.0.1", 9) || !_strnicmp(h, "localhost", 9) || !strncmp(h, "[::1]", 5)) &&
           (h[h[0] == '[' ? 5 : 9] == ':' || h[h[0] == '[' ? 5 : 9] == '/' || !h[h[0] == '[' ? 5 : 9]);
}

typedef struct {
    HANDLE file;                 /* the body goes here when set, else into memory */
    size_t max;                  /* memory: the most bytes accepted */
    unsigned long long expect;   /* file: the exact size, 0 for any */
    Sha *sha;                    /* file: hashed while it is written */
    BytesProgress progress;
    void *user;
    volatile LONG *cancel;
} HttpTarget;

/* GET url (https, or plain http to a loopback address when update_url allows it). Redirects are followed (GitHub's
   download addresses redirect to its storage), never from https to http. The answer must be 200. Memory bodies end
   with a NUL. */
static int http_get(const char *url, DWORD timeout_ms, HttpTarget *t, char **body, size_t *len, char *err, size_t errcap) {
    int https = !_strnicmp(url, "https://", 8);
    if (!https && !(g_allow_http && loopback_http(url))) {
        snprintf(err, errcap, "refused address %.200s", url);
        return 0;
    }
    wchar_t *wurl = wide(url), host[256], path[4096];
    URL_COMPONENTSW uc = {sizeof uc};
    uc.lpszHostName = host, uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path, uc.dwUrlPathLength = 4096;
    wchar_t extra[4096];
    uc.lpszExtraInfo = extra, uc.dwExtraInfoLength = 4096;
    HINTERNET s = NULL, c = NULL, r = NULL;
    int ok = 0;
    char *mem = NULL;
    size_t n = 0;
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc)) {
        snprintf(err, errcap, "bad address %.200s", url);
        goto out;
    }
    if (uc.dwExtraInfoLength && wcslen(path) + wcslen(extra) < 4096) wcscat(path, extra);
    s = WinHttpOpen(USER_AGENT, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, NULL, NULL, 0);
    if (!s) s = WinHttpOpen(USER_AGENT, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!s) goto neterr;
    WinHttpSetTimeouts(s, (int)timeout_ms, (int)timeout_ms, (int)timeout_ms, (int)timeout_ms);
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | 0x2000 /* TLS 1.3 */;
    if (!WinHttpSetOption(s, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols)) {
        protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        WinHttpSetOption(s, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols);
    }
    c = WinHttpConnect(s, host, uc.nPort, 0);
    if (!c) goto neterr;
    r = WinHttpOpenRequest(c, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                           https ? WINHTTP_FLAG_SECURE : 0);
    if (!r) goto neterr;
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(r, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof policy);
    const wchar_t *headers = L"Accept: application/vnd.github+json, application/octet-stream;q=0.9, */*;q=0.5\r\n";
    if (!WinHttpSendRequest(r, headers, (DWORD)-1, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(r, NULL))
        goto neterr;
    DWORD status = 0, sl = sizeof status;
    WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &status, &sl, NULL);
    if (status != 200) {
        snprintf(err, errcap, "HTTP %lu from %.200s", status, url);
        goto out;
    }
    static char chunk[1 << 16];
    unsigned long long total = 0;
    for (;;) {
        DWORD got = 0;
        if (t && t->cancel && *t->cancel) {
            snprintf(err, errcap, "cancelled");
            goto out;
        }
        if (!WinHttpReadData(r, chunk, sizeof chunk, &got)) goto neterr;
        if (!got) break;
        total += got;
        if (t && t->file) {
            DWORD put = 0;
            if (t->expect && total > t->expect) {
                snprintf(err, errcap, "%.200s is larger than the manifest says", url);
                goto out;
            }
            if (!WriteFile(t->file, chunk, got, &put, NULL) || put != got) {
                snprintf(err, errcap, "cannot write the download (error %lu)", GetLastError());
                goto out;
            }
            if (t->sha) sha_add(t->sha, chunk, got);
        } else {
            size_t max = t ? t->max : MAX_API_BYTES;
            if (n + got > max) {
                snprintf(err, errcap, "%.200s is larger than %zu bytes", url, max);
                goto out;
            }
            char *grown = (char *)realloc(mem, n + got + 1);
            if (!grown) goto out;
            mem = grown;
            memcpy(mem + n, chunk, got);
            n += got;
        }
        if (t && t->progress) t->progress(got, t->user);
    }
    if (t && t->file && t->expect && total != t->expect) {
        snprintf(err, errcap, "%.200s ended after %llu of %llu bytes", url, total, t->expect);
        goto out;
    }
    if (!(t && t->file)) {
        if (!mem) mem = (char *)calloc(1, 1);
        mem[n] = 0;
        *body = mem, *len = n;
        mem = NULL;
    }
    ok = 1;
    goto out;
neterr:
    snprintf(err, errcap, "network error %lu for %.200s", GetLastError(), url);
out:
    free(mem);
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    free(wurl);
    return ok;
}

/* ---- the release ------------------------------------------------------------------------------------------- */

typedef struct {
    char version[32];             /* the tag without its v */
    char page[512];               /* html_url: the release page */
    char notes[1200];             /* the first lines of the release notes, plain text */
    char manifest_url[2048], sig_url[2048];
    unsigned long long package_size;   /* the setup zip's size from the asset list, for the prompt; 0 unknown */
    JsonNode *tree;               /* the whole answer: assets are looked up again for the package */
} Release;

static const JsonNode *find_asset(const JsonNode *tree, const char *name) {
    const JsonNode *assets = json_get(tree, "assets");
    for (const JsonNode *a = assets && assets->type == J_ARRAY ? assets->child : NULL; a; a = a->next) {
        const char *n = json_str(a, "name");
        if (n && !strcmp(n, name)) return a;
    }
    return NULL;
}

/* The release notes' first lines as plain text: headings, blank lines and list markers dropped, **bold**, `code`
   and [links](address) reduced to their words, at most five lines. */
static void notes_excerpt(const char *md, char *out, size_t cap) {
    size_t o = 0;
    int lines = 0;
    out[0] = 0;
    for (const char *line = md; line && *line && lines < 5;) {
        const char *end = strchr(line, '\n'), *next = end ? end + 1 : NULL;
        size_t n = end ? (size_t)(end - line) : strlen(line);
        while (n && (line[n - 1] == '\r' || line[n - 1] == ' ')) n--;
        const char *p = line, *e = line + n;
        while (p < e && (*p == ' ' || *p == '\t')) p++;
        if (p < e && *p != '#' && *p != '|' && *p != '>' && strncmp(p, "```", 3) && strncmp(p, "---", 3)) {
            if ((*p == '-' || *p == '*') && p + 1 < e && p[1] == ' ') {
                if (o + 3 < cap) out[o++] = (char)0xE2, out[o++] = (char)0x80, out[o++] = (char)0xA2;   /* a bullet */
                p++;
            }
            for (; p < e && o + 2 < cap; p++) {
                if (*p == '*' || *p == '`' || (*p == '_' && (p == line || p[-1] == ' ' || p + 1 == e || p[1] == ' '))) continue;
                if (*p == '[') continue;
                if (*p == ']' && p + 1 < e && p[1] == '(') {   /* skip the link's address */
                    const char *close = memchr(p, ')', (size_t)(e - p));
                    if (close) { p = close; continue; }
                }
                out[o++] = *p;
            }
            if (o + 2 < cap) out[o++] = '\n';
            lines++;
        }
        line = next;
    }
    while (o && out[o - 1] == '\n') o--;
    out[o] = 0;
}

/* The latest release (GitHub's releases/latest: never a draft or a pre-release). 0 with a reason in err. */
static int fetch_release(Release *rel, DWORD timeout_ms, char *err, size_t errcap) {
    char *body = NULL;
    size_t len = 0;
    memset(rel, 0, sizeof *rel);
    if (!http_get(g_api_url, timeout_ms, NULL, &body, &len, err, errcap)) return 0;
    rel->tree = json_parse(body, len);
    free(body);
    const char *tag = json_str(rel->tree, "tag_name");
    int v[4];
    if (!rel->tree || !tag || !parse_version(tag, v) || strlen(tag) >= sizeof rel->version + 1) {
        snprintf(err, errcap, "the release answer has no version tag");
        return 0;
    }
    if (json_true(rel->tree, "draft") || json_true(rel->tree, "prerelease")) {
        snprintf(err, errcap, "the release is a draft or a pre-release");
        return 0;
    }
    copy_str(rel->version, sizeof rel->version, bare_version(tag));
    const char *page = json_str(rel->tree, "html_url");
    if (page && !strncmp(page, "https://github.com/", 19)) copy_str(rel->page, sizeof rel->page, page);
    const char *notes = json_str(rel->tree, "body");
    if (notes) notes_excerpt(notes, rel->notes, sizeof rel->notes);
    const JsonNode *m = find_asset(rel->tree, MANIFEST_NAME), *s = find_asset(rel->tree, SIGNATURE_NAME);
    const char *mu = json_str(m, "browser_download_url"), *su = json_str(s, "browser_download_url");
    if (!mu || !su || strlen(mu) >= sizeof rel->manifest_url || strlen(su) >= sizeof rel->sig_url) {
        snprintf(err, errcap, "release %s carries no update manifest", rel->version);
        return 0;
    }
    copy_str(rel->manifest_url, sizeof rel->manifest_url, mu);
    copy_str(rel->sig_url, sizeof rel->sig_url, su);
    char zip[128];
    snprintf(zip, sizeof zip, "TotKMP-Setup-%s-windows.zip", rel->version);
    long long size = json_uint(find_asset(rel->tree, zip), "size");
    rel->package_size = size > 0 ? (unsigned long long)size : 0;
    return 1;
}

/* ---- the manifest ------------------------------------------------------------------------------------------ */

typedef struct {
    char *path;          /* below the package's root folder, with / */
    char *key;           /* path in lower case, for lookups (Windows names ignore case) */
    unsigned long long size;
    char sha[65];
    int seen;
} MFile;

typedef struct {
    JsonNode *tree;
    char version[32], root[128], package[256], package_sha[65];
    unsigned long long package_size, files_size;
    MFile *files;
    int nfiles;
} Manifest;

static const char *const devices[] = {"con", "prn", "aux", "nul", "com0", "com1", "com2", "com3", "com4", "com5", "com6",
                                      "com7", "com8", "com9", "lpt0", "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6",
                                      "lpt7", "lpt8", "lpt9"};

/* A relative path the updater writes: parts joined by /, none empty, "." or "..", none starting with a space or
   ending in a space or dot, no control character, none of \ : * ? " < > | and no device name (tools/make_update.py
   safe_rel_path is the same rule). */
static int safe_rel_path(const char *p) {
    if (!p || !*p || strlen(p) >= 400) return 0;
    for (const char *part = p; part;) {
        const char *slash = strchr(part, '/');
        size_t n = slash ? (size_t)(slash - part) : strlen(part);
        if (!n || part[0] == ' ' || part[n - 1] == ' ' || part[n - 1] == '.') return 0;
        for (size_t i = 0; i < n; i++) {
            unsigned char c = (unsigned char)part[i];
            if (c < 0x20 || c == 0x7f || strchr("\\:*?\"<>|", c)) return 0;
        }
        size_t stem = 0;
        while (stem < n && part[stem] != '.') stem++;
        while (stem && part[stem - 1] == ' ') stem--;
        for (size_t d = 0; d < sizeof devices / sizeof *devices; d++)
            if (stem == strlen(devices[d]) && !_strnicmp(part, devices[d], stem)) return 0;
        part = slash ? slash + 1 : NULL;
    }
    return 1;
}

static int mfile_cmp(const void *a, const void *b) {
    return strcmp(((const MFile *)a)->key, ((const MFile *)b)->key);
}

static void manifest_free(Manifest *m) {
    for (int i = 0; i < m->nfiles; i++) free(m->files[i].path), free(m->files[i].key);
    free(m->files);
    json_free(m->tree);
    memset(m, 0, sizeof *m);
}

/* Checks the signature of data (the manifest's exact bytes) and reads it. want_version: the release's version, which
   the signed text must name; it must also be newer than this install. 0 with the reason in err. */
static int manifest_load(Manifest *m, const char *data, size_t len, const char *sig_text, const char *want_version,
                         char *err, size_t errcap) {
    uint8_t sig[64], pub[32];
    char sig_hex[129] = "";
    memset(m, 0, sizeof *m);
    if (sig_text) sscanf(sig_text, "%128s", sig_hex);
    if (!is_hex(sig_hex, 128) || !unhex(sig_hex, sig, 64) || !is_hex(UPDATE_PUBLIC_KEY_HEX, 64) ||
        !unhex(UPDATE_PUBLIC_KEY_HEX, pub, 32) || crypto_ed25519_check(sig, pub, (const uint8_t *)data, len) != 0) {
        snprintf(err, errcap, "the update's signature is not valid");
        return 0;
    }
    m->tree = json_parse(data, len);
    const JsonNode *t = m->tree, *pkg = json_get(t, "package"), *files = json_get(t, "files");
    const char *product = json_str(t, "product"), *platform = json_str(t, "platform"),
               *version = json_str(t, "version"), *root = json_str(t, "root"),
               *pname = json_str(pkg, "name"), *psha = json_str(pkg, "sha256");
    long long psize = json_uint(pkg, "size");
    if (!t || json_uint(t, "format") != 1 || !product || strcmp(product, PRODUCT_ID) || !platform ||
        strcmp(platform, "windows")) {
        snprintf(err, errcap, "the manifest is not one of a TotK Recomp Multiplayer update for Windows");
        goto bad;
    }
    int v[4];
    if (!version || !parse_version(version, v) || strlen(version) >= sizeof m->version || version[0] == 'v') {
        snprintf(err, errcap, "the manifest names no version");
        goto bad;
    }
    if (want_version && compare_versions(version, want_version) != 0) {
        snprintf(err, errcap, "the manifest is for %s, the release is %s", version, want_version);
        goto bad;
    }
    if (compare_versions(version, TOTKMP_VERSION) <= 0) {
        snprintf(err, errcap, "the update (%s) is not newer than the installed version (%s)", version, TOTKMP_VERSION);
        goto bad;
    }
    if (!root || !safe_rel_path(root) || strchr(root, '/') || strlen(root) >= sizeof m->root || !pname ||
        !safe_rel_path(pname) || strchr(pname, '/') || strlen(pname) >= sizeof m->package || !is_hex(psha, 64) ||
        psize <= 0 || psize > (16ll << 30) || !files || files->type != J_ARRAY) {
        snprintf(err, errcap, "the manifest's package entry is not valid");
        goto bad;
    }
    copy_str(m->version, sizeof m->version, version);
    copy_str(m->root, sizeof m->root, root);
    copy_str(m->package, sizeof m->package, pname);
    copy_str(m->package_sha, sizeof m->package_sha, psha);
    m->package_size = (unsigned long long)psize;
    int n = 0;
    for (const JsonNode *f = files->child; f; f = f->next) n++;
    if (!n || n > MAX_FILES || !(m->files = (MFile *)calloc((size_t)n, sizeof *m->files))) {
        snprintf(err, errcap, "the manifest lists %d files", n);
        goto bad;
    }
    for (const JsonNode *f = files->child; f; f = f->next) {
        const char *p = json_str(f, "path"), *h = json_str(f, "sha256");
        long long size = json_uint(f, "size");
        if (!safe_rel_path(p) || !is_hex(h, 64) || size < 0) {
            snprintf(err, errcap, "the manifest's file entry %d is not valid", m->nfiles);
            goto bad;
        }
        MFile *e = &m->files[m->nfiles++];
        e->path = _strdup(p), e->key = _strdup(p);
        for (char *k = e->key; *k; k++) *k = (char)tolower((unsigned char)*k);
        e->size = (unsigned long long)size;
        m->files_size += e->size;
        memcpy(e->sha, h, 65);
    }
    qsort(m->files, (size_t)m->nfiles, sizeof *m->files, mfile_cmp);
    for (int i = 1; i < m->nfiles; i++)
        if (!strcmp(m->files[i - 1].key, m->files[i].key)) {
            snprintf(err, errcap, "the manifest lists %s twice", m->files[i].path);
            goto bad;
        }
    return 1;
bad:
    manifest_free(m);
    return 0;
}

static MFile *manifest_find(Manifest *m, const char *rel) {
    char key[512];
    copy_str(key, sizeof key, rel);
    for (char *k = key; *k; k++) *k = (char)tolower((unsigned char)*k);
    MFile probe = {.key = key};
    return (MFile *)bsearch(&probe, m->files, (size_t)m->nfiles, sizeof *m->files, mfile_cmp);
}

/* ---- the progress window ----------------------------------------------------------------------------------- */

#define WM_APP_REFRESH (WM_APP + 1)
#define WM_APP_DONE (WM_APP + 2)
#define ID_CANCEL 100

static HWND g_win, g_title, g_status, g_bar, g_detail, g_cancel_btn;
static HFONT g_font, g_bold;
static CRITICAL_SECTION g_ui_lock;
static wchar_t g_ui_title[256], g_ui_status[512], g_ui_detail[512];
static int g_ui_permille = -1;            /* -1: the bar moves without a known amount */
static volatile LONG g_cancel, g_cancellable = 1;
static int g_dpi = 96;

static int S(int px) { return MulDiv(px, g_dpi, 96); }

static void ui_set(const wchar_t *status, const wchar_t *detail, int permille) {
    EnterCriticalSection(&g_ui_lock);
    if (status) swprintf(g_ui_status, 512, L"%ls", status);
    if (detail) swprintf(g_ui_detail, 512, L"%ls", detail);
    g_ui_permille = permille;
    LeaveCriticalSection(&g_ui_lock);
    if (g_win) PostMessageW(g_win, WM_APP_REFRESH, 0, 0);
}

static void ui_refresh(void) {
    static int marquee = -1;
    EnterCriticalSection(&g_ui_lock);
    SetWindowTextW(g_title, g_ui_title);
    SetWindowTextW(g_status, g_ui_status);
    SetWindowTextW(g_detail, g_ui_detail);
    int pm = g_ui_permille;
    LeaveCriticalSection(&g_ui_lock);
    int want = pm < 0;
    if (want != marquee) {
        LONG_PTR style = GetWindowLongPtrW(g_bar, GWL_STYLE);
        SetWindowLongPtrW(g_bar, GWL_STYLE, want ? style | PBS_MARQUEE : style & ~PBS_MARQUEE);
        SendMessageW(g_bar, PBM_SETMARQUEE, want, 30);
        marquee = want;
    }
    if (pm >= 0) SendMessageW(g_bar, PBM_SETPOS, pm, 0);
    EnableWindow(g_cancel_btn, g_cancellable && !g_cancel);
}

static LRESULT CALLBACK win_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_APP_REFRESH: ui_refresh(); return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == ID_CANCEL && g_cancellable) {
            InterlockedExchange(&g_cancel, 1);
            ui_set(L"Stopping the update...", L"", -1);
        }
        return 0;
    case WM_CLOSE:
        if (g_cancellable) {
            InterlockedExchange(&g_cancel, 1);
            ui_set(L"Stopping the update...", L"", -1);
        }
        return 0;   /* the window closes when the work is done */
    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void ui_create(void) {
    INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    HDC dc = GetDC(NULL);
    g_dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(NULL, dc);
    NONCLIENTMETRICSW ncm = {sizeof ncm};
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW bold = ncm.lfMessageFont;
    bold.lfWeight = FW_SEMIBOLD;
    bold.lfHeight = bold.lfHeight * 5 / 4;
    g_bold = CreateFontIndirectW(&bold);
    HINSTANCE inst = GetModuleHandleW(NULL);
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = win_proc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.lpszClassName = L"TotKMPUpdate";
    RegisterClassW(&wc);
    int cw = S(500), ch = S(176);
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r = {0, 0, cw, ch};
    AdjustWindowRect(&r, style, FALSE);
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int w = r.right - r.left, hgt = r.bottom - r.top;
    g_win = CreateWindowExW(0, wc.lpszClassName, PRODUCT_NAME L" update", style, wa.left + (wa.right - wa.left - w) / 2,
                            wa.top + (wa.bottom - wa.top - hgt) / 2, w, hgt, NULL, NULL, inst, NULL);
    int m = S(18), iw = cw - 2 * m;
    g_title = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_NOPREFIX | SS_ENDELLIPSIS, m, S(16), iw, S(26),
                              g_win, NULL, inst, NULL);
    g_status = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_NOPREFIX | SS_ENDELLIPSIS, m, S(50), iw, S(20),
                               g_win, NULL, inst, NULL);
    g_bar = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE, m, S(76), iw, S(16), g_win, NULL, inst, NULL);
    SendMessageW(g_bar, PBM_SETRANGE32, 0, 1000);
    g_detail = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_NOPREFIX | SS_PATHELLIPSIS, m, S(100), iw,
                               S(20), g_win, NULL, inst, NULL);
    g_cancel_btn = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                   cw - m - S(96), S(132), S(96), S(28), g_win, (HMENU)(INT_PTR)ID_CANCEL, inst, NULL);
    SendMessageW(g_title, WM_SETFONT, (WPARAM)g_bold, 0);
    SendMessageW(g_status, WM_SETFONT, (WPARAM)g_font, 0);
    SendMessageW(g_detail, WM_SETFONT, (WPARAM)g_font, 0);
    SendMessageW(g_cancel_btn, WM_SETFONT, (WPARAM)g_font, 0);
    ui_refresh();
    ShowWindow(g_win, SW_SHOWNORMAL);
    UpdateWindow(g_win);
}

/* ---- processes --------------------------------------------------------------------------------------------- */

static void append_arg(wchar_t *cmd, size_t cap, const wchar_t *arg) {
    /* the quoting CommandLineToArgvW and the C runtime read: backslashes before a quote are doubled */
    size_t n = wcslen(cmd);
    if (n && n + 1 < cap) cmd[n++] = L' ';
    if (n + 1 < cap) cmd[n++] = L'"';
    for (const wchar_t *p = arg; *p && n + 4 < cap; p++) {
        int bs = 0;
        while (*p == L'\\') bs++, p++;
        if (!*p || *p == L'"') {
            for (int i = 0; i < bs * 2 && n + 4 < cap; i++) cmd[n++] = L'\\';
            if (!*p) break;
            cmd[n++] = L'\\';
            cmd[n++] = L'"';
        } else {
            for (int i = 0; i < bs && n + 4 < cap; i++) cmd[n++] = L'\\';
            cmd[n++] = *p;
        }
    }
    if (n + 1 < cap) cmd[n++] = L'"';
    cmd[n] = 0;
}

/* Starts the game of the install folder. TOTK_UPDATER_STARTED tells it (runtime/update_start.c, update_at_start) to
 * skip its update check this once: after a refused update the check would show the same prompt again. */
static void start_game(void) {
    wchar_t exe[PATHW], cmd[PATHW] = L"";
    SetEnvironmentVariableW(L"TOTK_UPDATER_STARTED", L"1");
    swprintf(exe, PATHW, L"%ls\\totk.exe", g_dir);
    append_arg(cmd, PATHW, exe);
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, g_dir, &si, &pi)) {
        LOG("started %ls (pid %lu)", exe, pi.dwProcessId);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        LOG("cannot start %ls (error %lu)", exe, GetLastError());
    }
}

/* Runs a program without a window and waits; its exit code, or -1. */
static int run_hidden(const wchar_t *exe, wchar_t *cmd, const wchar_t *cwd) {
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi;
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, cwd, &si, &pi)) return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = (DWORD)-1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}

/* ---- backup and restore ------------------------------------------------------------------------------------ */
/* The journal (update\journal.txt) has one word: "saving" while the backup is made (the install folder is still
   untouched), "patching" while the setup runs (a restore is needed if it stops), "done" after a success (only the
   backup's removal is left). update\backup.list names each top-level entry the backup covers: "f NAME" a file,
   "d NAME" a folder, "- NAME" a folder kept as it is (no hard links on that drive). */

static int never_touched(const wchar_t *name) {
    return !_wcsicmp(name, L"update") || !_wcsicmp(name, L"profile") || !_wcsicmp(name, L"mods") ||
           !_wcsicmp(name, L"logs");
}

static int g_links_failed;

/* Mirrors folder from into to: files as hard links (link = 1) or copies. 1 when everything is there. */
static int mirror_tree(const wchar_t *from, const wchar_t *to, int link, int *link_unsupported) {
    wchar_t pat[PATHW], a[PATHW], b[PATHW], la[PATHW], lb[PATHW];
    WIN32_FIND_DATAW fd;
    if (!make_dir(to)) return 0;
    swprintf(pat, PATHW, L"%ls\\*", from);
    HANDLE h = FindFirstFileExW(longp(pat, a), FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
    int ok = 1;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        swprintf(a, PATHW, L"%ls\\%ls", from, fd.cFileName);
        swprintf(b, PATHW, L"%ls\\%ls", to, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;   /* a link inside: left alone */
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            ok = mirror_tree(a, b, link, link_unsupported) && ok;
        } else if (link) {
            if (!CreateHardLinkW(longp(b, lb), longp(a, la), NULL)) {
                DWORD e = GetLastError();
                if (e == ERROR_NOT_SAME_DEVICE || e == ERROR_INVALID_FUNCTION || e == ERROR_NOT_SUPPORTED ||
                    e == ERROR_ACCESS_DENIED || e == ERROR_TOO_MANY_LINKS) {
                    if (link_unsupported) *link_unsupported = 1;
                    ok = copy_file(a, b) && ok;
                } else {
                    LOG("backup: cannot link %ls (error %lu)", a, e);
                    ok = 0;
                }
            }
        } else {
            ok = copy_file(a, b) && ok;
            if (!ok) LOG("backup: cannot copy %ls (error %lu)", a, GetLastError());
        }
    } while (ok && FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}

static int write_journal(const char *state) {
    wchar_t p[PATHW];
    swprintf(p, PATHW, L"%ls\\journal.txt", g_upd);
    LOG("journal: %s", state);
    return write_file(p, state, strlen(state));
}

static int journal_state(char *out, size_t cap) {
    wchar_t p[PATHW];
    swprintf(p, PATHW, L"%ls\\journal.txt", g_upd);
    char *t = read_file(p, 64, NULL);
    if (!t) return 0;
    sscanf(t, "%15s", out);
    out[cap - 1] = 0;
    free(t);
    return 1;
}

static void remove_journal(void) {
    wchar_t p[PATHW];
    swprintf(p, PATHW, L"%ls\\journal.txt", g_upd);
    wchar_t b[PATHW];
    DeleteFileW(longp(p, b));
}

/* The game folders that are big and that only a new extraction rewrites: without hard links they are not copied. */
static int big_game_folder(const wchar_t *name) {
    return !_wcsicmp(name, L"romfs") || !_wcsicmp(name, L"exefs") || !_wcsicmp(name, L"shared-fonts");
}

static int backup_install(void) {
    wchar_t backup[PATHW], list_path[PATHW], pat[PATHW], a[PATHW], b[PATHW], lp[PATHW];
    swprintf(backup, PATHW, L"%ls\\backup", g_upd);
    swprintf(list_path, PATHW, L"%ls\\backup.list", g_upd);
    if (!write_journal("saving")) return 0;
    delete_tree(backup);
    if (!make_dir(backup)) return 0;
    size_t cap = 1 << 16, used = 0;
    char *list = (char *)malloc(cap);
    WIN32_FIND_DATAW fd;
    swprintf(pat, PATHW, L"%ls\\*", g_dir);
    HANDLE h = FindFirstFileExW(longp(pat, lp), FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int ok = 1;
    g_links_failed = 0;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..") || never_touched(fd.cFileName)) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;   /* the player's own link: not ours */
        swprintf(a, PATHW, L"%ls\\%ls", g_dir, fd.cFileName);
        swprintf(b, PATHW, L"%ls\\%ls", backup, fd.cFileName);
        char *name = utf8(fd.cFileName);
        char kind = 'f';
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            kind = 'd';
            if (!_wcsicmp(fd.cFileName, L"build-cache")) {
                ok = mirror_tree(a, b, 0, NULL);
            } else if (g_links_failed && big_game_folder(fd.cFileName)) {
                kind = '-';
            } else {
                int unsupported = 0;
                if (big_game_folder(fd.cFileName)) {   /* try one link first: no copying 16 GB when links fail */
                    ok = mirror_tree(a, b, 1, &unsupported);
                } else {
                    ok = mirror_tree(a, b, !g_links_failed, &unsupported);
                }
                if (unsupported) g_links_failed = 1;
            }
        } else {
            ok = copy_file(a, b);
            if (!ok) LOG("backup: cannot copy %ls (error %lu)", a, GetLastError());
        }
        size_t need = strlen(name) + 4;
        if (used + need + 1 > cap) list = (char *)realloc(list, cap = (cap + need) * 2);
        used += (size_t)sprintf(list + used, "%c %s\n", kind, name);
        free(name);
    } while (ok && FindNextFileW(h, &fd));
    FindClose(h);
    ok = ok && write_file(list_path, list, used);
    free(list);
    if (!ok) {
        delete_tree(backup);
        remove_journal();
        return 0;
    }
    LOG("backup: done%s", g_links_failed ? " (no hard links on this drive: game data folders kept as they are)" : "");
    return write_journal("patching");
}

/* Removes the backup after a success, or after a restore. */
static void drop_backup(void) {
    wchar_t p[PATHW];
    swprintf(p, PATHW, L"%ls\\backup", g_upd);
    delete_tree(p);
    swprintf(p, PATHW, L"%ls\\trash", g_upd);
    delete_tree(p);
    swprintf(p, PATHW, L"%ls\\backup.list", g_upd);
    delete_tree(p);
    remove_journal();
}

/* Puts the saved install back: what the update added at the top of the folder goes, every saved entry returns. Safe
   to run again after it was cut off: an entry already moved back is skipped. 1 when the folder is as it was. */
static int restore_install(void) {
    wchar_t backup[PATHW], trash[PATHW], list_path[PATHW], pat[PATHW], a[PATHW], b[PATHW], t[PATHW], lp[PATHW];
    swprintf(backup, PATHW, L"%ls\\backup", g_upd);
    swprintf(trash, PATHW, L"%ls\\trash", g_upd);
    swprintf(list_path, PATHW, L"%ls\\backup.list", g_upd);
    char *list = read_file(list_path, 64u << 20, NULL);
    if (!list) {
        LOG("restore: no backup.list; the backup was never finished, so the install folder was not changed");
        drop_backup();
        return 1;
    }
    make_dir(trash);
    int ok = 1;
    /* what is in the folder now but not in the list: the update made it */
    WIN32_FIND_DATAW fd;
    swprintf(pat, PATHW, L"%ls\\*", g_dir);
    HANDLE h = FindFirstFileExW(longp(pat, lp), FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..") || never_touched(fd.cFileName) ||
                (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                continue;
            char *name = utf8(fd.cFileName), *hit = NULL;
            for (char *line = list; line && *line;) {
                char *end = strchr(line, '\n');
                size_t n = end ? (size_t)(end - line) : strlen(line);
                if (n > 2 && n - 2 == strlen(name) && !_strnicmp(line + 2, name, n - 2)) hit = line;
                line = end ? end + 1 : NULL;
            }
            if (!hit) {
                swprintf(a, PATHW, L"%ls\\%ls", g_dir, fd.cFileName);
                LOG("restore: removing %s, which the update added", name);
                if (!delete_tree(a)) ok = 0, LOG("restore: cannot remove %ls (error %lu)", a, GetLastError());
            }
            free(name);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    int n_entry = 0;
    for (char *line = list; line && *line;) {
        char *end = strchr(line, '\n');
        if (end) *end = 0;
        if (strlen(line) > 2 && line[0] != '-') {
            wchar_t *name = wide(line + 2);
            swprintf(a, PATHW, L"%ls\\%ls", backup, name);
            swprintf(b, PATHW, L"%ls\\%ls", g_dir, name);
            if (exists(a)) {
                if (line[0] == 'd') {
                    if (exists(b)) {
                        swprintf(t, PATHW, L"%ls\\%d-%ls", trash, n_entry, name);
                        delete_tree(t);
                        if (!move_path(b, t, 0)) {
                            /* a file held open somewhere: empty the folder instead */
                            LOG("restore: cannot move %ls aside (error %lu), deleting it", b, GetLastError());
                            delete_tree(b);
                        }
                    }
                    if (!move_path(a, b, 0)) ok = 0, LOG("restore: cannot put back %ls (error %lu)", b, GetLastError());
                } else {
                    /* a running program (the game and this updater, when the check at start restores) cannot be
                       replaced, but it can be renamed: the update's file goes to the trash first */
                    if (exists(b)) {
                        swprintf(t, PATHW, L"%ls\\%d-%ls", trash, n_entry, name);
                        delete_tree(t);
                        if (!move_path(b, t, 0)) LOG("restore: cannot move %ls aside (error %lu)", b, GetLastError());
                    }
                    if (!move_path(a, b, 1)) ok = 0, LOG("restore: cannot put back %ls (error %lu)", b, GetLastError());
                }
            }
            free(name);
        }
        n_entry++;
        line = end ? end + 1 : NULL;
    }
    free(list);
    if (ok) {
        LOG("restore: the install folder is as it was before the update");
        drop_backup();
    }
    return ok;
}

/* At start, before anything else: an update that was cut off is undone, one whose clean-up was cut off finished. */
static int recover(void) {
    char state[16] = "";
    if (!journal_state(state, sizeof state)) return 0;
    LOG("recover: journal says %s", state);
    if (!strcmp(state, "done")) {
        drop_backup();
        return 0;
    }
    if (!strcmp(state, "saving")) {   /* the backup was being made: the folder itself is untouched */
        drop_backup();
        return 0;
    }
    return restore_install() ? 1 : -1;
}

/* ---- the setup's command line ------------------------------------------------------------------------------ */

static const char *const step_names[][2] = {
    {"check", "Checking the computer and the game files"}, {"fonts", "Extracting the system fonts"},
    {"extract", "Extracting the game"}, {"lift", "Translating the game code to C"}, {"compile", "Compiling totk.exe"},
    {"prepare", "Preparing the shaders"}, {"dlss", "Downloading NVIDIA DLSS"}, {"cleanup", "Cleaning up"},
};

typedef struct {
    char steps[16][24];
    int nsteps, current;
    char error_id[96];
    char error_text[1024];
    char result[16];
    JsonNode *catalog;            /* the new setup's installer\text\en.json, for the error's English text */
} SetupRun;

static const char *step_title(const char *id) {
    for (size_t i = 0; i < sizeof step_names / sizeof *step_names; i++)
        if (!strcmp(step_names[i][0], id)) return step_names[i][1];
    return id;
}

/* The English text of an error event: the catalog's sentence with its {name} placeholders filled from params. */
static void render_error(SetupRun *run, const JsonNode *ev) {
    const char *id = json_str(ev, "id");
    const JsonNode *params = json_get(ev, "params");
    copy_str(run->error_id, sizeof run->error_id, id);
    const char *text = id ? json_str(run->catalog, id) : NULL;
    if (!text) {
        snprintf(run->error_text, sizeof run->error_text, "%s", id ? id : "unknown error");
        return;
    }
    size_t o = 0;
    for (const char *p = text; *p && o + 1 < sizeof run->error_text;) {
        const char *close = *p == '{' ? strchr(p, '}') : NULL;
        if (close && close - p < 40) {
            char name[48];
            snprintf(name, sizeof name, "%.*s", (int)(close - p - 1), p + 1);
            const JsonNode *v = !strcmp(name, "game_exe") ? NULL : json_get(params, name);
            const char *val = !strcmp(name, "game_exe") ? "totk.exe" : v && v->text ? v->text : "";
            o += (size_t)snprintf(run->error_text + o, sizeof run->error_text - o, "%s", val);
            if (o >= sizeof run->error_text) o = sizeof run->error_text - 1;
            p = close + 1;
        } else {
            run->error_text[o++] = *p++;
        }
    }
    run->error_text[o] = 0;
}

static void on_event(SetupRun *run, const char *line) {
    JsonNode *ev = json_parse(line, strlen(line));
    const char *type = json_str(ev, "type");
    if (!type) {
        json_free(ev);
        return;
    }
    if (!strcmp(type, "start")) {
        const JsonNode *steps = json_get(ev, "steps");
        run->nsteps = 0;
        for (const JsonNode *s = steps && steps->type == J_ARRAY ? steps->child : NULL; s && run->nsteps < 16; s = s->next)
            if (s->type == J_STRING) copy_str(run->steps[run->nsteps++], sizeof run->steps[0], s->text);
    } else if (!strcmp(type, "step") || !strcmp(type, "progress")) {
        const char *step = json_str(ev, "step"), *state = json_str(ev, "state");
        int index = -1;
        for (int i = 0; step && i < run->nsteps; i++)
            if (!strcmp(run->steps[i], step)) index = i;
        if (index >= 0) run->current = index;
        if (state && step) LOG("setup: %s %s", step, state);
        long long done = json_uint(ev, "done"), total = json_uint(ev, "total");
        const char *unit = json_str(ev, "unit");
        int n = run->nsteps ? run->nsteps : 8;
        double frac = total > 0 && done >= 0 ? (double)done / (double)total : state && !strcmp(state, "done") ? 1.0 : 0.0;
        if (frac > 1) frac = 1;
        int permille = (int)((run->current + frac) * 1000.0 / n);
        wchar_t status[256], detail[256];
        wchar_t *title = wide(step_title(step ? step : ""));
        swprintf(status, 256, L"Step %d of %d: %ls", run->current + 1, n, title);
        free(title);
        if (total > 0 && done >= 0 && unit && !strcmp(unit, "bytes"))
            swprintf(detail, 256, L"%.0f of %.0f MB", done / 1048576.0, total / 1048576.0);
        else if (total > 0 && done >= 0)
            swprintf(detail, 256, L"%lld of %lld %hs", done, total, unit ? unit : "");
        else
            swprintf(detail, 256, L"%ls", L"");
        ui_set(status, detail, permille);
    } else if (!strcmp(type, "error")) {
        render_error(run, ev);
        LOG("setup: error %s: %s", run->error_id, run->error_text);
    } else if (!strcmp(type, "warning")) {
        LOG("setup: warning %s", json_str(ev, "id") ? json_str(ev, "id") : "?");
    } else if (!strcmp(type, "end")) {
        copy_str(run->result, sizeof run->result, json_str(ev, "result"));
        LOG("setup: end %s", run->result);
    }
    json_free(ev);
}

typedef struct { char base[PATHW / 4], update[PATHW / 4], keys[PATHW / 4], firmware[PATHW / 4]; } Remembered;

/* The game files the setup remembered for this install folder (installer/recent.py: setup.json, or the file
   TOTK_SETUP_RECENT names). Paths only; the keys entry is the keys file's path. 0 when none is for this folder. */
static int remembered_inputs(Remembered *r) {
    wchar_t path[PATHW], env[PATHW];
    memset(r, 0, sizeof *r);
    DWORD n = GetEnvironmentVariableW(L"TOTK_SETUP_RECENT", env, PATHW);
    if (n && n < PATHW) {
        swprintf(path, PATHW, L"%ls", env);
    } else {
        n = GetEnvironmentVariableW(L"LOCALAPPDATA", env, PATHW);
        if (!n || n >= PATHW) return 0;
        swprintf(path, PATHW, L"%ls\\TotK Recomp Multiplayer\\setup.json", env);
    }
    char *text = read_file(path, 1 << 20, NULL);
    JsonNode *t = text ? json_parse(text, strlen(text)) : NULL;
    free(text);
    const char *dest = json_str(t, "dest");
    int ok = 0;
    if (dest) {
        wchar_t *wd = wide(dest), full[PATHW];
        if (GetFullPathNameW(wd, PATHW, full, NULL) && !_wcsicmp(full, g_dir)) {
            copy_str(r->base, sizeof r->base, json_str(t, "base"));
            copy_str(r->update, sizeof r->update, json_str(t, "update"));
            copy_str(r->keys, sizeof r->keys, json_str(t, "keys"));
            copy_str(r->firmware, sizeof r->firmware, json_str(t, "firmware"));
            ok = r->base[0] && r->update[0] && r->keys[0];
        }
        free(wd);
    }
    json_free(t);
    return ok;
}

/* Runs <setup>\python\python.exe -m installer --dest DIR [inputs] in a job that ends with this process, reading its
   events for the window. 1 when it ended ok. */
static int run_setup(const wchar_t *setup, const Remembered *inputs, SetupRun *run) {
    wchar_t python[PATHW], cmd[PATHW] = L"", catalog_path[PATHW];
    swprintf(python, PATHW, L"%ls\\python\\python.exe", setup);
    swprintf(catalog_path, PATHW, L"%ls\\installer\\text\\en.json", setup);
    char *cat = read_file(catalog_path, 4u << 20, NULL);
    json_free(run->catalog);
    run->catalog = cat ? json_parse(cat, strlen(cat)) : NULL;
    free(cat);
    run->error_id[0] = run->error_text[0] = run->result[0] = 0;
    run->nsteps = run->current = 0;
    append_arg(cmd, PATHW, python);
    wcscat(cmd, L" -m installer --dest");
    append_arg(cmd, PATHW, g_dir);
    wcscat(cmd, L" --root");
    append_arg(cmd, PATHW, setup);
    wchar_t dlss[PATHW];
    swprintf(dlss, PATHW, L"%ls\\nvngx_dlss.dll", g_dir);
    if (exists(dlss)) wcscat(cmd, L" --dlss");   /* the player chose DLSS: the setup keeps or fetches it */
    if (inputs) {
        const char *names[] = {"--base", "--update", "--keys", "--firmware"};
        const char *values[] = {inputs->base, inputs->update, inputs->keys, inputs->firmware};
        for (int i = 0; i < 4; i++)
            if (values[i][0]) {
                wchar_t *wn = wide(names[i]), *wv = wide(values[i]);
                wcscat(cmd, L" ");
                wcscat(cmd, wn);
                append_arg(cmd, PATHW, wv);
                free(wn), free(wv);
            }
    }
    char *ucmd = utf8(cmd);
    LOG("setup: %s", ucmd);
    free(ucmd);
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return 0;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
    STARTUPINFOW si = {sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = nul, si.hStdOutput = wr, si.hStdError = wr;
    PROCESS_INFORMATION pi;
    HANDLE job = CreateJobObjectW(NULL, NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim = {0};
    lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (job) SetInformationJobObject(job, JobObjectExtendedLimitInformation, &lim, sizeof lim);
    BOOL started = CreateProcessW(python, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, NULL, setup, &si, &pi);
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!started) {
        LOG("setup: cannot start %ls (error %lu)", python, GetLastError());
        CloseHandle(rd);
        if (job) CloseHandle(job);
        snprintf(run->error_text, sizeof run->error_text, "The new setup did not start.");
        return 0;
    }
    if (job) AssignProcessToJobObject(job, pi.hProcess);   /* its tools join the job too */
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    char buf[8192], line[16384];
    size_t ln = 0;
    DWORD got;
    while (ReadFile(rd, buf, sizeof buf, &got, NULL) && got) {
        for (DWORD i = 0; i < got; i++) {
            if (buf[i] == '\n' || ln + 1 >= sizeof line) {
                line[ln] = 0;
                if (ln && line[ln - 1] == '\r') line[ln - 1] = 0;
                if (line[0] == '{') on_event(run, line);
                else if (line[0]) LOG("setup> %s", line);
                ln = 0;
            } else {
                line[ln++] = buf[i];
            }
        }
    }
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    if (job) CloseHandle(job);
    LOG("setup: exit code %lu, result %s", code, run->result[0] ? run->result : "none");
    return code == 0 && !strcmp(run->result, "ok");
}

/* ---- the update -------------------------------------------------------------------------------------------- */

typedef struct {
    enum { UPD_OK, UPD_UNCHANGED, UPD_RESTORED, UPD_BROKEN, UPD_CANCELLED } result;
    char version[32];
    char message[1600];
} Outcome;

typedef struct { unsigned long long done, total; const wchar_t *what; DWORD last; } Meter;

static void meter_step(unsigned long long n, void *user) {
    Meter *m = (Meter *)user;
    m->done += n;
    DWORD now = GetTickCount();
    if (now - m->last < 100 && m->done < m->total) return;
    m->last = now;
    wchar_t detail[128];
    swprintf(detail, 128, L"%.0f of %.0f MB", m->done / 1048576.0, m->total / 1048576.0);
    ui_set(m->what, detail, m->total ? (int)(m->done * 1000 / m->total) : -1);
}

/* Walks the unpacked folder: every file must be in the manifest with its size and hash, nothing a link. */
static int verify_tree(Manifest *m, const wchar_t *dir, const char *rel, Meter *meter, char *err, size_t errcap) {
    wchar_t pat[PATHW], child[PATHW], lp[PATHW];
    WIN32_FIND_DATAW fd;
    swprintf(pat, PATHW, L"%ls\\*", dir);
    HANDLE h = FindFirstFileExW(longp(pat, lp), FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return 1;
    int ok = 1;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (g_cancel) { snprintf(err, errcap, "cancelled"); ok = 0; break; }
        char *name = utf8(fd.cFileName), sub[1024];
        snprintf(sub, sizeof sub, "%s%s%s", rel, *rel ? "/" : "", name);
        free(name);
        swprintf(child, PATHW, L"%ls\\%ls", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            snprintf(err, errcap, "the package holds a link: %s", sub);
            ok = 0;
        } else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            ok = verify_tree(m, child, sub, meter, err, errcap);
        } else {
            MFile *f = manifest_find(m, sub);
            char sha[65];
            unsigned long long size = 0;
            if (!f) {
                snprintf(err, errcap, "the package holds a file the manifest does not list: %s", sub);
                ok = 0;
            } else if (f->seen || !file_sha256(child, sha, &size, meter_step, meter) || size != f->size || strcmp(sha, f->sha)) {
                snprintf(err, errcap, "a file of the package does not match the manifest: %s", sub);
                ok = 0;
            } else {
                f->seen = 1;
            }
        }
    } while (ok && FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}

static unsigned long long top_files_size(void) {
    wchar_t pat[PATHW], lp[PATHW];
    WIN32_FIND_DATAW fd;
    unsigned long long n = 0;
    swprintf(pat, PATHW, L"%ls\\*", g_dir);
    HANDLE h = FindFirstFileExW(longp(pat, lp), FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) n += ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

/* Removes the setup folders of earlier updates, the downloads and the unpacking folder; keep names the one in use. */
static void clean_old_setups(const wchar_t *keep) {
    wchar_t pat[PATHW], p[PATHW], lp[PATHW];
    WIN32_FIND_DATAW fd;
    swprintf(pat, PATHW, L"%ls\\setup-*", g_upd);
    HANDLE h = FindFirstFileExW(longp(pat, lp), FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (keep && !_wcsicmp(fd.cFileName, keep))) continue;
            swprintf(p, PATHW, L"%ls\\%ls", g_upd, fd.cFileName);
            LOG("removing the earlier setup %ls", p);
            delete_tree(p);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    swprintf(p, PATHW, L"%ls\\download", g_upd);
    delete_tree(p);
    swprintf(p, PATHW, L"%ls\\stage", g_upd);
    delete_tree(p);
}

#define FAIL(...) do { snprintf(out->message, sizeof out->message, __VA_ARGS__); LOG("update: %s", out->message); goto done; } while (0)

static void do_update(Outcome *out, DWORD game_pid) {
    char err[600] = "";
    Release rel = {0};
    Manifest man = {0};
    char *mdata = NULL, *sdata = NULL;
    size_t mlen = 0, slen = 0;
    wchar_t pkg_path[PATHW], part[PATHW], stage[PATHW], setup[PATHW], setup_name[64], p[PATHW], lb[PATHW];
    out->result = UPD_UNCHANGED;

    /* the game has to be gone before any file is touched; it exits as soon as this process starts */
    if (game_pid) {
        HANDLE g = OpenProcess(SYNCHRONIZE, FALSE, game_pid);
        if (g) {
            ui_set(L"Waiting for the game to close...", L"", -1);
            DWORD w = WaitForSingleObject(g, 60000);
            CloseHandle(g);
            if (w != WAIT_OBJECT_0) FAIL("The game did not close.");
        }
    }

    ui_set(L"Looking for the update...", L"", -1);
    if (!fetch_release(&rel, 20000, err, sizeof err)) FAIL("The update could not be found: %s.", err);
    copy_str(out->version, sizeof out->version, rel.version);
    {
        wchar_t title[128];
        swprintf(title, 128, L"Updating to %hs", rel.version);
        EnterCriticalSection(&g_ui_lock);
        swprintf(g_ui_title, 256, L"%ls", title);
        LeaveCriticalSection(&g_ui_lock);
    }
    HttpTarget small = {.max = MAX_MANIFEST_BYTES, .cancel = &g_cancel};
    if (!http_get(rel.manifest_url, 30000, &small, &mdata, &mlen, err, sizeof err)) FAIL("The update manifest could not be downloaded: %s.", err);
    small.max = 4096;
    if (!http_get(rel.sig_url, 30000, &small, &sdata, &slen, err, sizeof err)) FAIL("The update's signature could not be downloaded: %s.", err);
    if (!manifest_load(&man, mdata, mlen, sdata, rel.version, err, sizeof err)) FAIL("The update was refused: %s.", err);
    LOG("update: manifest %s signed and valid, %d files, package %s (%llu bytes)", man.version, man.nfiles, man.package,
        man.package_size);

    /* space: the download, the unpacked setup, a copy of the top-level files, and room for the setup's own work */
    ULARGE_INTEGER free_bytes;
    unsigned long long need = man.package_size + man.files_size + top_files_size() + (1ull << 30);
    if (GetDiskFreeSpaceExW(g_dir, &free_bytes, NULL, NULL) && free_bytes.QuadPart < need)
        FAIL("Not enough free space: the update needs %.1f GB on the drive of %ls, %.1f GB are free.", need / 1e9, g_dir,
             free_bytes.QuadPart / 1e9);

    /* the package */
    swprintf(p, PATHW, L"%ls\\download", g_upd);
    make_dir(p);
    wchar_t *wpkg = wide(man.package);
    swprintf(pkg_path, PATHW, L"%ls\\download\\%ls", g_upd, wpkg);
    swprintf(part, PATHW, L"%ls.part", pkg_path);
    free(wpkg);
    char sha[65] = "";
    unsigned long long size = 0;
    if (!(exists(pkg_path) && file_sha256(pkg_path, sha, &size, NULL, NULL) && size == man.package_size &&
          !strcmp(sha, man.package_sha))) {
        const JsonNode *asset = find_asset(rel.tree, man.package);
        const char *url = json_str(asset, "browser_download_url");
        if (!url) FAIL("The release has no %s.", man.package);
        HANDLE f = CreateFileW(longp(part, lb), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        if (f == INVALID_HANDLE_VALUE) FAIL("Cannot write %ls (error %lu).", part, GetLastError());
        Sha s;
        sha_begin(&s);
        Meter meter = {0, man.package_size, L"Downloading the update...", 0};
        HttpTarget big = {.file = f, .expect = man.package_size, .sha = &s, .progress = meter_step, .user = &meter,
                          .cancel = &g_cancel};
        int got = http_get(url, 60000, &big, NULL, NULL, err, sizeof err);
        int flushed = FlushFileBuffers(f);
        CloseHandle(f);
        sha_end(&s, sha);
        if (!got) {
            delete_tree(part);
            if (g_cancel) { out->result = UPD_CANCELLED; FAIL("The update was cancelled."); }
            FAIL("The download failed: %s.", err);
        }
        if (!flushed || strcmp(sha, man.package_sha)) {
            delete_tree(part);
            FAIL("The download does not match the signed manifest (it is damaged or was changed), so it was refused.");
        }
        if (!move_path(part, pkg_path, 1)) FAIL("Cannot keep the download (error %lu).", GetLastError());
    }
    LOG("update: package verified (sha256 %s)", sha);

    /* unpack and check every file */
    ui_set(L"Unpacking the update...", L"", -1);
    swprintf(stage, PATHW, L"%ls\\stage", g_upd);
    delete_tree(stage);
    if (!make_dir(stage)) FAIL("Cannot create %ls.", stage);
    {
        wchar_t tar[PATHW], cmd[PATHW] = L"";
        GetSystemDirectoryW(tar, MAX_PATH);
        wcscat(tar, L"\\tar.exe");
        if (!exists(tar)) FAIL("Windows' tar.exe is missing; the update needs Windows 10 version 1803 or later.");
        append_arg(cmd, PATHW, tar);
        wcscat(cmd, L" -x -f");
        append_arg(cmd, PATHW, pkg_path);
        wcscat(cmd, L" -C");
        append_arg(cmd, PATHW, stage);
        int code = run_hidden(tar, cmd, stage);
        if (code != 0) FAIL("Unpacking the update failed (tar exit code %d).", code);
    }
    if (g_cancel) { out->result = UPD_CANCELLED; FAIL("The update was cancelled."); }
    {
        wchar_t pat[PATHW], lp[PATHW];
        WIN32_FIND_DATAW fd;
        swprintf(pat, PATHW, L"%ls\\*", stage);
        HANDLE h = FindFirstFileExW(longp(pat, lp), FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0);
        int extra = 0;
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
                char *n = utf8(fd.cFileName);
                if (strcmp(n, man.root) || !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
                    (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                    extra = 1;
                free(n);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        if (extra) FAIL("The update was refused: the package holds more than its %s folder.", man.root);
    }
    wchar_t *wroot = wide(man.root), root_dir[PATHW];
    swprintf(root_dir, PATHW, L"%ls\\%ls", stage, wroot);
    free(wroot);
    Meter vm = {0, man.files_size, L"Checking the update's files...", 0};
    if (!verify_tree(&man, root_dir, "", &vm, err, sizeof err)) {
        if (g_cancel) { out->result = UPD_CANCELLED; FAIL("The update was cancelled."); }
        FAIL("The update was refused: %s.", err);
    }
    for (int i = 0; i < man.nfiles; i++)
        if (!man.files[i].seen) FAIL("The update was refused: the package lacks %s.", man.files[i].path);
    LOG("update: all %d files match the manifest", man.nfiles);
    swprintf(setup_name, 64, L"setup-%hs", man.version);
    swprintf(setup, PATHW, L"%ls\\%ls", g_upd, setup_name);
    delete_tree(setup);
    if (!move_path(root_dir, setup, 0)) FAIL("Cannot move the new setup into place (error %lu).", GetLastError());
    delete_tree(stage);
    /* one temp folder for every setup that updates this install: the generated C and the objects stay, so the next
       update and the mods' rebuilds compile only what changed */
    swprintf(p, PATHW, L"%ls\\temp", g_upd);
    make_dir(p);
    {
        wchar_t link[PATHW], cmdexe[PATHW], cmd[PATHW] = L"";
        swprintf(link, PATHW, L"%ls\\temp", setup);
        GetSystemDirectoryW(cmdexe, MAX_PATH);
        wcscat(cmdexe, L"\\cmd.exe");
        if (!exists(link)) {
            swprintf(cmd, PATHW, L"\"%ls\" /d /c mklink /J", cmdexe);
            append_arg(cmd, PATHW, link);
            append_arg(cmd, PATHW, p);
            int code = run_hidden(cmdexe, cmd, g_upd);
            LOG("update: temp junction %ls -> %ls: %s", link, p, code == 0 ? "made" : "not made, the setup's own temp is used");
        }
    }
    if (g_cancel) { out->result = UPD_CANCELLED; FAIL("The update was cancelled."); }

    /* from here the install folder changes: no more Cancel */
    InterlockedExchange(&g_cancellable, 0);
    ui_set(L"Saving the current version...", L"So it can come back if anything goes wrong", -1);
    if (!backup_install()) FAIL("The current files could not be saved, so nothing was changed. See %ls\\update.log.", g_upd);

    ui_set(L"Updating the game...", L"", 0);
    SetupRun run = {0};
    int ok = run_setup(setup, NULL, &run);
    if (!ok && !strcmp(run.error_id, "check.inputs_needed")) {
        Remembered inputs;
        if (remembered_inputs(&inputs)) {
            LOG("update: the setup needs the game files; running it again with the remembered ones");
            ok = run_setup(setup, &inputs, &run);
        }
    }
    json_free(run.catalog);
    if (!ok) {
        ui_set(L"Putting the previous version back...", L"", -1);
        int restored = restore_install();
        out->result = restored ? UPD_RESTORED : UPD_BROKEN;
        FAIL("%s", run.error_text[0] ? run.error_text : "The setup stopped with an error.");
    }
    write_journal("done");
    ui_set(L"Cleaning up...", L"", -1);
    drop_backup();
    clean_old_setups(setup_name);
    out->result = UPD_OK;
    LOG("update: %s installed", man.version);
done:
    free(mdata);
    free(sdata);
    manifest_free(&man);
    json_free(rel.tree);
}

typedef struct { Outcome out; DWORD pid; } Work;

static DWORD WINAPI worker(void *arg) {
    Work *w = (Work *)arg;
    do_update(&w->out, w->pid);
    PostMessageW(g_win, WM_APP_DONE, 0, 0);
    return 0;
}

/* ---- the install folder ------------------------------------------------------------------------------------ */

/* g_dir from the command line: an install folder of this product (totk-install.json naming it, totk.exe beside it).
   Nothing is ever deleted or restored in any other folder. */
static int set_install_dir(const wchar_t *arg) {
    wchar_t full[PATHW], p[PATHW];
    DWORD n = GetFullPathNameW(arg, PATHW, full, NULL);
    if (!n || n >= PATHW - 64) return 0;
    while (n > 3 && (full[n - 1] == L'\\' || full[n - 1] == L'/')) full[--n] = 0;
    swprintf(g_dir, PATHW, L"%ls", full);
    swprintf(g_upd, PATHW, L"%ls\\update", g_dir);
    swprintf(p, PATHW, L"%ls\\totk-install.json", g_dir);
    char *text = read_file(p, 16u << 20, NULL);
    JsonNode *t = text ? json_parse(text, strlen(text)) : NULL;
    const char *product = json_str(t, "product");
    int ok = product && !strcmp(product, PRODUCT_ID);
    json_free(t);
    free(text);
    swprintf(p, PATHW, L"%ls\\totk.exe", g_dir);
    return ok && exists(p);
}

static void settings(void) {
    char url[2048];
    if (ini_get("update_url", url, sizeof url)) {
        copy_str(g_api_url, sizeof g_api_url, url);
        g_allow_http = loopback_http(url);
    } else {
        copy_str(g_api_url, sizeof g_api_url, API_URL);
    }
}

/* ---- --apply ------------------------------------------------------------------------------------------------ */

static int mode_apply(DWORD pid) {
    wchar_t lock_path[PATHW], lb[PATHW];
    InitializeCriticalSection(&g_ui_lock);
    swprintf(g_ui_title, 256, L"Updating %ls", PRODUCT_NAME);
    swprintf(g_ui_status, 512, L"Starting...");
    swprintf(lock_path, PATHW, L"%ls\\updater.lock", g_upd);
    HANDLE lock = CreateFileW(longp(lock_path, lb), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (lock == INVALID_HANDLE_VALUE) {
        LOG("apply: another updater runs");
        MessageBoxW(NULL, L"An update is already running.", PRODUCT_NAME, MB_OK | MB_ICONINFORMATION);
        return 1;
    }
    if (recover() < 0) {
        MessageBoxW(NULL, L"The last update could not be undone. Run TotKMP-Setup.exe on the install folder to repair it.",
                    PRODUCT_NAME, MB_OK | MB_ICONERROR);
        CloseHandle(lock);
        return 1;
    }
    ui_create();
    Work w = {.pid = pid};
    HANDLE th = CreateThread(NULL, 0, worker, &w, 0, NULL);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (msg.message == WM_APP_DONE) break;
        if (!IsDialogMessageW(g_win, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    WaitForSingleObject(th, INFINITE);
    CloseHandle(th);
    Outcome *o = &w.out;
    wchar_t text[2400];
    wchar_t *message = wide(o->message);
    switch (o->result) {
    case UPD_OK:
        DestroyWindow(g_win);
        CloseHandle(lock);
        start_game();
        return 0;
    case UPD_CANCELLED:
        DestroyWindow(g_win);
        CloseHandle(lock);
        start_game();
        return 0;
    case UPD_UNCHANGED:
        swprintf(text, 2400, L"The update to %hs did not happen.\n\n%ls\n\nNothing was changed; the game starts as it was.",
                 o->version[0] ? o->version : "the new version", message);
        break;
    case UPD_RESTORED:
        swprintf(text, 2400, L"The update to %hs failed.\n\n%ls\n\nThe previous version was put back and starts now. "
                             L"Details: %ls\\update.log",
                 o->version, message, g_upd);
        break;
    case UPD_BROKEN:
        swprintf(text, 2400, L"The update to %hs failed and the previous version could not be fully put back.\n\n%ls\n\n"
                             L"Run TotKMP-Setup.exe on %ls to repair the game. Details: %ls\\update.log",
                 o->version, message, g_dir, g_upd);
        break;
    }
    free(message);
    ShowWindow(g_win, SW_HIDE);
    MessageBoxW(NULL, text, PRODUCT_NAME, MB_OK | (o->result == UPD_UNCHANGED ? MB_ICONWARNING : MB_ICONERROR));
    DestroyWindow(g_win);
    CloseHandle(lock);
    if (o->result != UPD_BROKEN) start_game();
    return 1;
}

/* ---- --check ------------------------------------------------------------------------------------------------ */

typedef struct { Release rel; char err[600]; int ok; } CheckWork;

static DWORD WINAPI check_worker(void *arg) {
    CheckWork *c = (CheckWork *)arg;
    c->ok = fetch_release(&c->rel, CHECK_TIMEOUT_MS - 500, c->err, sizeof c->err);
    return 0;
}

#define ID_UPDATE 100
#define ID_LATER 101
#define ID_SKIP 102

static HRESULT CALLBACK prompt_cb(HWND h, UINT msg, WPARAM wp, LPARAM lp, LONG_PTR ref) {
    (void)h, (void)wp, (void)ref;
    if (msg == TDN_HYPERLINK_CLICKED) {
        const wchar_t *url = (const wchar_t *)lp;
        if (!wcsncmp(url, L"https://github.com/", 19)) ShellExecuteW(NULL, L"open", url, NULL, NULL, SW_SHOWNORMAL);
    }
    return S_OK;
}

static int prompt(const Release *rel) {
    wchar_t main_text[256], content[2400], footer[700];
    swprintf(main_text, 256, L"%ls %hs is out. Update now?", PRODUCT_NAME, rel->version);
    wchar_t *notes = wide(rel->notes);
    wchar_t size[64] = L"";
    if (rel->package_size) swprintf(size, 64, L" (a %.0f MB download)", rel->package_size / 1048576.0);
    swprintf(content, 2400, L"%ls%lsYou have %hs. Update downloads the new version%ls, rebuilds the game on this PC and "
                            L"starts it again. Your settings, saves, mods, shader caches and online identity are kept.",
             notes, notes[0] ? L"\n\n" : L"", TOTKMP_VERSION, size);
    free(notes);
    footer[0] = 0;
    if (rel->page[0]) {
        wchar_t *page = wide(rel->page);
        swprintf(footer, 700, L"<a href=\"%ls\">Full release notes</a>", page);
        free(page);
    }
    TASKDIALOG_BUTTON buttons[] = {{ID_UPDATE, L"&Update"}, {ID_LATER, L"&Later"}, {ID_SKIP, L"&Skip this version"}};
    TASKDIALOGCONFIG tc = {sizeof tc};
    tc.hInstance = GetModuleHandleW(NULL);
    tc.dwFlags = TDF_ENABLE_HYPERLINKS | TDF_ALLOW_DIALOG_CANCELLATION | TDF_USE_HICON_MAIN | TDF_SIZE_TO_CONTENT;
    tc.pszWindowTitle = PRODUCT_NAME;
    tc.hMainIcon = (HICON)LoadImageW(tc.hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON) * 2,
                                     GetSystemMetrics(SM_CYICON) * 2, LR_DEFAULTCOLOR);
    tc.pszMainInstruction = main_text;
    tc.pszContent = content;
    tc.pszFooter = footer[0] ? footer : NULL;
    tc.cButtons = 3;
    tc.pButtons = buttons;
    tc.nDefaultButton = ID_UPDATE;
    tc.pfCallback = prompt_cb;
    char *um = utf8(main_text), *uc = utf8(content);
    LOG("check: prompt: %s | %s", um, uc);
    free(um), free(uc);
    int choice = ID_LATER;
    HRESULT hr = TaskDialogIndirect(&tc, &choice, NULL, NULL);
    if (FAILED(hr)) {   /* no common controls 6: a plain box, Yes updates */
        int r = MessageBoxW(NULL, content, main_text, MB_YESNO | MB_ICONQUESTION);
        choice = r == IDYES ? ID_UPDATE : ID_LATER;
    }
    if (choice == IDCANCEL) choice = ID_LATER;
    return choice;
}

static int mode_check(DWORD game_pid) {
    wchar_t lock_path[PATHW], lb[PATHW];
    swprintf(lock_path, PATHW, L"%ls\\updater.lock", g_upd);
    if (exists(lock_path)) {   /* held open by a running updater (FILE_FLAG_DELETE_ON_CLOSE removes it at its end) */
        HANDLE l = CreateFileW(longp(lock_path, lb), GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (l == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION) {
            LOG("check: an update is running; the game does not start");
            MessageBoxW(NULL, L"An update of the game is running. The game starts by itself when it is done.",
                        PRODUCT_NAME, MB_OK | MB_ICONINFORMATION);
            return EXIT_UPDATING;
        }
        if (l != INVALID_HANDLE_VALUE) CloseHandle(l);
        DeleteFileW(longp(lock_path, lb));
    }
    int rec = recover();
    if (rec > 0) {
        /* the game that asked may be the cut-off update's new totk.exe (now in update\trash), and so may this
           updater: the restored game starts instead, without a check, and the caller exits */
        MessageBoxW(NULL, L"The last update did not finish. The previous version was put back and starts now.",
                    PRODUCT_NAME, MB_OK | MB_ICONWARNING);
        start_game();
        return EXIT_UPDATING;
    } else if (rec < 0) {
        MessageBoxW(NULL, L"The last update did not finish and could not be undone. Run TotKMP-Setup.exe on the install "
                          L"folder to repair the game.", PRODUCT_NAME, MB_OK | MB_ICONERROR);
        return EXIT_UPDATING;
    }
    char v[64];
    if (ini_get("update_check", v, sizeof v) && !strcmp(v, "0")) {
        LOG("check: update_check = 0");
        return EXIT_CONTINUE;
    }
    CheckWork *c = (CheckWork *)calloc(1, sizeof *c);   /* left to a thread that may outlive the wait */
    HANDLE th = CreateThread(NULL, 0, check_worker, c, 0, NULL);
    if (!th || WaitForSingleObject(th, CHECK_TIMEOUT_MS) != WAIT_OBJECT_0) {
        LOG("check: no answer within %d ms", CHECK_TIMEOUT_MS);
        return EXIT_CONTINUE;
    }
    CloseHandle(th);
    if (!c->ok) {
        LOG("check: %s", c->err);
        return EXIT_CONTINUE;
    }
    int cmp = compare_versions(c->rel.version, TOTKMP_VERSION);
    LOG("check: latest %s, installed %s", c->rel.version, TOTKMP_VERSION);
    if (cmp <= 0) return EXIT_CONTINUE;
    char skip[64];
    if (ini_get("update_skip", skip, sizeof skip) && compare_versions(skip, c->rel.version) == 0) {
        LOG("check: %s is skipped (update_skip)", c->rel.version);
        return EXIT_CONTINUE;
    }
    int choice = prompt(&c->rel);
    LOG("check: the player chose %s", choice == ID_UPDATE ? "Update" : choice == ID_SKIP ? "Skip this version" : "Later");
    if (choice == ID_SKIP) {
        if (!ini_set("update_skip", c->rel.version)) LOG("check: cannot write update_skip to totk.ini");
        return EXIT_CONTINUE;
    }
    if (choice != ID_UPDATE) return EXIT_CONTINUE;
    /* the update replaces totk-update.exe itself: a copy does the work */
    wchar_t self[PATHW], run_dir[PATHW], copy[PATHW], cmd[PATHW] = L"", pid[32];
    GetModuleFileNameW(NULL, self, PATHW);
    swprintf(run_dir, PATHW, L"%ls\\run", g_upd);
    swprintf(copy, PATHW, L"%ls\\totk-update.exe", run_dir);
    make_dir(run_dir);
    if (!copy_file(self, copy)) {
        LOG("check: cannot copy the updater to %ls (error %lu)", copy, GetLastError());
        MessageBoxW(NULL, L"The update could not start. The game starts as it is.", PRODUCT_NAME, MB_OK | MB_ICONWARNING);
        return EXIT_CONTINUE;
    }
    append_arg(cmd, PATHW, copy);
    wcscat(cmd, L" --apply");
    append_arg(cmd, PATHW, g_dir);
    swprintf(pid, 32, L" --pid %lu", game_pid);
    wcscat(cmd, pid);
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(copy, cmd, NULL, NULL, FALSE, 0, NULL, run_dir, &si, &pi)) {
        LOG("check: cannot start %ls (error %lu)", copy, GetLastError());
        MessageBoxW(NULL, L"The update could not start. The game starts as it is.", PRODUCT_NAME, MB_OK | MB_ICONWARNING);
        return EXIT_CONTINUE;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    LOG("check: updater started (pid %lu)", pi.dwProcessId);
    return EXIT_UPDATING;
}

/* ---- --selftest ---------------------------------------------------------------------------------------------- */

static int g_fails;
static void expect(int cond, const char *what) {
    char line[300];
    snprintf(line, sizeof line, "%s %s\n", cond ? "ok  " : "FAIL", what);
    DWORD put;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line, (DWORD)strlen(line), &put, NULL);
    g_fails += !cond;
}

static int mode_selftest(void) {
    /* RFC 8032 section 7.1, test 2: one message byte */
    static const char *pk = "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c";
    static const char *sg = "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
                            "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00";
    uint8_t pub[32], sig[64], msg[1] = {0x72};
    unhex(pk, pub, 32), unhex(sg, sig, 64);
    expect(crypto_ed25519_check(sig, pub, msg, 1) == 0, "ed25519: RFC 8032 test 2 verifies");
    msg[0] ^= 1;
    expect(crypto_ed25519_check(sig, pub, msg, 1) != 0, "ed25519: a changed message fails");
    expect(compare_versions("0.1.1", "0.1.0") > 0 && compare_versions("v0.2", "0.1.9") > 0 &&
           compare_versions("0.1.0", "0.1") == 0 && compare_versions("0.10.0", "0.9.0") > 0 &&
           compare_versions("0.1.0-rc1", "0.0.1") == -2 && compare_versions("1.2.3.4.5", "1") == -2 &&
           compare_versions("", "1") == -2 && compare_versions("1..2", "1") == -2, "versions compare");
    expect(safe_rel_path("bin/totk-update.exe") && !safe_rel_path("../x") && !safe_rel_path("a/./b") &&
           !safe_rel_path("a\\b") && !safe_rel_path("c:x") && !safe_rel_path("a/con.txt") && !safe_rel_path("a/") &&
           !safe_rel_path("/a") && !safe_rel_path("a/b.") && !safe_rel_path("a/ b") && safe_rel_path("Lib/(x)~1.py"),
           "manifest paths");
    const char *j = "{\"a\": [1, {\"b\": \"x\\u00e9\\ud83d\\ude00\\n\"}], \"c\": true, \"n\": 12345678901}";
    JsonNode *t = json_parse(j, strlen(j));
    const JsonNode *a = json_get(t, "a");
    expect(t && a && a->child && json_str(a->child->next, "b") &&
           !strcmp(json_str(a->child->next, "b"), "x\xc3\xa9\xf0\x9f\x98\x80\n") && json_true(t, "c") &&
           json_uint(t, "n") == 12345678901ll, "json reads");
    json_free(t);
    const char *bad[] = {"{", "[1,]", "{\"a\" 1}", "\"\\ud800\"", "\"a\x01\"", "[1] x", "{\"a\":\"\\u0000\"}", ""};
    int refused = 1;
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        JsonNode *b = json_parse(bad[i], strlen(bad[i]));
        refused &= !b;
        json_free(b);
    }
    expect(refused, "json refuses broken text");
    char deep[200];
    memset(deep, '[', 100), memset(deep + 100, ']', 100);
    JsonNode *d = json_parse(deep, 200);
    expect(!d, "json refuses deep nesting");
    json_free(d);
    char ex[300];
    notes_excerpt("# Title\n\nThe **first** line with `code` and a [link](https://x).\n- a point\n", ex, sizeof ex);
    expect(!strcmp(ex, "The first line with code and a link.\n\xe2\x80\xa2 a point"), "release notes excerpt");
    /* totk.ini edits */
    wchar_t tmp[MAX_PATH], ini[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    swprintf(ini, MAX_PATH, L"%lsupdater-selftest-%lu.ini", tmp, GetCurrentProcessId());
    const char *start = "[display]\r\nwindow_size = 960x540\r\n[update]\r\n# update_check = 1\r\n";
    write_file(ini, start, strlen(start));
    ini_set_in(ini, "update_skip", "0.1.1");
    char *got = read_file(ini, 4096, NULL);
    expect(got && !strcmp(got, "[display]\r\nwindow_size = 960x540\r\n[update]\r\nupdate_skip = 0.1.1\r\n# update_check = 1\r\n"),
           "ini: a new key goes under [update]");
    free(got);
    ini_set_in(ini, "update_skip", "0.1.2");
    got = read_file(ini, 4096, NULL);
    expect(got && strstr(got, "update_skip = 0.1.2\r\n") && !strstr(got, "0.1.1"), "ini: a key is replaced in place");
    free(got);
    write_file(ini, "fps_limit = 60", 14);
    ini_set_in(ini, "update_skip", "0.1.1");
    got = read_file(ini, 4096, NULL);
    expect(got && !strcmp(got, "fps_limit = 60\n\n[update]\nupdate_skip = 0.1.1\n"), "ini: a new section at the end");
    free(got);
    DeleteFileW(ini);
    expect(loopback_http("http://127.0.0.1:8080/x") && loopback_http("http://localhost/x") &&
           !loopback_http("http://127.0.0.1.evil.example/x") && !loopback_http("http://localhost.evil/x") &&
           !loopback_http("https://127.0.0.1/"), "plain http only to a loopback address");
    return g_fails ? 1 : 0;
}

/* ---- main ---------------------------------------------------------------------------------------------------- */

/* ---- TOTK_TEST_WINDOW=<slot> (test runs, as the game's runtime/gfx_window_win.c): every top-level window of the
 * updater (progress window, task dialog, message boxes) opens at the top left of that slot, a third of the second
 * monitor's working area stacked top to bottom, and is never activated, so a test never takes the user's focus. A
 * thread-local CBT hook on the UI thread does it: HCBT_ACTIVATE comes before the window is first shown and activated,
 * after the dialog code has centred it, so the move sticks, and returning nonzero refuses the activation. The variable
 * is inherited by the copy the updater relaunches itself as and by the game it starts. */
static int g_test_slot = -1;
static RECT g_test_area;

static BOOL CALLBACK test_find_secondary(HMONITOR m, HDC dc, LPRECT r, LPARAM out) {
    (void)dc, (void)r;
    MONITORINFO mi = {sizeof mi};
    if (GetMonitorInfoW(m, &mi) && !(mi.dwFlags & MONITORINFOF_PRIMARY)) {
        *(MONITORINFO *)out = mi;
        return FALSE;
    }
    return TRUE;
}

static LRESULT CALLBACK test_cbt(int code, WPARAM wp, LPARAM lp) {
    if (code == HCBT_ACTIVATE) {
        HWND h = (HWND)wp;
        if (!(GetWindowLongW(h, GWL_STYLE) & WS_CHILD)) {
            SetWindowPos(h, HWND_BOTTOM, g_test_area.left, g_test_area.top, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
            return 1;   /* refuse the activation */
        }
    }
    return CallNextHookEx(NULL, code, wp, lp);
}

static void test_window_setup(void) {
    wchar_t v[8];
    DWORD n = GetEnvironmentVariableW(L"TOTK_TEST_WINDOW", v, 8);
    if (n != 1 || v[0] < L'0' || v[0] > L'2') return;
    g_test_slot = v[0] - L'0';
    MONITORINFO mi = {0};
    EnumDisplayMonitors(NULL, NULL, test_find_secondary, (LPARAM)&mi);
    if (!mi.cbSize) {
        mi.cbSize = sizeof mi;
        GetMonitorInfoW(MonitorFromPoint((POINT){0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    }
    RECT w = mi.rcWork;
    int third = (w.bottom - w.top) / 3;
    g_test_area = (RECT){w.left + 20, w.top + g_test_slot * third + 20, w.right, w.top + (g_test_slot + 1) * third};
    SetWindowsHookExW(WH_CBT, test_cbt, NULL, GetCurrentThreadId());
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show) {
    (void)inst, (void)prev, (void)cmdline, (void)show;
    SetProcessDPIAware();
    test_window_setup();
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const wchar_t *mode = NULL, *dir = NULL;
    DWORD pid = 0;
    for (int i = 1; argv && i < argc; i++) {
        if ((!wcscmp(argv[i], L"--check") || !wcscmp(argv[i], L"--apply")) && i + 1 < argc) mode = argv[i], dir = argv[++i];
        else if (!wcscmp(argv[i], L"--pid") && i + 1 < argc) pid = wcstoul(argv[++i], NULL, 10);
        else if (!wcscmp(argv[i], L"--selftest")) mode = argv[i];
    }
    if (mode && !wcscmp(mode, L"--selftest")) return mode_selftest();
    if (!mode || !dir) {
        MessageBoxW(NULL, L"totk-update.exe is started by the game (totk.exe); it updates TotK Recomp Multiplayer.",
                    PRODUCT_NAME, MB_OK | MB_ICONINFORMATION);
        return 2;
    }
    if (!set_install_dir(dir)) return !wcscmp(mode, L"--check") ? EXIT_CONTINUE : 2;   /* not an install: never touch it */
    log_open();
    settings();
    LOG("%ls %ls (updater %s, install %ls)", mode, g_dir, TOTKMP_VERSION, g_dir);
    if (!wcscmp(mode, L"--check")) return mode_check(pid);
    return mode_apply(pid);
}
