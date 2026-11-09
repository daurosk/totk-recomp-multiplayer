/* The server's configuration file and its data folder: identity key, accounts, bans, mutes, moderators, the host,
 * one-time claim codes and the log. Every file is plain text the host can read and edit while the server is stopped
 * (docs/server.md, "The data folder"). Lists are rewritten whole through a temporary file, so a crash leaves either
 * the old file or the new one.
 *
 * Nicknames, reasons and chat text that reach these files and the log came from clients, but the wire layer only
 * lets through UTF-8 without control characters, so none of them can start a new line or forge an entry.
 */
#include "sv.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

sv_store g_store;

/* ---- small helpers ---- */

void sv_hex(char *out, const uint8_t *in, size_t n) {
    static const char d[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = d[in[i] >> 4];
        out[2 * i + 1] = d[in[i] & 15];
    }
    out[2 * n] = 0;
}

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int sv_unhex(uint8_t *out, size_t n, const char *in) {
    for (size_t i = 0; i < n; i++) {
        int a = hexval((unsigned char)in[2 * i]), b = a < 0 ? -1 : hexval((unsigned char)in[2 * i + 1]);
        if (a < 0 || b < 0) return -1;
        out[i] = (uint8_t)(a << 4 | b);
    }
    return in[2 * n] && !isspace((unsigned char)in[2 * n]) ? -1 : 0;
}

static void *grow(void *p, int *cap, int need, size_t elem) {
    if (need <= *cap) return p;
    int c = *cap ? *cap * 2 : 16;
    while (c < need) c *= 2;
    void *q = realloc(p, (size_t)c * elem);
    if (!q) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    *cap = c;
    return q;
}

static void path_of(char *out, size_t n, const char *name) { snprintf(out, n, "%s/%s", g_store.dir, name); }

void sv_copy_cut(char *dst, size_t max, const char *src) {
    size_t n = strlen(src);
    if (n > max) {
        n = max;
        while (n && ((unsigned char)src[n] & 0xC0) == 0x80) n--;
    }
    memcpy(dst, src, n);
    dst[n] = 0;
}

/* Text a client could have sent: no byte below 0x20 and no DEL, so it cannot break a line of a file. */
static int clean_text(const char *s) {
    for (; *s; s++)
        if ((unsigned char)*s < 0x20 || *s == 0x7F) return 0;
    return 1;
}

/* Opens path.tmp for writing; sv_commit then puts it in place. */
static FILE *begin_write(const char *name, char *tmp, size_t n) {
    snprintf(tmp, n, "%s/%s.tmp", g_store.dir, name);
    FILE *f = fopen(tmp, "w");
    if (!f) sv_log("cannot write %s: %s", tmp, strerror(errno));
    return f;
}

static void commit(FILE *f, const char *tmp, const char *name) {
    char dst[600];
    path_of(dst, sizeof dst, name);
    int bad = ferror(f);
    if (fclose(f) != 0 || bad || sv_replace_file(tmp, dst) != 0) sv_log("cannot replace %s", dst);
}

/* Lines of a data file without comments or blank lines; fn gets each, its number and ctx. */
static void each_line(const char *name, void (*fn)(char *line, int no, void *ctx), void *ctx) {
    char path[600], line[1024];
    path_of(path, sizeof path, name);
    FILE *f = fopen(path, "r");
    if (!f) return;
    int no = 0;
    while (fgets(line, sizeof line, f)) {
        no++;
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#') continue;
        fn(p, no, ctx);
    }
    fclose(f);
}

/* ---- the log ---- */

void sv_log(const char *fmt, ...) {
    char msg[1200], stamp[32];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    time_t t = (time_t)sv_unix_time();
    struct tm tm;
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &tm);
    printf("[%s] %s\n", stamp, msg);
    fflush(stdout);
    if (g_store.log) {
        fprintf(g_store.log, "[%s] %s\n", stamp, msg);
        fflush(g_store.log);
    }
}

/* ---- configuration ---- */

