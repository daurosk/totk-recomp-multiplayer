/* Headless test clients for the multiplayer server: any number of bots in one process, driven by commands on
 * standard input, reporting every message they receive as one JSON object per line on standard output.
 *
 *   net_bot [--host 127.0.0.1] [--port 27120]
 *
 * Commands, one per line (NAME names a bot; field values of str and b32 fields are hex):
 *   connect NAME SEED NICK [claim=HEX] [features=N] [char=N] [version=N] [badsig=1]
 *                                    key from SEED (any word: the same word gives the same key), full handshake;
 *                                    prints {"ev":"key"} with the bot's public key first
 *   raw NAME                         a TCP connection that sends nothing by itself
 *   rawsend NAME HEX                 bytes as they are, sealed stream or not (for broken clients)
 *   sealraw NAME TYPE HEX            a frame of TYPE (hex number) whose payload is HEX, sealed like a real one but
 *                                    not checked by the encoder (malformed payloads past the seal)
 *   send NAME TYPE [field=value]...  one message; OBJ fields as where.state=1, vec3 as 1,2,3
 *   flood NAME COUNT TYPE [field=value]...
 *   quiet NAME 0|1                   stop or restart the bot's own pings and its answers to the server's
 *   drop NAME                        close the socket without a goodbye
 *   echo TEXT                        prints {"ev":"echo","text":TEXT} once the commands before it ran
 *   quit
 * Events: {"bot":NAME,"ev":TYPE,...fields} for each message, plus "challenge" (with the server key), "closed" and
 * "key", "error". Pings and pongs are not printed. tests/net_server_check.py drives it; docs/server.md shows how a host can
 * use it to check a server by hand.
 */
#include "../server/sv_os.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../online/proto/ow_wire.h"
#include "../online/third_party/monocypher/monocypher.h"

enum { B_FREE, B_GREETED, B_LIVE, B_RAW, B_CLOSED };

typedef struct bot {
    int state;
    char name[32];
    sv_sock s;
    ow_link link;
    ow_reader reader;
    ow_handshake hs;
    ow_identity id;
    char nick[OW_NICK_MAX + 1], claim[OW_CLAIM_MAX + 1];
    uint8_t character;
    uint32_t features, seq;
    int quiet, badsig;
    uint64_t next_ping;
} bot;

enum { BOTS = 64 };
static bot *g_bots[BOTS];
static const char *g_host = "127.0.0.1";
static uint16_t g_port = 27120;
static uint8_t g_frame[OW_FRAME_MAX];
static char g_out[1 << 20];

/* ---- JSON out ---- */

static size_t g_len;
static void put(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void put(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(g_out + g_len, sizeof g_out - g_len, fmt, ap);
    va_end(ap);
    if (k > 0) g_len = g_len + (size_t)k < sizeof g_out ? g_len + (size_t)k : sizeof g_out - 1;
}

static void put_str(const char *s, size_t n) {
    put("\"");
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') put("\\%c", c);
        else if (c < 0x20) put("\\u%04x", c);
        else put("%c", c);
    }
    put("\"");
}

static void put_hex(const uint8_t *b, size_t n) {
    put("\"");
    for (size_t i = 0; i < n; i++) put("%02x", b[i]);
    put("\"");
}

static void put_struct(const ow_struct *d, const uint8_t *s);

