/* A headless game for the online client: runtime/net/net_client.c and net_stub.c, linked with this file instead of the
 * runtime, driven by commands on standard input like tools/net_bot.c, printing what the UI would see as one JSON
 * object per line. tests/net_client_check.py builds it and runs it against the real server and the bots.
 *
 *   net_client_main --profile DIR
 *
 * Commands, one per line:
 *   connect ADDRESS NAME [PASSWORD]     net_connect
 *   disconnect                          net_disconnect
 *   say all|team|me TEXT...             net_chat_send (me: "/me TEXT" on the all channel)
 *   whisper NAME TEXT...
 *   tp ID | tpanswer REQUEST 0|1        net_teleport_request, net_teleport_answer
 *   teamcreate NAME | invite ID | teamanswer ID 0|1 | leave
 *   host ACTION ID [TEXT...]            net_host_action with a NetHostAction number
 *   pos X Y Z                           net_set_local
 *   drop                                net_test_drop: the socket closes as if the network failed
 *   wallet DELTA CAUSE | lfind KEY | sfind KEY | lspend N | sspend N | skysend DAY HOUR DUE
 *                                       net_wallet_ask, net_ledger_find (Light, Korok seed), net_ledger_spend, net_sky_send
 *   eco                                 {"ev":"eco", on, wallet, rupees, ledger, sky, leader}: the economy as the game sees it
 *   dump                                {"ev":"dump", state, self, status, players, teams}
 *   echo TEXT | quit
 * Out: {"ev":"event", type, player, request, accepted, pos, name, text} for each net_poll_events event,
 * {"ev":"chat", channel, from, from_name, to_name, text} for each new chat line, {"ev":"ret", cmd, r} after each
 * command that returns something.
 *
 * The shims below stand in for the runtime functions the client calls (cfg.h, host.h, platform/platform.h). cfg_get
 * reads the environment, as the runtime does before totk.ini.
 */
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#endif
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "net/net_client.h"
#include "platform/platform.h"

void net_test_drop(void);

/* ---- shims of the runtime */

static RtConfig g_rc;
const char rt_default_profile[] = "profile";
const char rt_port_version[] = "test";
const RtConfig *rt_config(void) { return &g_rc; }

const char *cfg_get(const char *name) { return getenv(name); }
int cfg_bool(const char *name) {
    const char *v = getenv(name);
    return v && *v && strcmp(v, "0");
}

void rt_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[host] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

#ifdef _WIN32
void plat_mutex_lock(PlatMutex *m) { AcquireSRWLockExclusive((PSRWLOCK)&m->p); }
void plat_mutex_unlock(PlatMutex *m) { ReleaseSRWLockExclusive((PSRWLOCK)&m->p); }
uint64_t plat_now_ns(void) {
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)((double)c.QuadPart * 1e9 / (double)f.QuadPart);
}
uint64_t plat_unix_time_ns(void) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (t - 116444736000000000ull) * 100u;
}
void plat_sleep_ms(uint32_t ms) { Sleep(ms); }
typedef struct { PlatThreadFn fn; void *arg; } Start;
static DWORD WINAPI thread_start(LPVOID p) {
    Start s = *(Start *)p;
    free(p);
    s.fn(s.arg);
    return 0;
}
int plat_thread_spawn(PlatThreadFn fn, void *arg, const char *name, size_t stack) {
    (void)name, (void)stack;
    Start *s = malloc(sizeof *s);
    s->fn = fn, s->arg = arg;
    HANDLE h = CreateThread(NULL, 0, thread_start, s, 0, NULL);
    if (!h) return 0;
    CloseHandle(h);
    return 1;
}
void plat_file_mkdir_p(const char *path) {
    char p[1024];
    snprintf(p, sizeof p, "%s", path);
    for (char *c = p + 1; *c; c++)
        if (*c == '/' || *c == '\\') {
            char k = *c;
            *c = 0;
            CreateDirectoryA(p, NULL);
            *c = k;
        }
    CreateDirectoryA(p, NULL);
}
static int replace_file(const char *from, const char *to) { return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING); }
#else
void plat_mutex_lock(PlatMutex *m) {
    while (__atomic_exchange_n((uintptr_t *)&m->p, (uintptr_t)1, __ATOMIC_ACQUIRE)) sched_yield();
}
void plat_mutex_unlock(PlatMutex *m) { __atomic_store_n((uintptr_t *)&m->p, (uintptr_t)0, __ATOMIC_RELEASE); }
uint64_t plat_now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
uint64_t plat_unix_time_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
void plat_sleep_ms(uint32_t ms) { usleep(ms * 1000u); }
typedef struct { PlatThreadFn fn; void *arg; } Start;
static void *thread_start(void *p) {
    Start s = *(Start *)p;
    free(p);
    s.fn(s.arg);
    return NULL;
}
int plat_thread_spawn(PlatThreadFn fn, void *arg, const char *name, size_t stack) {
    (void)name, (void)stack;
    Start *s = malloc(sizeof *s);
    s->fn = fn, s->arg = arg;
    pthread_t t;
    if (pthread_create(&t, NULL, thread_start, s)) return 0;
    pthread_detach(t);
    return 1;
}
void plat_file_mkdir_p(const char *path) {
    char p[1024];
    snprintf(p, sizeof p, "%s", path);
    for (char *c = p + 1; *c; c++)
        if (*c == '/') {
            *c = 0;
            mkdir(p, 0700);
            *c = '/';
        }
    mkdir(p, 0700);
}
static int replace_file(const char *from, const char *to) { return rename(from, to) == 0; }
#endif