void sv_config_defaults(sv_config *c) {
    memset(c, 0, sizeof *c);
    snprintf(c->bind, sizeof c->bind, "0.0.0.0");
    c->port = SV_PORT;
    snprintf(c->name, sizeof c->name, "TotK Recomp room");
    snprintf(c->motd, sizeof c->motd, "Welcome. Type /help in chat for commands.");
    c->max_players = 8;
    c->open = 1;
    c->hold_seconds = 30;
    c->handshake_seconds = 5;
    c->idle_seconds = 10;
    c->teleport = 1;
    c->max_per_ip = 4;
    c->max_pending_per_ip = 4;
    c->connects_per_minute = 30;
    c->new_accounts_per_hour = 20;
    c->chat_burst = 5;
    c->chat_per_second = 1.0;
    c->action_burst = 10;
    c->action_per_second = 2.0;
    c->frame_burst = 200;
    c->frames_per_second = 60.0;
    c->log_chat = 1;
    c->afk_seconds = 300;
    c->start_rupees = 0;
    c->wallet_earn_max = 5000;
    c->wallet_earn_per_minute = 20000;
}

static const char DEFAULT_CONFIG[] =
    "# TotK Recomp Multiplayer server settings. Restart the server after editing (docs/server.md).\n"
    "\n"
    "# Address and TCP port to listen on. \"0.0.0.0\" is every IPv4 address, \"::\" every IPv4 and IPv6 address.\n"
    "bind = \"0.0.0.0\"\n"
    "port = 27120\n"
    "\n"
    "# Shown to players.\n"
    "name = \"TotK Recomp room\"\n"
    "motd = \"Welcome. Type /help in chat for commands.\"\n"
    "\n"
    "# Most players at once (1 to 32). The host can always get in.\n"
    "max_players = 8\n"
    "\n"
    "# Who may make an account. Accounts are game keys, never passwords: a player who joined once is let back in.\n"
    "# open = true: any new player may join; with a password set they must type it in the game's claim field.\n"
    "# open = false: a new player needs a one-time claim code from the console's `claim` command.\n"
    "open = true\n"
    "password = \"\"\n"
    "\n"
    "# The host's key (64 hex characters, shown in the console when they join). Empty: the first account to join\n"
    "# becomes the host and is written to host.txt.\n"
    "host = \"\"\n"
    "\n"
    "# Seconds a dropped player has to reconnect and keep their place, team and rank.\n"
    "hold_seconds = 30\n"
    "\n"
    "# Players may ask to teleport to each other.\n"
    "teleport = true\n"
    "# Which positions players see on their map: \"all\" or \"team\" (the host always sees all).\n"
    "positions = \"all\"\n"
    "\n"
    "# Protection against floods and broken clients. The defaults suit a normal room.\n"
    "handshake_seconds = 5\n"
    "idle_seconds = 10\n"
    "max_per_ip = 4\n"
    "max_pending_per_ip = 4\n"
    "connects_per_minute = 30\n"
    "new_accounts_per_hour = 20\n"
    "chat_burst = 5\n"
    "chat_per_second = 1.0\n"
    "action_burst = 10\n"
    "action_per_second = 2.0\n"
    "frame_burst = 200\n"
    "frames_per_second = 60.0\n"
    "\n"
    "# Write chat lines to server.log.\n"
    "log_chat = true\n"
    "\n"
    "# Seconds without moving, chatting or acting before the player list marks a player away (0: never).\n"
    "afk_seconds = 300\n"
    "\n"
    "# The shared economy (docs/server.md). Rupees a new account starts with; the most one request may add; the most\n"
    "# an account may gain in a minute. A game's request for more is cut down to this.\n"
    "start_rupees = 0\n"
    "wallet_earn_max = 5000\n"
    "wallet_earn_per_minute = 20000\n";