static void put_field(const ow_field *f, const uint8_t *s) {
    const uint8_t *p = s + f->off;
    uint16_t u16;
    uint32_t u32;
    uint64_t u64;
    int16_t i16;
    int32_t i32;
    float fl[4];
    switch (f->kind) {
    case OW_K_U8:
    case OW_K_BOOL: put("%u", *p); break;
    case OW_K_U16: memcpy(&u16, p, 2); put("%u", u16); break;
    case OW_K_U32: memcpy(&u32, p, 4); put("%lu", (unsigned long)u32); break;
    case OW_K_U64: memcpy(&u64, p, 8); put("%llu", (unsigned long long)u64); break;
    case OW_K_I16: memcpy(&i16, p, 2); put("%d", i16); break;
    case OW_K_I32: memcpy(&i32, p, 4); put("%ld", (long)i32); break;
    case OW_K_F32: memcpy(fl, p, 4); put("%.9g", fl[0]); break;
    case OW_K_VEC3: memcpy(fl, p, 12); put("[%.9g,%.9g,%.9g]", fl[0], fl[1], fl[2]); break;
    case OW_K_QUAT: memcpy(fl, p, 16); put("[%.9g,%.9g,%.9g,%.9g]", fl[0], fl[1], fl[2], fl[3]); break;
    case OW_K_B32: put_hex(p, 32); break;
    case OW_K_B64: put_hex(p, 64); break;
    case OW_K_STR: put_str((const char *)p, strlen((const char *)p)); break;
    case OW_K_BLOB: memcpy(&u16, s + f->aux, 2); put_hex(p, u16); break;
    case OW_K_OBJ: put_struct(f->sub, p); break;
    case OW_K_LIST:
        memcpy(&u16, s + f->aux, 2);
        put("[");
        for (uint16_t i = 0; i < u16; i++) {
            if (i) put(",");
            put_struct(f->sub, p + (size_t)i * f->sub->size);
        }
        put("]");
        break;
    case OW_K_COMPS: {
        const ow_comp *c = (const ow_comp *)p;
        put("[");
        for (uint8_t i = 0; i < s[f->aux]; i++) put("%s{\"kind\":%u,\"len\":%u}", i ? "," : "", c[i].kind, c[i].len);
        put("]");
        break;
    }
    }
}

static void put_fields(const ow_struct *d, const uint8_t *s) {
    for (int i = 0; i < d->nfields; i++) {
        put(",\"%s\":", d->fields[i].name);
        put_field(&d->fields[i], s);
    }
}

static void put_struct(const ow_struct *d, const uint8_t *s) {
    put("{");
    for (int i = 0; i < d->nfields; i++) {
        put("%s\"%s\":", i ? "," : "", d->fields[i].name);
        put_field(&d->fields[i], s);
    }
    put("}");
}

static void emit_begin(const bot *b, const char *ev) {
    g_len = 0;
    put("{\"bot\":");
    put_str(b ? b->name : "", b ? strlen(b->name) : 0);
    put(",\"ev\":\"%s\"", ev);
}

static void emit_end(void) {
    put("}\n");
    fwrite(g_out, 1, g_len, stdout);
    fflush(stdout);
}

static void emit_note(const bot *b, const char *ev, const char *key, const char *text) {
    emit_begin(b, ev);
    if (key) {
        put(",\"%s\":", key);
        put_str(text, strlen(text));
    }
    emit_end();
}

/* ---- field values in ---- */

static int unhex(uint8_t *out, size_t max, const char *in, size_t *n) {
    size_t len = strlen(in);
    if (len % 2 || len / 2 > max) return -1;
    for (size_t i = 0; i < len / 2; i++) {
        unsigned v;
        if (sscanf(in + 2 * i, "%2x", &v) != 1) return -1;
        out[i] = (uint8_t)v;
    }
    *n = len / 2;
    return 0;
}