int plat_file_write_atomic(const char *path, const void *data, size_t n) {
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return 0;
    int ok = fwrite(data, 1, n, f) == n;
    ok &= fclose(f) == 0;
    return ok && replace_file(tmp, path);
}

/* ---- standard input, on a thread so the loop below keeps ticking */

static PlatMutex g_in_mx;
static char *g_in[256];
static int g_nin, g_in_end;

static void reader(void *arg) {
    (void)arg;
    char line[4096];
    while (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\r\n")] = 0;
        for (;;) {
            plat_mutex_lock(&g_in_mx);
            int ok = g_nin < 256;
            if (ok) g_in[g_nin++] = strdup(line);
            plat_mutex_unlock(&g_in_mx);
            if (ok) break;
            plat_sleep_ms(5);
        }
    }
    plat_mutex_lock(&g_in_mx);
    g_in_end = 1;
    plat_mutex_unlock(&g_in_mx);
}

/* ---- JSON out */

static void js(const char *s) {
    putchar('"');
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') printf("\\%c", c);
        else if (c < 0x20) printf("\\u%04x", c);
        else putchar(c);
    }
    putchar('"');
}

static void print_player(const NetPlayer *p) {
    printf("{\"id\":%u,\"name\":", p->id);
    js(p->name);
    printf(",\"character\":");
    js(p->character);
    printf(",\"area\":");
    js(p->area);
    printf(",\"pos\":[%g,%g,%g],\"team\":%u,\"role\":%d,\"ping\":%u,\"muted\":%u}", p->pos[0], p->pos[1], p->pos[2], p->team,
           (int)p->role, p->ping_ms, p->muted);
}

static void dump(void) {
    static NetPlayer pl[NET_MAX_PLAYERS];
    static NetTeam tm[NET_MAX_TEAMS];
    int n = net_players(pl, NET_MAX_PLAYERS), nt = net_teams(tm, NET_MAX_TEAMS);
    printf("{\"ev\":\"dump\",\"enabled\":%d,\"state\":%d,\"self\":%u,\"status\":", net_enabled(), (int)net_state(),
           net_self_id());
    js(net_status_text());
    printf(",\"players\":[");
    for (int i = 0; i < n; i++) {
        if (i) putchar(',');
        print_player(&pl[i]);
    }
    printf("],\"teams\":[");
    for (int i = 0; i < nt; i++) {
        printf("%s{\"id\":%u,\"name\":", i ? "," : "", tm[i].id);
        js(tm[i].name);
        printf(",\"leader\":%u}", tm[i].leader);
    }
    printf("]}\n");
}

static void ret(const char *cmd, int r) {
    printf("{\"ev\":\"ret\",\"cmd\":");
    js(cmd);
    printf(",\"r\":%d}\n", r);
}