static int parse_value(char *v, char *out, size_t n, int *quoted) {
    *quoted = 0;
    if (*v == '"') {
        size_t k = 0;
        *quoted = 1;
        for (v++; *v && *v != '"'; v++) {
            if (*v == '\\' && (v[1] == '"' || v[1] == '\\')) v++;
            if (k + 1 >= n) return -1;
            out[k++] = *v;
        }
        if (*v != '"') return -1;
        out[k] = 0;
        for (v++; *v == ' ' || *v == '\t'; v++) {}
        return *v && *v != '#' ? -1 : 0;
    }
    size_t k = 0;
    while (*v && *v != '#' && *v != ' ' && *v != '\t') {
        if (k + 1 >= n) return -1;
        out[k++] = *v++;
    }
    out[k] = 0;
    return k ? 0 : -1;
}

static int as_int(const char *v, long lo, long hi, int *out) {
    char *end;
    errno = 0;
    long x = strtol(v, &end, 10);
    if (errno || *end || x < lo || x > hi) return -1;
    *out = (int)x;
    return 0;
}

static int as_double(const char *v, double lo, double hi, double *out) {
    char *end;
    double x = strtod(v, &end);
    if (*end || !(x >= lo && x <= hi)) return -1;
    *out = x;
    return 0;
}

static int as_bool(const char *v, int *out) {
    if (!strcmp(v, "true")) *out = 1;
    else if (!strcmp(v, "false")) *out = 0;
    else return -1;
    return 0;
}

static int as_text(const char *v, char *out, size_t max) {
    if (strlen(v) > max || !clean_text(v)) return -1;
    snprintf(out, max + 1, "%s", v);
    return 0;
}

static int config_set(sv_config *c, const char *k, const char *v) {
    int port;
    if (!strcmp(k, "bind")) return as_text(v, c->bind, sizeof c->bind - 1);
    if (!strcmp(k, "port")) {
        if (as_int(v, 1, 65535, &port)) return -1;
        c->port = (uint16_t)port;
        return 0;
    }
    if (!strcmp(k, "name")) return as_text(v, c->name, OW_NICK_MAX);
    if (!strcmp(k, "motd")) return as_text(v, c->motd, OW_NOTICE_MAX);
    if (!strcmp(k, "max_players")) return as_int(v, 1, OW_PLAYERS_MAX, &c->max_players);
    if (!strcmp(k, "password")) return as_text(v, c->password, OW_CLAIM_MAX);
    if (!strcmp(k, "open")) return as_bool(v, &c->open);
    if (!strcmp(k, "host")) {
        uint8_t key[32];
        if (*v && sv_unhex(key, 32, v)) return -1;
        return as_text(v, c->host, 64);
    }
    if (!strcmp(k, "hold_seconds")) return as_int(v, 0, 3600, &c->hold_seconds);
    if (!strcmp(k, "handshake_seconds")) return as_int(v, 1, 60, &c->handshake_seconds);
    if (!strcmp(k, "idle_seconds")) return as_int(v, 2, 600, &c->idle_seconds);
    if (!strcmp(k, "teleport")) return as_bool(v, &c->teleport);
    if (!strcmp(k, "positions")) {
        if (!strcmp(v, "all")) c->positions_team_only = 0;
        else if (!strcmp(v, "team")) c->positions_team_only = 1;
        else return -1;
        return 0;
    }
    if (!strcmp(k, "max_per_ip")) return as_int(v, 1, SV_MAX_CONNS, &c->max_per_ip);
    if (!strcmp(k, "max_pending_per_ip")) return as_int(v, 1, SV_MAX_PENDING, &c->max_pending_per_ip);
    if (!strcmp(k, "connects_per_minute")) return as_int(v, 1, 100000, &c->connects_per_minute);
    if (!strcmp(k, "new_accounts_per_hour")) return as_int(v, 1, 100000, &c->new_accounts_per_hour);
    if (!strcmp(k, "chat_burst")) return as_int(v, 1, 1000, &c->chat_burst);
    if (!strcmp(k, "chat_per_second")) return as_double(v, 0.01, 1000, &c->chat_per_second);
    if (!strcmp(k, "action_burst")) return as_int(v, 1, 1000, &c->action_burst);
    if (!strcmp(k, "action_per_second")) return as_double(v, 0.01, 1000, &c->action_per_second);
    if (!strcmp(k, "frame_burst")) return as_int(v, 20, 100000, &c->frame_burst);
    if (!strcmp(k, "frames_per_second")) return as_double(v, 10, 100000, &c->frames_per_second);
    if (!strcmp(k, "log_chat")) return as_bool(v, &c->log_chat);
    if (!strcmp(k, "afk_seconds")) return as_int(v, 0, 86400, &c->afk_seconds);
    if (!strcmp(k, "start_rupees")) return as_int(v, 0, 999999, &c->start_rupees);
    if (!strcmp(k, "wallet_earn_max")) return as_int(v, 1, 999999, &c->wallet_earn_max);
    if (!strcmp(k, "wallet_earn_per_minute")) return as_int(v, 1, 9999999, &c->wallet_earn_per_minute);
    return -2;
}