static int set_field(const ow_struct *d, uint8_t *s, const char *path, const char *val) {
    const char *dot = strchr(path, '.');
    size_t nl = dot ? (size_t)(dot - path) : strlen(path);
    for (int i = 0; i < d->nfields; i++) {
        const ow_field *f = &d->fields[i];
        if (strlen(f->name) != nl || strncmp(f->name, path, nl)) continue;
        uint8_t *p = s + f->off;
        size_t n;
        char *end;
        switch (f->kind) {
        case OW_K_OBJ: return dot ? set_field(f->sub, p, dot + 1, val) : -1;
        case OW_K_U8:
        case OW_K_BOOL: *p = (uint8_t)strtoul(val, &end, 0); return 0;
        case OW_K_U16: { uint16_t v = (uint16_t)strtoul(val, &end, 0); memcpy(p, &v, 2); return 0; }
        case OW_K_U32: { uint32_t v = (uint32_t)strtoul(val, &end, 0); memcpy(p, &v, 4); return 0; }
        case OW_K_U64: { uint64_t v = strtoull(val, &end, 0); memcpy(p, &v, 8); return 0; }
        case OW_K_I16: { int16_t v = (int16_t)strtol(val, &end, 0); memcpy(p, &v, 2); return 0; }
        case OW_K_I32: { int32_t v = (int32_t)strtol(val, &end, 0); memcpy(p, &v, 4); return 0; }
        case OW_K_F32: { float v = strtof(val, &end); memcpy(p, &v, 4); return 0; }
        case OW_K_VEC3:
        case OW_K_QUAT: {
            float v[4] = { 0 };
            int k = f->kind == OW_K_VEC3 ? 3 : 4;
            const char *q = val;
            for (int j = 0; j < k; j++) {
                v[j] = strtof(q, &end);
                q = *end == ',' ? end + 1 : end;
            }
            memcpy(p, v, (size_t)k * 4);
            return 0;
        }
        case OW_K_B32: return unhex(p, 32, val, &n) || n != 32 ? -1 : 0;
        case OW_K_B64: return unhex(p, 64, val, &n) || n != 64 ? -1 : 0;
        case OW_K_STR:
            if (unhex(p, f->max, val, &n)) return -1;
            p[n] = 0;
            return 0;
        case OW_K_BLOB: {
            if (unhex(p, f->max, val, &n)) return -1;
            uint16_t v = (uint16_t)n;
            memcpy(s + f->aux, &v, 2);
            return 0;
        }
        default: return -1;
        }
    }
    return -1;
}

/* ---- bots ---- */

static bot *find(const char *name) {
    for (int i = 0; i < BOTS; i++)
        if (g_bots[i] && !strcmp(g_bots[i]->name, name)) return g_bots[i];
    return NULL;
}

static bot *make(const char *name) {
    bot *b = find(name);
    if (b) {
        if (b->s != SV_BAD_SOCK) sv_close(b->s);
        memset(b, 0, sizeof *b);
    } else {
        for (int i = 0; i < BOTS && !b; i++)
            if (!g_bots[i]) b = g_bots[i] = calloc(1, sizeof *b);
    }
    if (!b) return NULL;
    snprintf(b->name, sizeof b->name, "%s", name);
    b->s = SV_BAD_SOCK;
    return b;
}

static void close_bot(bot *b, const char *why) {
    if (b->state == B_FREE || b->state == B_CLOSED) return;
    sv_close(b->s);
    b->s = SV_BAD_SOCK;
    b->state = B_CLOSED;
    emit_note(b, "closed", "why", why);
}

static int write_all(bot *b, const uint8_t *p, size_t n) {
    uint64_t give_up = sv_now_us() + 3000000;
    while (n) {
        int k = sv_send(b->s, p, n);
        if (k < 0) return -1;
        if (k == 0) {
            if (sv_now_us() > give_up) return -1;
            sv_sleep_ms(1);
            continue;
        }
        p += k;
        n -= (size_t)k;
    }
    return 0;
}

static int send_msg(bot *b, uint16_t type, const void *msg) {
    size_t n;
    int e = ow_frame_write(&b->link, type, msg, g_frame, sizeof g_frame, &n);
    if (e) {
        emit_note(b, "error", "err", ow_strerror(e));
        return e;
    }
    if (write_all(b, g_frame, n)) {
        close_bot(b, "send failed");
        return -1;
    }
    return 0;
}

/* Waits for a non-blocking connect to finish. */
static int wait_connected(sv_sock s) {
    sv_pollfd p;
    sv_pollfd_set(&p, s, 0, 1);
    if (sv_poll(&p, 1, 3000) <= 0 || !sv_poll_writable(&p)) return -1;
    int err = 0;
    socklen_t len = sizeof err;
    getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &len);
    return err ? -1 : 0;
}

