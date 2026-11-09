/* The server's entry point: arguments, the data folder, then the poll loop with the host's console.
 *
 *   totk-server [--data DIR] [--port N] [--bind ADDR]
 *
 * docs/server.md describes running it, its settings and its console.
 */
#include "sv.h"

#include <signal.h>
#include <stdlib.h>
#include <string.h>

static volatile sig_atomic_t g_stop;

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

static void usage(void) {
    printf("usage: totk-server [--data DIR] [--port N] [--bind ADDR]\n"
           "  --data DIR   the data folder: settings, key, accounts, bans, log (default: server-data)\n"
           "  --port N     listen on port N instead of the one in server.toml\n"
           "  --bind ADDR  listen on ADDR instead of the one in server.toml\n");
}

int main(int argc, char **argv) {
    const char *dir = "server-data", *bind_addr = NULL;
    long port = -1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--data") && i + 1 < argc) dir = argv[++i];
        else if (!strcmp(argv[i], "--port") && i + 1 < argc) port = strtol(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--bind") && i + 1 < argc) bind_addr = argv[++i];
        else {
            usage();
            return strcmp(argv[i], "--help") && strcmp(argv[i], "-h") ? 2 : 0;
        }
    }
    setvbuf(stdout, NULL, _IOLBF, 4096);
    if (sv_net_init()) {
        fprintf(stderr, "cannot start networking\n");
        return 1;
    }
    if (sv_mkdir(dir)) {
        fprintf(stderr, "cannot make the data folder %s\n", dir);
        return 1;
    }
    sv_config cfg;
    sv_config_defaults(&cfg);
    char path[600];
    snprintf(path, sizeof path, "%s/server.toml", dir);
    if (sv_config_load(&cfg, path)) return 1;
    if (port >= 0) {
        if (port < 1 || port > 65535) {
            fprintf(stderr, "--port takes 1 to 65535\n");
            return 2;
        }
        cfg.port = (uint16_t)port;
    }
    if (bind_addr) snprintf(cfg.bind, sizeof cfg.bind, "%s", bind_addr);
    if (sv_store_open(dir, &cfg)) return 1;
    if (sv_room_start(&cfg)) return 1;

    char key[65];
    sv_hex(key, g_store.identity.pub, 32);
    sv_log("%s", SV_BUILD);
    sv_log("\"%s\" listening on %s port %u (TCP), protocol %d, up to %d players", cfg.name, cfg.bind, cfg.port,
           OW_VERSION, cfg.max_players);
    sv_log("server key %s", key);
    sv_log("data folder %s; type help for the console's commands", dir);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    sv_lines_start();
    char line[2048];
    while (!g_stop) {
        sv_room_poll(20);
        int r;
        while (!g_stop && (r = sv_lines_get(line, sizeof line)) == 1)
            if (sv_console(line)) g_stop = 1;
    }
    sv_log("stopping");
    sv_room_stop();
    sv_store_close();
    return 0;
}