int sv_config_load(sv_config *c, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        f = fopen(path, "w");
        if (!f) {
            fprintf(stderr, "cannot write %s: %s\n", path, strerror(errno));
            return -1;
        }
        fputs(DEFAULT_CONFIG, f);
        fclose(f);
        printf("wrote the default settings to %s\n", path);
        return 0;
    }
    char line[1024], val[600];
    int no = 0, bad = 0;
    while (fgets(line, sizeof line, f)) {
        no++;
        char *p = line;
        size_t n = strlen(p);
        while (n && (p[n - 1] == '\n' || p[n - 1] == '\r')) p[--n] = 0;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#') continue;
        char *eq = strchr(p, '=');
        if (!eq) {
            fprintf(stderr, "%s:%d: expected key = value\n", path, no);
            bad = 1;
            continue;
        }
        char *ke = eq;
        while (ke > p && (ke[-1] == ' ' || ke[-1] == '\t')) ke--;
        *ke = 0;
        char *v = eq + 1;
        while (*v == ' ' || *v == '\t') v++;
        int quoted;
        if (parse_value(v, val, sizeof val, &quoted)) {
            fprintf(stderr, "%s:%d: cannot read the value of %s\n", path, no, p);
            bad = 1;
            continue;
        }
        int e = config_set(c, p, val);
        if (e == -2) {
            fprintf(stderr, "%s:%d: unknown setting %s (ignored)\n", path, no, p);
        } else if (e) {
            fprintf(stderr, "%s:%d: %s = %s is out of range or malformed\n", path, no, p, val);
            bad = 1;
        }
    }
    fclose(f);
    return bad ? -1 : 0;
}

/* ---- identity ---- */

static int load_identity(void) {
    char path[600], line[200];
    uint8_t seed[32];
    path_of(path, sizeof path, "server.key");
    FILE *f = fopen(path, "r");
    if (f) {
        int ok = fgets(line, sizeof line, f) && sv_unhex(seed, 32, line) == 0;
        fclose(f);
        if (!ok) {
            fprintf(stderr, "%s is damaged: it must hold 64 hex characters. Restore it from a backup, or delete it "
                            "to make a new server key (players are then asked to trust the new key).\n", path);
            return -1;
        }
    } else {
        if (ow_random(seed, 32)) {
            fprintf(stderr, "no random bytes from the system\n");
            return -1;
        }
        char hex[65], tmp[620];
        sv_hex(hex, seed, 32);
        FILE *w = begin_write("server.key", tmp, sizeof tmp);
        if (!w) return -1;
        fprintf(w, "%s\n", hex);
        commit(w, tmp, "server.key");
        sv_private_file(path);
        memset(hex, 0, sizeof hex);
        sv_log("made a new server key in %s", path);
    }
    ow_identity_from_seed(&g_store.identity, seed);
    memset(seed, 0, sizeof seed);
    memset(line, 0, sizeof line);
    return 0;
}

/* ---- accounts ---- */