static void cmd_connect(char **argv, int argc) {
    if (argc < 4) return;
    bot *b = make(argv[1]);
    if (!b) return;
    uint8_t seed[32];
    crypto_blake2b(seed, 32, (const uint8_t *)argv[2], strlen(argv[2]));
    ow_identity_from_seed(&b->id, seed);
    snprintf(b->nick, sizeof b->nick, "%s", argv[3]);
    int version = OW_VERSION;
    for (int i = 4; i < argc; i++) {
        size_t n;
        if (!strncmp(argv[i], "claim=", 6) && !unhex((uint8_t *)b->claim, OW_CLAIM_MAX, argv[i] + 6, &n)) b->claim[n] = 0;
        else if (!strncmp(argv[i], "features=", 9)) b->features = (uint32_t)strtoul(argv[i] + 9, NULL, 0);
        else if (!strncmp(argv[i], "char=", 5)) b->character = (uint8_t)atoi(argv[i] + 5);
        else if (!strncmp(argv[i], "version=", 8)) version = atoi(argv[i] + 8);
        else if (!strncmp(argv[i], "badsig=", 7)) b->badsig = atoi(argv[i] + 7);
    }
    emit_begin(b, "key");
    put(",\"key\":");
    put_hex(b->id.pub, 32);
    emit_end();
    b->s = sv_connect(g_host, g_port);
    if (b->s == SV_BAD_SOCK || wait_connected(b->s)) {
        b->state = B_GREETED;
        close_bot(b, "connect failed");
        return;
    }
    ow_link_init(&b->link, OW_ROLE_CLIENT);
    ow_reader_init(&b->reader);
    uint8_t rnd[64];
    ow_random(rnd, sizeof rnd);
    ow_greet g;
    ow_hs_client_greet(&b->hs, rnd, "net_bot", &g);
    g.version = (uint16_t)version;
    b->state = B_GREETED;
    send_msg(b, OW_T_greet, &g);
}

static void cmd_raw(char **argv, int argc) {
    if (argc < 2) return;
    bot *b = make(argv[1]);
    if (!b) return;
    b->s = sv_connect(g_host, g_port);
    b->state = B_RAW;
    if (b->s == SV_BAD_SOCK || wait_connected(b->s)) close_bot(b, "connect failed");
    ow_link_init(&b->link, OW_ROLE_CLIENT);
    ow_reader_init(&b->reader);
}

static void cmd_rawsend(char **argv, int argc) {
    bot *b = argc >= 3 ? find(argv[1]) : NULL;
    static uint8_t buf[1 << 17];
    size_t n;
    if (!b || b->state == B_CLOSED || unhex(buf, sizeof buf, argv[2], &n)) return;
    if (write_all(b, buf, n)) close_bot(b, "send failed");
}

static void cmd_sealraw(char **argv, int argc) {
    bot *b = argc >= 3 ? find(argv[1]) : NULL;
    size_t n = 0;
    if (!b || b->state == B_CLOSED || !b->link.sealed ||
        (argc >= 4 && unhex(g_frame + OW_HEADER, OW_PAYLOAD_MAX, argv[3], &n))) {
        emit_note(b, "error", "err", "sealraw needs a sealed bot, a type and a hex payload");
        return;
    }
    uint16_t type = (uint16_t)strtoul(argv[2], NULL, 16);
    uint32_t len = (uint32_t)n + OW_TAG;
    uint8_t nonce[24];
    for (int i = 0; i < 4; i++) g_frame[i] = (uint8_t)(len >> (8 * i));
    g_frame[4] = (uint8_t)type;
    g_frame[5] = (uint8_t)(type >> 8);
    memcpy(nonce, b->link.tx.prefix, 16);
    for (int i = 0; i < 8; i++) nonce[16 + i] = (uint8_t)(b->link.tx.counter >> (8 * i));
    crypto_aead_lock(g_frame + OW_HEADER, g_frame + OW_HEADER + n, b->link.tx.key, nonce, g_frame, OW_HEADER,
                     g_frame + OW_HEADER, n);
    b->link.tx.counter++;
    if (write_all(b, g_frame, OW_HEADER + len)) close_bot(b, "send failed");
}