static const char *rest_of(char *line, int words) {
    char *s = line;
    for (int i = 0; i < words && *s; i++) {
        while (*s == ' ') s++;
        while (*s && *s != ' ') s++;
    }
    while (*s == ' ') s++;
    return s;
}

static int run(char *line) {
    char cmd[32] = "", a[128] = "", b[128] = "";
    sscanf(line, "%31s %127s %127s", cmd, a, b);
    if (!strcmp(cmd, "quit")) return 1;
    if (!strcmp(cmd, "echo")) {
        printf("{\"ev\":\"echo\",\"text\":");
        js(rest_of(line, 1));
        printf("}\n");
    } else if (!strcmp(cmd, "connect")) {
        NetConfig c;
        memset(&c, 0, sizeof c);
        snprintf(c.address, sizeof c.address, "%s", a);
        snprintf(c.name, sizeof c.name, "%s", b);
        snprintf(c.token, sizeof c.token, "%s", rest_of(line, 3));
        ret(cmd, net_connect(&c));
    } else if (!strcmp(cmd, "disconnect")) {
        net_disconnect();
    } else if (!strcmp(cmd, "say")) {
        const char *t = rest_of(line, 2);
        char me[300];
        if (!strcmp(a, "me")) snprintf(me, sizeof me, "/me %s", t), t = me;
        ret(cmd, net_chat_send(!strcmp(a, "team") ? NET_CHAT_TEAM : NET_CHAT_ALL, NULL, t));
    } else if (!strcmp(cmd, "whisper")) {
        ret(cmd, net_chat_send(NET_CHAT_WHISPER, a, rest_of(line, 2)));
    } else if (!strcmp(cmd, "tp")) {
        ret(cmd, net_teleport_request((uint32_t)atoi(a)));
    } else if (!strcmp(cmd, "tpanswer")) {
        net_teleport_answer((uint32_t)atoi(a), atoi(b));
    } else if (!strcmp(cmd, "teamcreate")) {
        ret(cmd, net_team_create(rest_of(line, 1)));
    } else if (!strcmp(cmd, "invite")) {
        ret(cmd, net_team_invite((uint32_t)atoi(a)));
    } else if (!strcmp(cmd, "teamanswer")) {
        net_team_answer((uint32_t)atoi(a), atoi(b));
    } else if (!strcmp(cmd, "leave")) {
        net_team_leave();
    } else if (!strcmp(cmd, "host")) {
        ret(cmd, net_host_action((NetHostAction)atoi(a), (uint32_t)atoi(b), rest_of(line, 3)));
    } else if (!strcmp(cmd, "pos")) {
        float p[3] = {0};
        sscanf(line, "%*s %f %f %f", &p[0], &p[1], &p[2]);
        char area[NET_AREA_MAX];
        snprintf(area, sizeof area, "Surface (%d, %d)", (int)p[0], (int)p[2]);
        net_set_local(area, p, "Link");
    } else if (!strcmp(cmd, "drop")) {
        net_test_drop();
    } else if (!strcmp(cmd, "dump")) {
        dump();
    } else if (!strcmp(cmd, "wallet")) {
        ret(cmd, net_wallet_ask((int32_t)atoi(a), (NetWalletCause)atoi(b)));
    } else if (!strcmp(cmd, "lfind") || !strcmp(cmd, "sfind")) {
        ret(cmd, net_ledger_find(cmd[0] == 's', (uint32_t)strtoul(a, NULL, 0)));
    } else if (!strcmp(cmd, "lspend") || !strcmp(cmd, "sspend")) {
        ret(cmd, net_ledger_spend(cmd[0] == 's', (uint32_t)atoi(a)));
    } else if (!strcmp(cmd, "skysend")) {
        ret(cmd, net_sky_send((uint32_t)atoi(a), (float)atof(b), atoi(rest_of(line, 3))));
    } else if (!strcmp(cmd, "lootdrop")) {
        NetLootDrop d;
        NetActorId id;
        memset(&d, 0, sizeof d);
        snprintf(d.name, sizeof d.name, "%s", a);
        d.fused = (uint32_t)strtoul(b, NULL, 0);
        d.pos[0] = 1;
        d.pos[1] = 2;
        d.pos[2] = 3;
        int r = net_loot_drop(&d, &id);
        printf("{\"ev\":\"lootid\",\"r\":%d,\"origin\":%llu,\"serial\":%llu}\n", r, (unsigned long long)id.origin,
               (unsigned long long)id.serial);
    } else if (!strcmp(cmd, "loottake")) {
        NetActorId id = {strtoull(a, NULL, 10), strtoull(b, NULL, 10)};
        ret(cmd, net_loot_take(id));
    } else if (!strcmp(cmd, "eco")) {
        uint32_t rupees = 0, ws = 0, ls = 0, ss = 0;
        NetLedger l = {0};
        NetSky s = {0};
        int w = net_wallet_get(&rupees, &ws), lk = net_ledger_get(&l, &ls), sk = net_sky_get(&s, &ss);
        printf("{\"ev\":\"eco\",\"on\":%d,\"wallet\":%d,\"rupees\":%u,\"ledger\":%d,\"lights_found\":%u,\"lights_spent\":%u,"
               "\"seeds_found\":%u,\"seeds_spent\":%u,\"sky\":%d,\"day\":%u,\"hour\":%g,\"due\":%u,\"leader\":%d}\n",
               net_economy_on(), w, rupees, lk, l.lights_found, l.lights_spent, l.seeds_found, l.seeds_spent, sk, s.day,
               s.hour, s.blood_moon_due, net_sky_is_leader());
    }
    return 0;
}