static void account_line(char *line, int no, void *ctx) {
    (void)ctx;
    char *end;
    uint64_t id = strtoull(line, &end, 10);
    uint8_t key[32];
    if (!id || *end != ' ' || sv_unhex(key, 32, end + 1)) {
        sv_log("accounts.txt:%d: not an account line (ignored)", no);
        return;
    }
    const char *nick = end + 1 + 64;
    while (*nick == ' ') nick++;
    if (sv_account_by_key(key) || sv_account_by_id(id)) {
        sv_log("accounts.txt:%d: account %llu or its key is listed twice (ignored)", no, (unsigned long long)id);
        return;
    }
    g_store.acc = grow(g_store.acc, &g_store.capacc, g_store.nacc + 1, sizeof *g_store.acc);
    sv_account *a = &g_store.acc[g_store.nacc++];
    a->id = id;
    memcpy(a->key, key, 32);
    sv_copy_cut(a->nick, OW_NICK_MAX, clean_text(nick) ? nick : "");
    if (id >= g_store.next_id) g_store.next_id = id + 1;
}

sv_account *sv_account_by_key(const uint8_t key[32]) {
    for (int i = 0; i < g_store.nacc; i++)
        if (!memcmp(g_store.acc[i].key, key, 32)) return &g_store.acc[i];
    return NULL;
}

sv_account *sv_account_by_id(uint64_t id) {
    for (int i = 0; i < g_store.nacc; i++)
        if (g_store.acc[i].id == id) return &g_store.acc[i];
    return NULL;
}

sv_account *sv_account_add(const uint8_t key[32], const char *nick) {
    g_store.acc = grow(g_store.acc, &g_store.capacc, g_store.nacc + 1, sizeof *g_store.acc);
    sv_account *a = &g_store.acc[g_store.nacc++];
    a->id = g_store.next_id++;
    memcpy(a->key, key, 32);
    sv_copy_cut(a->nick, OW_NICK_MAX, nick);
    sv_accounts_save();
    return a;
}

void sv_accounts_save(void) {
    char tmp[620], hex[65];
    FILE *f = begin_write("accounts.txt", tmp, sizeof tmp);
    if (!f) return;
    fprintf(f, "# account id, key, last name. Written by the server.\n");
    for (int i = 0; i < g_store.nacc; i++) {
        sv_hex(hex, g_store.acc[i].key, 32);
        fprintf(f, "%llu %s %s\n", (unsigned long long)g_store.acc[i].id, hex, g_store.acc[i].nick);
    }
    commit(f, tmp, "accounts.txt");
}

/* ---- bans and mutes ---- */

static void sanctions_save(sv_sanctions *s) {
    char tmp[620], hex[65];
    FILE *f = begin_write(s->file, tmp, sizeof tmp);
    if (!f) return;
    fprintf(f, "# key, end (seconds since 1970, 0 for until lifted), reason\n");
    for (int i = 0; i < s->n; i++) {
        sv_hex(hex, s->v[i].key, 32);
        fprintf(f, "%s %lld %s\n", hex, (long long)s->v[i].until, s->v[i].reason);
    }
    commit(f, tmp, s->file);
}

static void sanction_line(char *line, int no, void *ctx) {
    sv_sanctions *s = ctx;
    uint8_t key[32];
    char *end;
    if (sv_unhex(key, 32, line)) {
        sv_log("%s:%d: not a key (ignored)", s->file, no);
        return;
    }
    long long until = strtoll(line + 64, &end, 10);
    if (end == line + 64 || until < 0) until = 0;
    while (*end == ' ') end++;
    s->v = grow(s->v, &s->cap, s->n + 1, sizeof *s->v);
    sv_sanction *e = &s->v[s->n++];
    memcpy(e->key, key, 32);
    e->until = until;
    sv_copy_cut(e->reason, OW_REASON_MAX, clean_text(end) ? end : "");
}

static void sanctions_load(sv_sanctions *s) {
    s->n = 0;
    each_line(s->file, sanction_line, s);
}

sv_sanction *sv_sanction_find(sv_sanctions *s, const uint8_t key[32]) {
    int64_t now = sv_unix_time();
    int dropped = 0;
    for (int i = 0; i < s->n; i++)
        if (s->v[i].until && s->v[i].until <= now) {
            s->v[i--] = s->v[--s->n];
            dropped = 1;
        }
    if (dropped) sanctions_save(s);
    for (int i = 0; i < s->n; i++)
        if (!memcmp(s->v[i].key, key, 32)) return &s->v[i];
    return NULL;
}