static int build(char **argv, int argc, int first, uint16_t *type, void *msg) {
    const ow_msg_info *m = NULL;
    for (int i = 0; i < ow_msg_count(); i++)
        if (!strcmp(ow_msg_at(i)->name, argv[first])) m = ow_msg_at(i);
    if (!m) return -1;
    memset(msg, 0, m->desc->size);
    for (int i = first + 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (!eq) return -1;
        *eq = 0;
        if (set_field(m->desc, msg, argv[i], eq + 1)) return -1;
    }
    *type = m->type;
    return 0;
}

static uint8_t g_msgbuf[1 << 18] __attribute__((aligned(16)));

static void cmd_send(char **argv, int argc, int flood) {
    int first = flood ? 3 : 2;
    bot *b = argc > first ? find(argv[1]) : NULL;
    uint16_t type;
    if (!b || b->state != B_LIVE) {
        emit_note(b, "error", "err", "not connected");
        return;
    }
    if (build(argv, argc, first, &type, g_msgbuf)) {
        emit_note(b, "error", "err", "bad message or field");
        return;
    }
    int count = flood ? atoi(argv[2]) : 1;
    for (int i = 0; i < count && b->state == B_LIVE; i++)
        if (send_msg(b, type, g_msgbuf)) return;
}

static void on_frame(bot *b, const ow_frame *f) {
    int e = ow_decode(f->type, f->payload, f->len, g_msgbuf);
    if (e) {
        emit_note(b, "error", "err", ow_strerror(e));
        close_bot(b, "bad frame from the server");
        return;
    }
    if (f->type == OW_T_challenge) {
        const ow_challenge *ch = (const ow_challenge *)g_msgbuf;
        emit_begin(b, "challenge");
        put(",\"identity\":");
        put_hex(ch->identity, 32);
        emit_end();
        ow_proof pr;
        e = ow_hs_client_challenge(&b->hs, ch, NULL, &b->id, b->nick, b->character, b->claim, b->features, &pr);
        if (!e) e = ow_hs_seal(&b->hs, &b->link);
        ow_hs_wipe(&b->hs);
        if (e) {
            emit_note(b, "error", "err", ow_strerror(e));
            close_bot(b, "handshake failed");
            return;
        }
        if (b->badsig) pr.signature[5] ^= 1;
        send_msg(b, OW_T_proof, &pr);
        return;
    }
    if (f->type == OW_T_admit) {
        const ow_admit *a = (const ow_admit *)g_msgbuf;
        ow_link_set_features(&b->link, a->features);
        b->state = B_LIVE;
        b->next_ping = sv_now_us() + 1000000;
    }
    if (f->type == OW_T_ping) {
        const ow_ping *p = (const ow_ping *)g_msgbuf;
        if (!b->quiet) {
            ow_pong r = { .seq = p->seq, .echo_us = p->sent_us, .reply_us = sv_now_us() };
            send_msg(b, OW_T_pong, &r);
        }
        return;
    }
    if (f->type == OW_T_pong) return;
    emit_begin(b, ow_msg(f->type)->name);
    put_fields(ow_msg(f->type)->desc, g_msgbuf);
    emit_end();
}