int main(int argc, char **argv) {
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], "--profile")) g_rc.profile = argv[++i];
    plat_thread_spawn(reader, NULL, "stdin", 0);
    uint32_t seen = 0;
    for (;;) {
        int quit = 0;
        for (;;) {
            plat_mutex_lock(&g_in_mx);
            char *line = NULL;
            int end = g_in_end && !g_nin;
            if (g_nin) {
                line = g_in[0];
                memmove(g_in, g_in + 1, (size_t)--g_nin * sizeof *g_in);
            }
            plat_mutex_unlock(&g_in_mx);
            if (end) quit = 1;
            if (!line) break;
            net_tick();   // a frame between commands, as in the game
            quit = run(line);
            free(line);
            if (quit) break;
        }
        net_tick();
        NetEvent ev[16];
        int n;
        while ((n = net_poll_events(ev, 16)) > 0)
            for (int i = 0; i < n; i++) {
                printf("{\"ev\":\"event\",\"type\":%d,\"player\":%u,\"request\":%u,\"accepted\":%u,\"pos\":[%g,%g,%g],\"name\":",
                       (int)ev[i].type, ev[i].player, ev[i].request, ev[i].accepted, ev[i].pos[0], ev[i].pos[1], ev[i].pos[2]);
                js(ev[i].name);
                printf(",\"text\":");
                js(ev[i].text);
                printf(",\"state\":%d}\n", (int)net_state());
            }
        NetLootEvent le[8];
        int nle;
        while ((nle = net_loot_poll(le, 8)) > 0)
            for (int i = 0; i < nle; i++)
                printf("{\"ev\":\"loot\",\"type\":%d,\"origin\":%llu,\"serial\":%llu,\"who\":%llu,\"yours\":%u,\"name\":\"%s\",\"fused\":%u,\"on\":%d}\n",
                       (int)le[i].type, (unsigned long long)le[i].id.origin, (unsigned long long)le[i].id.serial,
                       (unsigned long long)le[i].who, le[i].yours, le[i].drop.name, le[i].drop.fused, net_loot_on());
        uint32_t serial = net_chat_serial();
        int count = net_chat_count();
        int fresh = (int)(serial - seen) < count ? (int)(serial - seen) : count;
        for (int i = count - fresh; i < count; i++) {
            NetChatLine l;
            if (!net_chat_get(i, &l)) continue;
            printf("{\"ev\":\"chat\",\"channel\":%d,\"from\":%u,\"from_name\":", (int)l.channel, l.from);
            js(l.from_name);
            printf(",\"to_name\":");
            js(l.to_name);
            printf(",\"text\":");
            js(l.text);
            printf("}\n");
        }
        seen = serial;
        fflush(stdout);
        if (quit) break;
        plat_sleep_ms(10);
    }
    net_disconnect();
    plat_sleep_ms(200);   // the network thread says goodbye
    fflush(stdout);
    _Exit(0);
}