void sv_sanction_set(sv_sanctions *s, const uint8_t key[32], int64_t until, const char *reason) {
    sv_sanction *e = NULL;
    for (int i = 0; i < s->n; i++)
        if (!memcmp(s->v[i].key, key, 32)) e = &s->v[i];
    if (!e) {
        s->v = grow(s->v, &s->cap, s->n + 1, sizeof *s->v);
        e = &s->v[s->n++];
        memcpy(e->key, key, 32);
    }
    e->until = until;
    sv_copy_cut(e->reason, OW_REASON_MAX, reason ? reason : "");
    sanctions_save(s);
}

int sv_sanction_remove(sv_sanctions *s, const uint8_t key[32]) {
    for (int i = 0; i < s->n; i++)
        if (!memcmp(s->v[i].key, key, 32)) {
            s->v[i] = s->v[--s->n];
            sanctions_save(s);
            return 1;
        }
    return 0;
}

/* ---- moderators and the host ---- */

static void mods_save(void) {
    char tmp[620], hex[65];
    FILE *f = begin_write("moderators.txt", tmp, sizeof tmp);
    if (!f) return;
    fprintf(f, "# One key per line: these accounts are moderators.\n");
    for (int i = 0; i < g_store.nmods; i++) {
        sv_hex(hex, g_store.mods[i], 32);
        sv_account *a = sv_account_by_key(g_store.mods[i]);
        fprintf(f, "%s%s%s\n", hex, a ? " # " : "", a ? a->nick : "");
    }
    commit(f, tmp, "moderators.txt");
}

static void mod_line(char *line, int no, void *ctx) {
    (void)ctx;
    uint8_t key[32];
    char *hash = strchr(line, '#');
    if (hash) *hash = 0;
    if (sv_unhex(key, 32, line)) {
        sv_log("moderators.txt:%d: not a key (ignored)", no);
        return;
    }
    if (sv_is_moderator(key)) return;
    g_store.mods = grow(g_store.mods, &g_store.capmods, g_store.nmods + 1, sizeof *g_store.mods);
    memcpy(g_store.mods[g_store.nmods++], key, 32);
}

int sv_is_moderator(const uint8_t key[32]) {
    for (int i = 0; i < g_store.nmods; i++)
        if (!memcmp(g_store.mods[i], key, 32)) return 1;
    return 0;
}

void sv_set_moderator(const uint8_t key[32], int on) {
    for (int i = 0; i < g_store.nmods; i++)
        if (!memcmp(g_store.mods[i], key, 32)) {
            if (!on) {
                memmove(g_store.mods[i], g_store.mods[i + 1], (size_t)(g_store.nmods - i - 1) * 32);
                g_store.nmods--;
                mods_save();
            }
            return;
        }
    if (!on) return;
    g_store.mods = grow(g_store.mods, &g_store.capmods, g_store.nmods + 1, sizeof *g_store.mods);
    memcpy(g_store.mods[g_store.nmods++], key, 32);
    mods_save();
}

void sv_set_host(const uint8_t key[32]) {
    memcpy(g_store.host, key, 32);
    g_store.has_host = 1;
    char tmp[620], hex[65];
    FILE *f = begin_write("host.txt", tmp, sizeof tmp);
    if (!f) return;
    sv_hex(hex, key, 32);
    fprintf(f, "%s\n", hex);
    commit(f, tmp, "host.txt");
}

int sv_rank_of(const uint8_t key[32]) {
    if (g_store.has_host && !memcmp(g_store.host, key, 32)) return OW_RANK_HOST;
    return sv_is_moderator(key) ? OW_RANK_MODERATOR : OW_RANK_PLAYER;
}

/* ---- claim codes ---- */

static void claims_save(void) {
    char tmp[620];
    FILE *f = begin_write("claims.txt", tmp, sizeof tmp);
    if (!f) return;
    fprintf(f, "# Unused one-time claim codes. Each lets one new player make an account.\n");
    for (int i = 0; i < g_store.nclaims; i++) fprintf(f, "%s\n", g_store.claims[i]);
    commit(f, tmp, "claims.txt");
}