static void bot_read(bot *b) {
    static uint8_t buf[16384];
    for (;;) {
        int k = sv_recv(b->s, buf, sizeof buf);
        if (k == -1) return;
        if (k <= 0) {
            close_bot(b, k == 0 ? "server closed" : "connection reset");
            return;
        }
        if (b->state == B_RAW) {
            emit_begin(b, "raw_bytes");
            put(",\"n\":%d", k);
            emit_end();
            continue;
        }
        const uint8_t *d = buf;
        size_t n = (size_t)k;
        while (n && b->state != B_CLOSED) {
            ow_frame f;
            int r = ow_reader_take(&b->link, &b->reader, &d, &n, &f);
            if (r < 0) {
                emit_note(b, "error", "err", ow_strerror(r));
                close_bot(b, "bad stream from the server");
                return;
            }
            if (!r) break;
            on_frame(b, &f);
        }
        if (b->state == B_CLOSED) return;
    }
}

static int split(char *line, char **argv, int max) {
    int n = 0;
    for (char *s = line; *s && n < max;) {
        while (*s == ' ') s++;
        if (!*s) break;
        argv[n++] = s;
        while (*s && *s != ' ') s++;
        if (*s) *s++ = 0;
    }
    return n;
}

static int run_command(char *line) {
    char *argv[64];
    if (!strncmp(line, "echo ", 5)) {
        emit_note(NULL, "echo", "text", line + 5);
        return 0;
    }
    int argc = split(line, argv, 64);
    if (!argc) return 0;
    const char *c = argv[0];
    if (!strcmp(c, "quit")) return 1;
    if (!strcmp(c, "connect")) cmd_connect(argv, argc);
    else if (!strcmp(c, "raw")) cmd_raw(argv, argc);
    else if (!strcmp(c, "rawsend")) cmd_rawsend(argv, argc);
    else if (!strcmp(c, "sealraw")) cmd_sealraw(argv, argc);
    else if (!strcmp(c, "send")) cmd_send(argv, argc, 0);
    else if (!strcmp(c, "flood")) cmd_send(argv, argc, 1);
    else if (!strcmp(c, "quiet") && argc >= 3 && find(argv[1])) find(argv[1])->quiet = atoi(argv[2]);
    else if (!strcmp(c, "drop") && argc >= 2 && find(argv[1])) close_bot(find(argv[1]), "dropped");
    else if (!strcmp(c, "bye") && argc >= 2 && find(argv[1]) && find(argv[1])->state == B_LIVE) {
        ow_bye m;
        memset(&m, 0, sizeof m);
        send_msg(find(argv[1]), OW_T_bye, &m);
        close_bot(find(argv[1]), "said goodbye");
    } else emit_note(NULL, "error", "err", "unknown command");
    return 0;
}

int main(int argc, char **argv) {
    for (int i = 1; i + 1 < argc; i++) {
        if (!strcmp(argv[i], "--host")) g_host = argv[++i];
        else if (!strcmp(argv[i], "--port")) g_port = (uint16_t)atoi(argv[++i]);
    }
    if (sv_net_init()) return 1;
    sv_lines_start();
    char line[1 << 18];
    static sv_pollfd fds[BOTS];
    static bot *who[BOTS];
    for (;;) {
        int r;
        while ((r = sv_lines_get(line, sizeof line)) == 1)
            if (run_command(line)) break;
        if (r != 0) break;
        size_t n = 0;
        uint64_t now = sv_now_us();
        for (int i = 0; i < BOTS; i++) {
            bot *b = g_bots[i];
            if (!b || b->state == B_FREE || b->state == B_CLOSED) continue;
            if (b->state == B_LIVE && !b->quiet && now >= b->next_ping) {
                ow_ping p = { .seq = ++b->seq, .sent_us = now };
                b->next_ping = now + 1000000;
                if (send_msg(b, OW_T_ping, &p)) continue;
            }
            sv_pollfd_set(&fds[n], b->s, 1, 0);
            who[n++] = b;
        }
        sv_poll(fds, n, 10);
        for (size_t i = 0; i < n; i++)
            if (sv_poll_readable(&fds[i])) bot_read(who[i]);
    }
    /* as in the server's main: the stdin thread may hold the stream's lock, which exit() would wait for */
    fflush(stdout);
    _Exit(0);
}