static void claim_line(char *line, int no, void *ctx) {
    (void)ctx;
    size_t n = strlen(line);
    while (n && (line[n - 1] == ' ' || line[n - 1] == '\t')) line[--n] = 0;
    if (!n || n > OW_CLAIM_MAX || !clean_text(line)) {
        sv_log("claims.txt:%d: not a claim code (ignored)", no);
        return;
    }
    g_store.claims = grow(g_store.claims, &g_store.capclaims, g_store.nclaims + 1, sizeof *g_store.claims);
    snprintf(g_store.claims[g_store.nclaims++], OW_CLAIM_MAX + 1, "%s", line);
}

/* Compares in time that does not depend on where the texts differ, so a client cannot learn a code byte by byte. */
static int same_secret(const char *a, const char *b) {
    size_t na = strlen(a), nb = strlen(b);
    unsigned d = (unsigned)(na ^ nb);
    for (size_t i = 0; i < OW_CLAIM_MAX; i++) {
        unsigned char x = i < na ? (unsigned char)a[i] : 0, y = i < nb ? (unsigned char)b[i] : 0;
        d |= (unsigned)(x ^ y);
    }
    return d == 0;
}

int sv_claim_take(const char *code) {
    int hit = -1;
    for (int i = 0; i < g_store.nclaims; i++)
        if (same_secret(g_store.claims[i], code)) hit = i;
    if (hit < 0) return 0;
    memmove(g_store.claims[hit], g_store.claims[hit + 1], (size_t)(g_store.nclaims - hit - 1) * sizeof *g_store.claims);
    g_store.nclaims--;
    claims_save();
    return 1;
}

int sv_password_matches(const char *given, const char *password) { return same_secret(given, password); }

void sv_claim_new(char *out) {
    static const char abc[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"; /* 32 letters: a byte's low 5 bits pick evenly */
    uint8_t r[12];
    ow_random(r, sizeof r);
    int k = 0;
    for (int i = 0; i < 12; i++) {
        if (i && i % 4 == 0) out[k++] = '-';
        out[k++] = abc[r[i] & 31];
    }
    out[k] = 0;
    g_store.claims = grow(g_store.claims, &g_store.capclaims, g_store.nclaims + 1, sizeof *g_store.claims);
    snprintf(g_store.claims[g_store.nclaims++], OW_CLAIM_MAX + 1, "%s", out);
    claims_save();
}

/* ---- opening ---- */

static void host_line(char *line, int no, void *ctx) {
    (void)ctx;
    if (g_store.has_host) return;
    if (sv_unhex(g_store.host, 32, line)) {
        sv_log("host.txt:%d: not a key (ignored)", no);
        return;
    }
    g_store.has_host = 1;
}

void sv_store_reload(void) {
    sanctions_load(&g_store.bans);
    sanctions_load(&g_store.mutes);
    g_store.nmods = 0;
    each_line("moderators.txt", mod_line, NULL);
}

int sv_store_open(const char *dir, const sv_config *c) {
    snprintf(g_store.dir, sizeof g_store.dir, "%s", dir);
    if (sv_mkdir(dir)) {
        fprintf(stderr, "cannot make the data folder %s\n", dir);
        return -1;
    }
    char path[600];
    path_of(path, sizeof path, "server.log");
    g_store.log = fopen(path, "a");
    if (!g_store.log) fprintf(stderr, "cannot open %s; logging to the console only\n", path);
    if (load_identity()) return -1;
    g_store.next_id = 1;
    g_store.bans.file = "bans.txt";
    g_store.mutes.file = "mutes.txt";
    each_line("accounts.txt", account_line, NULL);
    sv_store_reload();
    each_line("claims.txt", claim_line, NULL);
    if (c->host[0]) {
        sv_unhex(g_store.host, 32, c->host);
        g_store.has_host = 1;
        g_store.host_from_config = 1;
    } else {
        each_line("host.txt", host_line, NULL);
    }
    return 0;
}

void sv_store_close(void) {
    if (g_store.log) fclose(g_store.log);
    g_store.log = NULL;
    ow_identity_wipe(&g_store.identity);
}
