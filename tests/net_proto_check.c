/* The wire codec's checks, compiled and run by tests/net_proto_check.py.
 *
 * Described in docs/online-wire.md.
 *
 *   net_proto_check list               the message, struct and component tables, for the comparison with the doc
 *   net_proto_check check SEED         round trips of every message and component, frames in 1 to 7 byte chunks,
 *                                      truncation, flipped bytes, oversize, sealing and the handshake
 *   net_proto_check fuzz SECONDS SEED  mutated payloads and frames until SECONDS have passed
 *
 * Inputs to the decoder and the reader, and the decoder's output struct, sit right before a page the process cannot
 * touch, so a read or write past their end faults instead of passing unseen.
 */
#include "ow_wire.h"

#include <float.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../online/third_party/monocypher/monocypher-ed25519.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

/* ---- harness ---- */

static int failures;
static uint64_t rng_state;

static void fail(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stdout, "FAIL ");
    vfprintf(stdout, fmt, ap);
    fprintf(stdout, "\n");
    va_end(ap);
    fflush(stdout);
    if (++failures > 50) exit(1);
}

#define CHECK(cond, ...) do { if (!(cond)) fail(__VA_ARGS__); } while (0)

static uint64_t rnd(void) {
    uint64_t z = (rng_state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
static uint32_t below(uint32_t n) { return n ? (uint32_t)(rnd() % n) : 0; }
static void rnd_bytes(uint8_t *p, size_t n) { for (size_t i = 0; i < n; i++) p[i] = (uint8_t)rnd(); }

static double now_s(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

/* A region with a no-access page after it. guarded(n) returns n bytes ending at the guard page. */
static uint8_t *guard_base;
static size_t guard_size;

static void guard_init(void) {
    size_t page = 4096, body = 512 * 1024;
#ifdef _WIN32
    guard_base = VirtualAlloc(NULL, body + page, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    DWORD old;
    if (!guard_base || !VirtualProtect(guard_base + body, page, PAGE_NOACCESS, &old)) exit(2);
#else
    guard_base = mmap(NULL, body + page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (guard_base == MAP_FAILED || mprotect(guard_base + body, page, PROT_NONE)) exit(2);
#endif
    guard_size = body;
}

/* Copies src to the end of the guarded region. Decoder output structs are checked with a canary instead (out_*). */
static uint8_t *guarded_input(const uint8_t *src, size_t n) {
    uint8_t *p = guard_base + guard_size - n;
    if (n) memcpy(p, src, n);
    return p;
}

#define CANARY 0xA5
typedef struct { uint8_t *mem; size_t size; } outbuf;
static outbuf out_new(size_t size) {
    outbuf o = { malloc(size + 64), size };
    if (!o.mem) exit(2);
    return o;
}
static void out_arm(outbuf *o) { memset(o->mem + o->size, CANARY, 64); }
static int out_intact(const outbuf *o) {
    for (int i = 0; i < 64; i++) if (o->mem[o->size + (size_t)i] != CANARY) return 0;
    return 1;
}

/* ---- random valid structs ---- */

static float rnd_float(void) {
    static const float edges[] = { 0.0f, -0.0f, 1.0f, -1.0f, 0.5f, 1e-40f, -1e-40f, FLT_MAX, -FLT_MAX, FLT_MIN };
    if (below(4) == 0) return edges[below(sizeof edges / sizeof edges[0])];
    for (;;) {
        uint32_t bits = (uint32_t)rnd();
        if (((bits >> 23) & 0xFF) == 0xFF) continue;
        float f;
        memcpy(&f, &bits, 4);
        return f;
    }
}

/* Valid text of at most max bytes. */
static size_t rnd_text(char *p, size_t max) {
    size_t target = below(5) == 0 ? max : below((uint32_t)max + 1), n = 0;
    while (n < target) {
        uint32_t cp;
        switch (below(4)) {
        case 0: cp = 0x20 + below(0x5F); break;
        case 1: cp = 0x80 + below(0x780); break;
        case 2: do cp = 0x800 + below(0x10000 - 0x800); while (cp >= 0xD800 && cp <= 0xDFFF); break;
        default: cp = 0x10000 + below(0x100000); break;
        }
        uint8_t b[4];
        size_t k;
        if (cp < 0x80) { b[0] = (uint8_t)cp; k = 1; }
        else if (cp < 0x800) { b[0] = (uint8_t)(0xC0 | cp >> 6); b[1] = (uint8_t)(0x80 | (cp & 0x3F)); k = 2; }
        else if (cp < 0x10000) {
            b[0] = (uint8_t)(0xE0 | cp >> 12); b[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
            b[2] = (uint8_t)(0x80 | (cp & 0x3F)); k = 3;
        } else {
            b[0] = (uint8_t)(0xF0 | cp >> 18); b[1] = (uint8_t)(0x80 | ((cp >> 12) & 0x3F));
            b[2] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F)); b[3] = (uint8_t)(0x80 | (cp & 0x3F)); k = 4;
        }
        if (n + k > target) { if (target - n == 0) break; b[0] = 'a'; k = 1; }
        memcpy(p + n, b, k);
        n += k;
    }
    p[n] = 0;
    return n;
}

static uint32_t rnd_count(uint32_t max, int shift) {
    uint32_t m = max >> shift;
    if (shift && m == 0 && max) m = 1;
    switch (below(6)) {
    case 0: return 0;
    case 1: return m;
    default: return below(m + 1);
    }
}

static void gen_struct(const ow_struct *d, uint8_t *s, int shift);

static void gen_comp(ow_comp *c) {
    int pick = (int)below(10);
    if (pick < 7) {
        uint8_t kind = ow_comp_kind_at((int)below((uint32_t)ow_comp_count()));
        const ow_struct *d = ow_comp_desc(kind);
        uint8_t *body = calloc(1, d->size);
        gen_struct(d, body, 0);
        if (ow_comp_put(c, kind, body) != OW_OK) fail("component %u does not encode", kind);
        free(body);
    } else {
        /* An unknown kind, or a known kind with a body the kind's decoder refuses: both travel as bytes. */
        c->kind = pick == 7 ? ow_comp_kind_at(0) : (uint8_t)(100 + below(156));
        c->len = (uint16_t)below(OW_COMP_BODY_MAX + 1);
        rnd_bytes(c->data, c->len);
    }
}

static void gen_field(const ow_field *f, uint8_t *s, int shift) {
    uint8_t *p = s + f->off;
    switch (f->kind) {
    case OW_K_U8: *p = f->max ? (uint8_t)below(f->max + 1u) : (uint8_t)rnd(); break;
    case OW_K_BOOL: *p = (uint8_t)below(2); break;
    case OW_K_U16: { uint16_t v = f->max ? (uint16_t)below(f->max + 1u) : (uint16_t)rnd(); memcpy(p, &v, 2); break; }
    case OW_K_I16: { uint16_t v = (uint16_t)rnd(); memcpy(p, &v, 2); break; }
    case OW_K_U32: case OW_K_I32: { uint32_t v = (uint32_t)rnd(); memcpy(p, &v, 4); break; }
    case OW_K_U64: { uint64_t v = below(4) ? rnd() : (below(2) ? 0 : UINT64_MAX); memcpy(p, &v, 8); break; }
    case OW_K_F32: case OW_K_VEC3: case OW_K_QUAT: {
        int n = f->kind == OW_K_F32 ? 1 : f->kind == OW_K_VEC3 ? 3 : 4;
        for (int i = 0; i < n; i++) { float v = rnd_float(); memcpy(p + 4 * i, &v, 4); }
        break;
    }
    case OW_K_B32: rnd_bytes(p, 32); break;
    case OW_K_B64: rnd_bytes(p, 64); break;
    case OW_K_STR: rnd_text((char *)p, f->max); break;
    case OW_K_BLOB: {
        uint16_t n = (uint16_t)rnd_count(f->max, shift);
        memcpy(s + f->aux, &n, 2);
        rnd_bytes(p, n);
        break;
    }
    case OW_K_OBJ: gen_struct(f->sub, p, shift); break;
    case OW_K_LIST: {
        uint16_t n = (uint16_t)rnd_count(f->max, shift);
        memcpy(s + f->aux, &n, 2);
        for (uint16_t i = 0; i < n; i++) gen_struct(f->sub, p + (size_t)i * f->sub->size, shift);
        break;
    }
    case OW_K_COMPS: {
        uint8_t n = (uint8_t)rnd_count(f->max, shift);
        s[f->aux] = n;
        for (uint8_t i = 0; i < n; i++) gen_comp((ow_comp *)p + i);
        break;
    }
    }
}

static void gen_struct(const ow_struct *d, uint8_t *s, int shift) {
    for (uint16_t i = 0; i < d->nfields; i++) gen_field(&d->fields[i], s, shift);
}

/* A random message of type m that fits its limit, encoded into out. Returns the length. */
static size_t gen_message(const ow_msg_info *m, uint8_t *s, uint8_t *out, size_t cap) {
    for (int shift = 0; shift < 16; shift++) {
        memset(s, 0, m->desc->size);
        gen_struct(m->desc, s, shift);
        size_t n;
        int e = ow_encode(m->type, s, out, cap, &n);
        if (e == OW_OK) return n;
        if (e != OW_E_TOO_BIG) { fail("%s: generated message does not encode: %s", m->name, ow_strerror(e)); return 0; }
    }
    fail("%s: no generated message fits the limit", m->name);
    return 0;
}

/* ---- list ---- */

static void kind_text(const ow_field *f, char *buf, size_t n) {
    static const char *names[] = { "u8", "u16", "u32", "u64", "i16", "i32", "f32", "bool", "vec3", "quat", "b32",
                                   "b64" };
    switch (f->kind) {
    case OW_K_U8: case OW_K_U16:
        if (f->max) snprintf(buf, n, "%s<=%u", names[f->kind], f->max);
        else snprintf(buf, n, "%s", names[f->kind]);
        break;
    case OW_K_STR: snprintf(buf, n, "str(%u)", f->max); break;
    case OW_K_BLOB: snprintf(buf, n, "blob(%u)", f->max); break;
    case OW_K_OBJ: snprintf(buf, n, "%s", f->sub->name); break;
    case OW_K_LIST: snprintf(buf, n, "list(%u, %s)", f->max, f->sub->name); break;
    case OW_K_COMPS: snprintf(buf, n, "comps(%u)", f->max); break;
    default: snprintf(buf, n, "%s", names[f->kind]); break;
    }
}

static void print_fields(const ow_struct *d) {
    for (uint16_t i = 0; i < d->nfields; i++) {
        char k[64], name[64];
        kind_text(&d->fields[i], k, sizeof k);
        snprintf(name, sizeof name, "%s", d->fields[i].name);
        size_t L = strlen(name);
        if (L && name[L - 1] == '_') name[L - 1] = 0; /* class_ is "class" on the wire and in the doc */
        printf("  %s %s\n", name, k);
    }
}

static const char *dir_text(int d) { return d == OW_DIR_C2S ? "c2s" : d == OW_DIR_S2C ? "s2c" : "both"; }
static const char *stage_text(int s) { return s == OW_STAGE_PLAIN ? "plain" : s == OW_STAGE_SEALED ? "sealed" : "any"; }
static const char *feature_text(uint32_t f) {
    static const char *names[] = { "puppets", "saves", "actors", "duels", "economy", "companions", "sky", "emotes" };
    for (int i = 0; i < 8; i++) if (f == 1u << i) return names[i];
    return f ? "?" : "core";
}

static void list_all(void) {
    printf("version %d\n", OW_VERSION);
    printf("magic 0x%08X\n", OW_MAGIC);
    printf("payload_max %d\n", OW_PAYLOAD_MAX);
    printf("comp_body_max %d\n", OW_COMP_BODY_MAX);
    for (int i = 0; i < ow_msg_count(); i++) {
        const ow_msg_info *m = ow_msg_at(i);
        printf("message 0x%04X %s %s %s %u %u %s\n", m->type, m->name, dir_text(m->dir), stage_text(m->stage),
               ow_msg_limit(m), ow_msg_min(m), feature_text(m->feature));
        print_fields(m->desc);
    }
    /* Sub-structs, found through the fields that use them (each printed once). */
    const ow_struct *seen[64];
    int nseen = 0;
    for (int pass = 0; pass < 2; pass++) {
        int changed = 1;
        while (changed) {
            changed = 0;
            for (int i = -nseen; i < ow_msg_count() + ow_comp_count(); i++) {
                const ow_struct *d;
                if (i < 0) d = seen[-i - 1];
                else if (i < ow_msg_count()) d = ow_msg_at(i)->desc;
                else d = ow_comp_desc(ow_comp_kind_at(i - ow_msg_count()));
                for (uint16_t j = 0; j < d->nfields; j++) {
                    const ow_struct *s = d->fields[j].sub;
                    if (!s) continue;
                    int known = 0;
                    for (int k = 0; k < nseen; k++) known |= seen[k] == s;
                    if (!known && nseen < 64) { seen[nseen++] = s; changed = 1; }
                }
            }
        }
    }
    for (int i = 0; i < nseen; i++) {
        uint32_t lo, hi;
        ow_struct_bounds(seen[i], &lo, &hi);
        printf("struct %s %u %u\n", seen[i]->name, hi, lo);
        print_fields(seen[i]);
    }
    for (int i = 0; i < ow_comp_count(); i++) {
        uint8_t k = ow_comp_kind_at(i);
        uint32_t lo, hi;
        ow_struct_bounds(ow_comp_desc(k), &lo, &hi);
        printf("component %u %s %u %u\n", k, ow_comp_desc(k)->name, hi, lo);
        print_fields(ow_comp_desc(k));
    }
}

/* ---- check: tables ---- */

static void check_tables(void) {
    for (int i = 0; i < ow_msg_count(); i++) {
        const ow_msg_info *m = ow_msg_at(i);
        if (i) CHECK(ow_msg_at(i - 1)->type < m->type, "table not in ascending type order at %s", m->name);
        CHECK(ow_msg(m->type) == m, "lookup of %s", m->name);
        uint32_t lo, hi;
        ow_struct_bounds(m->desc, &lo, &hi);
        uint32_t limit = ow_msg_limit(m);
        CHECK(limit <= OW_PAYLOAD_MAX, "%s: limit %u over OW_PAYLOAD_MAX", m->name, limit);
        CHECK(lo <= limit && lo > 0, "%s: smallest encoding %u against limit %u", m->name, lo, limit);
        for (int j = 0; j < i; j++) CHECK(strcmp(ow_msg_at(j)->name, m->name), "name %s twice", m->name);
        CHECK((m->feature & (m->feature - 1)) == 0 && (m->feature & ~(uint32_t)OW_FEAT_ALL) == 0,
              "%s: feature is not one known bit", m->name);
        CHECK(!m->feature || m->stage == OW_STAGE_SEALED, "%s: an add-on type outside the sealed stage", m->name);
    }
    CHECK(ow_msg(0) == NULL && ow_msg(0xFFFF) == NULL && ow_msg(0x0008) == NULL, "unknown types are found");
    for (int i = 0; i < ow_comp_count(); i++) {
        uint8_t k = ow_comp_kind_at(i);
        uint32_t lo, hi;
        ow_struct_bounds(ow_comp_desc(k), &lo, &hi);
        CHECK(k != 0 && hi <= OW_COMP_BODY_MAX, "component %u: kind or body bound %u", k, hi);
        if (i) CHECK(ow_comp_kind_at(i - 1) < k, "component kinds not ascending at %u", k);
    }
    /* Frozen: greet starts with magic and version; refuse never changes. */
    const ow_msg_info *g = ow_msg(OW_T_greet), *r = ow_msg(OW_T_refuse);
    CHECK(g->type == 1 && g->cap == 1024 && g->desc->fields[0].kind == OW_K_U32 &&
          g->desc->fields[1].kind == OW_K_U16 && !strcmp(g->desc->fields[0].name, "magic") &&
          !strcmp(g->desc->fields[1].name, "version"), "greet's frozen head changed");
    CHECK(r->type == 2 && r->cap == 1024 && r->stage == OW_STAGE_ANY && r->desc->nfields == 4, "refuse changed");
}

/* ---- check: round trips, truncation, flips ---- */

static uint8_t enc1[OW_FRAME_MAX], enc2[OW_FRAME_MAX], mut[OW_FRAME_MAX + 64];

/* Decodes in into a guarded struct. On success re-encodes it into enc2 and stores the length in *relen. */
static int decode_guarded(uint16_t type, const uint8_t *in, size_t n, outbuf *o, size_t *relen) {
    const ow_msg_info *m = ow_msg(type);
    const uint8_t *g = guarded_input(in, n);
    out_arm(o);
    int e = ow_decode(type, g, n, o->mem);
    if (!out_intact(o)) fail("%s: decode wrote past the struct", m->name);
    if (e == OW_OK) {
        int e2 = ow_encode(type, o->mem, enc2, sizeof enc2, relen);
        if (e2) fail("%s: a decoded message does not encode: %s", m->name, ow_strerror(e2));
    }
    return e;
}

static void check_components_of(const ow_struct *d, const uint8_t *s) {
    for (uint16_t i = 0; i < d->nfields; i++) {
        const ow_field *f = &d->fields[i];
        if (f->kind == OW_K_OBJ) check_components_of(f->sub, s + f->off);
        else if (f->kind == OW_K_LIST) {
            uint16_t n;
            memcpy(&n, s + f->aux, 2);
            for (uint16_t j = 0; j < n; j++) check_components_of(f->sub, s + f->off + (size_t)j * f->sub->size);
        } else if (f->kind == OW_K_COMPS) {
            const ow_comp *c = (const ow_comp *)(s + f->off);
            for (uint8_t j = 0; j < s[f->aux]; j++) {
                const ow_struct *cd = ow_comp_desc(c[j].kind);
                uint8_t body[2048];
                int e = ow_comp_get(&c[j], body);
                if (!cd) { CHECK(e == OW_E_COMPONENT, "unknown component %u: %s", c[j].kind, ow_strerror(e)); continue; }
                if (e == OW_OK) {
                    ow_comp again;
                    CHECK(ow_comp_put(&again, c[j].kind, body) == OW_OK && again.len == c[j].len &&
                          !memcmp(again.data, c[j].data, c[j].len), "component %u does not round-trip", c[j].kind);
                }
            }
        }
    }
}

static void check_messages(int per_type) {
    for (int i = 0; i < ow_msg_count(); i++) {
        const ow_msg_info *m = ow_msg_at(i);
        uint8_t *s = calloc(1, m->desc->size);
        outbuf o = out_new(m->desc->size);
        uint32_t limit = ow_msg_limit(m), lo = ow_msg_min(m);
        size_t largest = 0;
        for (int t = 0; t < per_type; t++) {
            size_t n = gen_message(m, s, enc1, sizeof enc1), re;
            if (!n) continue;
            if (n > largest) largest = n;
            CHECK(n <= limit && n >= lo, "%s: %zu bytes against %u..%u", m->name, n, lo, limit);
            int e = decode_guarded(m->type, enc1, n, &o, &re);
            CHECK(e == OW_OK, "%s: round trip decode: %s", m->name, ow_strerror(e));
            if (e) continue;
            CHECK(re == n && !memcmp(enc1, enc2, n), "%s: round trip changed the bytes", m->name);
            check_components_of(m->desc, o.mem);
            /* The encoder of the original and of the decoded struct agree: every wire field survived. */
            size_t n0;
            CHECK(ow_encode(m->type, s, enc2, sizeof enc2, &n0) == OW_OK && n0 == n && !memcmp(enc1, enc2, n),
                  "%s: decoded struct differs from the original", m->name);
            /* Truncation: every shorter payload fails. All cuts for small payloads, a sample for large ones. */
            size_t cuts = n <= 1500 ? n : 400;
            for (size_t c = 0; c < cuts; c++) {
                size_t L = n <= 1500 ? c : (c < 8 ? n - 1 - c : below((uint32_t)n));
                e = decode_guarded(m->type, enc1, L, &o, &re);
                CHECK(e != OW_OK, "%s: truncated to %zu of %zu bytes and still decoded", m->name, L, n);
            }
            /* One byte more fails. */
            memcpy(mut, enc1, n);
            mut[n] = (uint8_t)rnd();
            e = decode_guarded(m->type, mut, n + 1, &o, &re);
            CHECK(e == OW_E_TRAILING || (n + 1 > limit && e == OW_E_TOO_BIG), "%s: a trailing byte: %s", m->name,
                  ow_strerror(e));
            /* Flipped bytes: a payload that still decodes must encode back to exactly the flipped bytes. */
            size_t flips = n <= 600 ? n : 300;
            for (size_t c = 0; c < flips; c++) {
                size_t at = n <= 600 ? c : below((uint32_t)n);
                memcpy(mut, enc1, n);
                mut[at] ^= (uint8_t)(1u << below(8));
                e = decode_guarded(m->type, mut, n, &o, &re);
                if (e == OW_OK)
                    CHECK(re == n && !memcmp(mut, enc2, n), "%s: flip at %zu decodes to something else", m->name, at);
            }
        }
        CHECK(largest > 0, "%s: no message generated", m->name);
        free(s);
        free(o.mem);
    }
}

/* ---- check: oversize and bad values ---- */

static const ow_field *first_field(const ow_struct *d, int kind) {
    for (uint16_t i = 0; i < d->nfields; i++) if (d->fields[i].kind == kind) return &d->fields[i];
    return NULL;
}

static void check_bad_values(void) {
    size_t n;
    for (int i = 0; i < ow_msg_count(); i++) {
        const ow_msg_info *m = ow_msg_at(i);
        uint8_t *s = calloc(1, m->desc->size);
        const ow_field *f;
        memset(s, 0, m->desc->size);
        CHECK(ow_encode(m->type, s, enc1, sizeof enc1, &n) == OW_OK && n == ow_msg_min(m),
              "%s: the empty message is not the smallest encoding", m->name);
        if ((f = first_field(m->desc, OW_K_STR))) {
            memset(s, 0, m->desc->size);
            memset(s + f->off, 'x', f->max);
            CHECK(ow_encode(m->type, s, enc1, sizeof enc1, &n) == OW_OK, "%s.%s full string", m->name, f->name);
            /* A string with no NUL in its max + 1 bytes is over its limit. */
            s[f->off + f->max] = 'x';
            int e = ow_encode(m->type, s, enc1, sizeof enc1, &n);
            CHECK(e == OW_E_LIMIT, "%s.%s over-long string: %s", m->name, f->name, ow_strerror(e));
            memset(s + f->off, 0, f->max + 1u);
            s[f->off] = '\n';
            CHECK(ow_encode(m->type, s, enc1, sizeof enc1, &n) == OW_E_UTF8, "%s.%s control char", m->name, f->name);
            s[f->off] = (char)0xC0; s[f->off + 1] = (char)0x80;
            CHECK(ow_encode(m->type, s, enc1, sizeof enc1, &n) == OW_E_UTF8, "%s.%s overlong", m->name, f->name);
            s[f->off] = (char)0xED; s[f->off + 1] = (char)0xA0; s[f->off + 2] = (char)0x80;
            CHECK(ow_encode(m->type, s, enc1, sizeof enc1, &n) == OW_E_UTF8, "%s.%s surrogate", m->name, f->name);
        }
        if ((f = first_field(m->desc, OW_K_LIST))) {
            memset(s, 0, m->desc->size);
            uint16_t c = (uint16_t)(f->max + 1);
            memcpy(s + f->aux, &c, 2);
            CHECK(ow_encode(m->type, s, enc1, sizeof enc1, &n) == OW_E_LIMIT, "%s.%s count over", m->name, f->name);
        }
        if ((f = first_field(m->desc, OW_K_COMPS))) {
            memset(s, 0, m->desc->size);
            s[f->aux] = (uint8_t)(f->max + 1);
            CHECK(ow_encode(m->type, s, enc1, sizeof enc1, &n) == OW_E_LIMIT, "%s.%s count over", m->name, f->name);
            s[f->aux] = 1;
            ((ow_comp *)(s + f->off))->kind = 0;
            CHECK(ow_encode(m->type, s, enc1, sizeof enc1, &n) == OW_E_RANGE, "%s.%s kind 0", m->name, f->name);
            ((ow_comp *)(s + f->off))->kind = 1;
            ((ow_comp *)(s + f->off))->len = OW_COMP_BODY_MAX + 1;
            CHECK(ow_encode(m->type, s, enc1, sizeof enc1, &n) == OW_E_LIMIT, "%s.%s body over", m->name, f->name);
        }
        for (uint16_t j = 0; j < m->desc->nfields; j++) {
            f = &m->desc->fields[j];
            memset(s, 0, m->desc->size);
            if ((f->kind == OW_K_U8 && f->max) || f->kind == OW_K_BOOL) {
                s[f->off] = (uint8_t)(f->kind == OW_K_BOOL ? 2 : f->max + 1);
                CHECK(ow_encode(m->type, s, enc1, sizeof enc1, &n) == OW_E_RANGE, "%s.%s range", m->name, f->name);
                /* The decoder refuses the same byte. */
                s[f->off] = 0;
                ow_encode(m->type, s, enc1, sizeof enc1, &n);
                size_t at = 0;
                for (uint16_t k = 0; k < j; k++) {
                    uint32_t a, b;
                    const ow_field *g = &m->desc->fields[k];
                    ow_struct d1 = { "one", g, 1, m->desc->size };
                    ow_struct_bounds(&d1, &a, &b);
                    at += a; /* fields before j are at their smallest in an empty message */
                }
                enc1[at] = (uint8_t)(f->kind == OW_K_BOOL ? 2 : f->max + 1);
                uint8_t *o = calloc(1, m->desc->size);
                CHECK(ow_decode(m->type, enc1, n, o) == OW_E_RANGE, "%s.%s decoder range", m->name, f->name);
                free(o);
            }
            if (f->kind == OW_K_F32 || f->kind == OW_K_VEC3 || f->kind == OW_K_QUAT) {
                uint32_t nan = 0x7FC00000u, inf = 0x7F800000u;
                memcpy(s + f->off, &nan, 4);
                CHECK(ow_encode(m->type, s, enc1, sizeof enc1, &n) == OW_E_FLOAT, "%s.%s NaN", m->name, f->name);
                memcpy(s + f->off, &inf, 4);
                CHECK(ow_encode(m->type, s, enc1, sizeof enc1, &n) == OW_E_FLOAT, "%s.%s inf", m->name, f->name);
            }
        }
        /* A capped type filled to its field limits does not fit, and says so. */
        if (m->cap) {
            uint32_t lo, hi;
            ow_struct_bounds(m->desc, &lo, &hi);
            if (hi > m->cap) {
                memset(s, 0, m->desc->size);
                f = first_field(m->desc, OW_K_LIST);
                uint16_t c = f->max;
                memcpy(s + f->aux, &c, 2);
                for (uint16_t k = 0; k < c; k++) {
                    uint8_t *item = s + f->off + (size_t)k * f->sub->size;
                    const ow_field *cf = first_field(f->sub, OW_K_COMPS);
                    if (cf) {
                        item[cf->aux] = (uint8_t)cf->max;
                        for (int q = 0; q < cf->max; q++) {
                            ow_comp *cc = (ow_comp *)(item + cf->off) + q;
                            cc->kind = 200;
                            cc->len = OW_COMP_BODY_MAX;
                        }
                    }
                }
                int e = ow_encode(m->type, s, enc1, sizeof enc1, &n);
                CHECK(e == OW_E_TOO_BIG, "%s: over its cap: %s", m->name, ow_strerror(e));
                e = ow_encode(m->type, s, enc1, 100, &n);
                CHECK(e == OW_E_SPACE, "%s: small buffer: %s", m->name, ow_strerror(e));
            }
        }
        /* Decoding more than the limit is refused before parsing. */
        CHECK(ow_decode(m->type, enc1, ow_msg_limit(m) + 1, s) == OW_E_TOO_BIG, "%s: decode over limit", m->name);
        free(s);
    }
    CHECK(ow_decode(0x7777, enc1, 4, mut) == OW_E_TYPE, "decode of an unknown type");
}

/* ---- check: components ---- */

static void check_components(void) {
    for (int i = 0; i < ow_comp_count(); i++) {
        uint8_t k = ow_comp_kind_at(i);
        const ow_struct *d = ow_comp_desc(k);
        uint8_t *a = calloc(1, d->size), *b = calloc(1, d->size);
        for (int t = 0; t < 200; t++) {
            memset(a, 0, d->size);
            gen_struct(d, a, 0);
            ow_comp c;
            CHECK(ow_comp_put(&c, k, a) == OW_OK, "component %s put", d->name);
            CHECK(ow_comp_get(&c, b) == OW_OK, "component %s get", d->name);
            ow_comp c2;
            CHECK(ow_comp_put(&c2, k, b) == OW_OK && c2.len == c.len && !memcmp(c.data, c2.data, c.len),
                  "component %s round trip", d->name);
            for (uint16_t L = 0; L < c.len; L++) {
                ow_comp cut = c;
                cut.len = L;
                CHECK(ow_comp_get(&cut, b) != OW_OK, "component %s cut to %u decodes", d->name, L);
            }
        }
        free(a);
        free(b);
    }
    /* One bad component among good ones: the message decodes, the good ones read, the bad one is skipped. */
    ow_self_state st, back;
    memset(&st, 0, sizeof st);
    ow_c_life life = { 30, 40 };
    ow_c_aim aim = { 1, { 0, 0, 1 } };
    ow_comp_put(&st.comps[0], OW_C_life, &life);
    st.comps[1].kind = OW_C_glide;
    st.comps[1].len = 3; /* too short for a glide */
    st.comps[2].kind = 250; /* unknown */
    st.comps[2].len = 5;
    ow_comp_put(&st.comps[3], OW_C_aim, &aim);
    st.comps_count = 4;
    size_t n;
    CHECK(ow_encode(OW_T_self_state, &st, enc1, sizeof enc1, &n) == OW_OK, "mixed components encode");
    CHECK(ow_decode(OW_T_self_state, enc1, n, &back) == OW_OK && back.comps_count == 4, "mixed components decode");
    ow_c_life l2;
    ow_c_glide g2;
    ow_c_aim a2;
    uint8_t any[64];
    CHECK(ow_comp_get(&back.comps[0], &l2) == OW_OK && l2.life == 30 && l2.life_max == 40, "good component 0");
    CHECK(ow_comp_get(&back.comps[1], &g2) == OW_E_SHORT, "bad component 1");
    CHECK(ow_comp_get(&back.comps[2], any) == OW_E_COMPONENT, "unknown component 2");
    CHECK(ow_comp_get(&back.comps[3], &a2) == OW_OK && a2.on == 1 && a2.dir[2] == 1.0f, "good component 3");
}

/* ---- check: frames, chunks and sealing ---- */

typedef struct {
    ow_identity server_id, client_id;
    ow_link client, server;
} pair;

/* Runs the whole handshake in memory, through frames, and leaves both links sealed. */
static int handshake(pair *p, const uint8_t *pin, int flip_challenge_sig, int flip_proof_sig, int zero_exchange) {
    static ow_reader rc, rs;
    ow_handshake hc, hs;
    uint8_t frame[2048], r1[64], r2[64];
    size_t n;
    ow_frame f;
    ow_link_init(&p->client, OW_ROLE_CLIENT);
    ow_link_init(&p->server, OW_ROLE_SERVER);
    ow_reader_init(&rc);
    ow_reader_init(&rs);
    rnd_bytes(r1, 64);
    rnd_bytes(r2, 64);

    ow_greet g, g2;
    if (ow_hs_client_greet(&hc, r1, "test 1.0", &g)) return -100;
    if (zero_exchange) memset(g.exchange, 0, 32);
    if (ow_frame_write(&p->client, OW_T_greet, &g, frame, sizeof frame, &n)) return -101;
    const uint8_t *d = frame;
    if (ow_reader_take(&p->server, &rs, &d, &n, &f) != 1 || f.type != OW_T_greet) return -102;
    if (ow_decode(f.type, f.payload, f.len, &g2)) return -103;

    ow_challenge ch, ch2;
    int e = ow_hs_server_greet(&hs, &p->server_id, r2, &g2, &ch);
    if (e) return e;
    if (flip_challenge_sig) ch.signature[5] ^= 1;
    if (ow_frame_write(&p->server, OW_T_challenge, &ch, frame, sizeof frame, &n)) return -104;
    if ((e = ow_hs_seal(&hs, &p->server))) return e;
    d = frame;
    if (ow_reader_take(&p->client, &rc, &d, &n, &f) != 1 || f.type != OW_T_challenge) return -105;
    if (ow_decode(f.type, f.payload, f.len, &ch2)) return -106;

    ow_proof pr, pr2;
    if ((e = ow_hs_client_challenge(&hc, &ch2, pin, &p->client_id, "Link", OW_CHAR_ZELDA, "ABCD-1234",
                                    OW_FEAT_ALL | 0x80000000u, &pr)))
        return e;
    if ((e = ow_hs_seal(&hc, &p->client))) return e;
    if (flip_proof_sig) pr.signature[9] ^= 0x10;
    if (ow_frame_write(&p->client, OW_T_proof, &pr, frame, sizeof frame, &n)) return -107;
    d = frame;
    if ((e = ow_reader_take(&p->server, &rs, &d, &n, &f)) != 1 || f.type != OW_T_proof) return -108;
    if (ow_decode(f.type, f.payload, f.len, &pr2)) return -109;
    if ((e = ow_hs_server_proof(&hs, &pr2))) return e;
    if (memcmp(hs.peer_identity, p->client_id.pub, 32) || strcmp(pr2.nickname, "Link") || strcmp(pr2.claim, "ABCD-1234") ||
        pr2.character != OW_CHAR_ZELDA || pr2.features != (OW_FEAT_ALL | 0x80000000u))
        return -110;

    ow_admit ad, ad2;
    memset(&ad, 0, sizeof ad);
    ad.account = 42;
    strcpy(ad.nickname, "Link");
    ad.server_time_us = 123456789;
    ad.rules.max_players = 4;
    ad.rules.interest_m = 250.0f;
    ad.rules.hold_ms = 30000;
    ad.rank = OW_RANK_HOST;
    ad.features = pr2.features & OW_FEAT_ALL; /* the server grants what it knows of the offer */
    if (ow_frame_write(&p->server, OW_T_admit, &ad, frame, sizeof frame, &n)) return -111;
    d = frame;
    if (ow_reader_take(&p->client, &rc, &d, &n, &f) != 1 || f.type != OW_T_admit) return -112;
    if (ow_decode(f.type, f.payload, f.len, &ad2) || ad2.account != 42 || ad2.rules.hold_ms != 30000 ||
        ad2.features != OW_FEAT_ALL || ad2.rank != OW_RANK_HOST)
        return -113;
    ow_link_set_features(&p->server, ad.features);
    ow_link_set_features(&p->client, ad2.features);
    ow_hs_wipe(&hc);
    ow_hs_wipe(&hs);
    return OW_OK;
}

static void make_pair(pair *p) {
    uint8_t seed[32];
    rnd_bytes(seed, 32);
    ow_identity_from_seed(&p->server_id, seed);
    rnd_bytes(seed, 32);
    ow_identity_from_seed(&p->client_id, seed);
    int e = handshake(p, NULL, 0, 0, 0);
    if (e) fail("handshake: %d %s", e, ow_strerror(e));
}

static void check_handshake(void) {
    pair p;
    make_pair(&p);
    CHECK(p.client.sealed && p.server.sealed, "links sealed after the handshake");
    CHECK(!memcmp(p.client.tx.key, p.server.rx.key, 32) && !memcmp(p.client.rx.key, p.server.tx.key, 32) &&
          memcmp(p.client.tx.key, p.client.rx.key, 32), "direction keys");
    pair q = p;
    int e = handshake(&q, p.server_id.pub, 0, 0, 0);
    CHECK(e == OW_OK, "pinned handshake: %s", ow_strerror(e));
    CHECK(memcmp(q.client.tx.key, p.client.tx.key, 32), "two handshakes gave the same keys");
    uint8_t other[32];
    rnd_bytes(other, 32);
    e = handshake(&q, other, 0, 0, 0);
    CHECK(e == OW_E_PIN, "wrong pin: %s", ow_strerror(e));
    e = handshake(&q, NULL, 1, 0, 0);
    CHECK(e == OW_E_AUTH, "forged challenge: %s", ow_strerror(e));
    e = handshake(&q, NULL, 0, 1, 0);
    CHECK(e == OW_E_AUTH, "forged proof: %s", ow_strerror(e));
    e = handshake(&q, NULL, 0, 0, 1);
    CHECK(e == OW_E_KEY, "zero exchange key: %s", ow_strerror(e));

    /* A proof made by another identity for this transcript does not pass as the claimed key. */
    {
        ow_handshake hc, hs;
        uint8_t r1[64], r2[64];
        rnd_bytes(r1, 64);
        rnd_bytes(r2, 64);
        ow_greet g;
        ow_challenge ch;
        ow_proof pr;
        ow_hs_client_greet(&hc, r1, "b", &g);
        ow_hs_server_greet(&hs, &p.server_id, r2, &g, &ch);
        ow_link sl, cl;
        ow_link_init(&sl, OW_ROLE_SERVER);
        ow_link_init(&cl, OW_ROLE_CLIENT);
        ow_hs_seal(&hs, &sl);
        ow_hs_client_challenge(&hc, &ch, NULL, &p.client_id, "a", 0, "", 0, &pr);
        memcpy(pr.identity, p.server_id.pub, 32);
        CHECK(ow_hs_server_proof(&hs, &pr) == OW_E_AUTH, "proof under another key");
        CHECK(ow_hs_seal(&hc, &sl) == OW_E_STEP, "client handshake sealing a server link");
        CHECK(ow_hs_seal(&hc, &cl) == OW_OK && ow_hs_seal(&hc, &cl) == OW_E_STEP, "sealing twice");
        CHECK(ow_hs_client_challenge(&hc, &ch, NULL, &p.client_id, "a", 0, "", 0, &pr) == OW_E_STEP,
              "challenge twice");
    }

    /* Version: the server refuses another version and the refusal is readable before sealing. */
    {
        ow_handshake hs;
        ow_greet g;
        uint8_t r[64];
        rnd_bytes(r, 64);
        memset(&g, 0, sizeof g);
        g.magic = OW_MAGIC;
        g.version = OW_VERSION + 1;
        ow_challenge ch;
        CHECK(ow_hs_server_greet(&hs, &p.server_id, r, &g, &ch) == OW_E_VERSION, "other version accepted");
        g.version = OW_VERSION;
        g.magic = 0x12345678;
        CHECK(ow_hs_server_greet(&hs, &p.server_id, r, &g, &ch) == OW_E_MAGIC, "bad magic accepted");
        /* A later version's greet, longer and laid out differently after its head, still yields its version. */
        uint8_t later[600];
        rnd_bytes(later, sizeof later);
        later[0] = 'T'; later[1] = 'K'; later[2] = 'R'; later[3] = 'W'; later[4] = 9; later[5] = 0;
        uint32_t magic;
        uint16_t ver;
        CHECK(ow_greet_peek(later, sizeof later, &magic, &ver) == OW_OK && magic == OW_MAGIC && ver == 9, "peek");
        uint8_t frame[2048], hdr[6];
        size_t n;
        hdr[0] = (uint8_t)sizeof later; hdr[1] = (uint8_t)(sizeof later >> 8); hdr[2] = hdr[3] = 0;
        hdr[4] = 1; hdr[5] = 0;
        memcpy(frame, hdr, 6);
        memcpy(frame + 6, later, sizeof later);
        static ow_reader rr;
        ow_reader_init(&rr);
        ow_link sl;
        ow_link_init(&sl, OW_ROLE_SERVER);
        const uint8_t *d = frame;
        size_t left = 6 + sizeof later;
        ow_frame f;
        CHECK(ow_reader_take(&sl, &rr, &d, &left, &f) == 1 && f.type == OW_T_greet, "a later greet frames");
        ow_refuse rf, rf2;
        ow_refuse_make(&rf, OW_WHY_VERSION, "0.5", "server is 0.5, update your game");
        CHECK(ow_frame_write(&sl, OW_T_refuse, &rf, frame, sizeof frame, &n) == OW_OK, "plain refuse");
        ow_link cl;
        ow_link_init(&cl, OW_ROLE_CLIENT);
        ow_reader_init(&rr);
        d = frame;
        CHECK(ow_reader_take(&cl, &rr, &d, &n, &f) == 1 && ow_decode(f.type, f.payload, f.len, &rf2) == OW_OK &&
              rf2.reason == OW_WHY_VERSION && rf2.version == OW_VERSION && !strcmp(rf2.build, "0.5"),
              "refusal read back");
        /* A long detail is cut at a character boundary. */
        char longtext[600];
        memset(longtext, 0, sizeof longtext);
        for (int i = 0; i < 150; i++) memcpy(longtext + 3 * i, "\xE2\x82\xAC", 3);
        ow_refuse_make(&rf, OW_WHY_PROTOCOL, "b", longtext);
        CHECK(strlen(rf.detail) == 240 && ow_encode(OW_T_refuse, &rf, frame, sizeof frame, &n) == OW_OK, "cut detail");
    }

    /* Ed25519 as RFC 8032 section 7.1, test 1. */
    {
        static const uint8_t seed[32] = { 0x9d, 0x61, 0xb1, 0x9d, 0xef, 0xfd, 0x5a, 0x60, 0xba, 0x84, 0x4a, 0xf4,
            0x92, 0xec, 0x2c, 0xc4, 0x44, 0x49, 0xc5, 0x69, 0x7b, 0x32, 0x69, 0x19, 0x70, 0x3b, 0xac, 0x03, 0x1c,
            0xae, 0x7f, 0x60 };
        static const uint8_t pub[32] = { 0xd7, 0x5a, 0x98, 0x01, 0x82, 0xb1, 0x0a, 0xb7, 0xd5, 0x4b, 0xfe, 0xd3,
            0xc9, 0x64, 0x07, 0x3a, 0x0e, 0xe1, 0x72, 0xf3, 0xda, 0xa6, 0x23, 0x25, 0xaf, 0x02, 0x1a, 0x68, 0xf7,
            0x07, 0x51, 0x1a };
        static const uint8_t sig[64] = { 0xe5, 0x56, 0x43, 0x00, 0xc3, 0x60, 0xac, 0x72, 0x90, 0x86, 0xe2, 0xcc,
            0x80, 0x6e, 0x82, 0x8a, 0x84, 0x87, 0x7f, 0x1e, 0xb8, 0xe5, 0xd9, 0x74, 0xd8, 0x73, 0xe0, 0x65, 0x22,
            0x49, 0x01, 0x55, 0x5f, 0xb8, 0x82, 0x15, 0x90, 0xa3, 0x3b, 0xac, 0xc6, 0x1e, 0x39, 0x70, 0x1c, 0xf9,
            0xb4, 0x6b, 0xd2, 0x5b, 0xf5, 0xf0, 0x59, 0x5b, 0xbe, 0x24, 0x65, 0x51, 0x41, 0x43, 0x8e, 0x7a, 0x10,
            0x0b };
        ow_identity id;
        uint8_t s2[64];
        ow_identity_from_seed(&id, seed);
        CHECK(!memcmp(id.pub, pub, 32), "RFC 8032 public key");
        crypto_ed25519_sign(s2, id.secret, NULL, 0);
        CHECK(!memcmp(s2, sig, 64) && crypto_ed25519_check(sig, pub, NULL, 0) == 0, "RFC 8032 signature");
    }

    int64_t off;
    uint64_t rtt;
    ow_clock_sample(1000, 5600, 1200, &off, &rtt);
    CHECK(rtt == 200 && off == 4500, "clock sample");
    uint8_t rb[64] = { 0 }, zero[64] = { 0 };
    CHECK(ow_random(rb, 64) == 0 && memcmp(rb, zero, 64), "os random");
}

/* Feeds `n` bytes to the reader in chunks of 1 to 7 bytes; collects the frames' types and payloads. */
typedef struct { uint16_t type; uint32_t len; uint8_t *bytes; } got;

static int feed_chunks(ow_link *l, ow_reader *r, const uint8_t *data, size_t n, got *out, int max, int *count) {
    *count = 0;
    while (n) {
        size_t k = 1 + below(7);
        if (k > n) k = n;
        const uint8_t *d = guarded_input(data, k);
        size_t left = k;
        for (;;) {
            ow_frame f;
            int e = ow_reader_take(l, r, &d, &left, &f);
            if (e < 0) return e;
            if (e == 0) break;
            if (*count < max) {
                out[*count].type = f.type;
                out[*count].len = f.len;
                out[*count].bytes = malloc(f.len ? f.len : 1);
                memcpy(out[*count].bytes, f.payload, f.len);
            }
            (*count)++;
        }
        if (left) fail("reader left %zu bytes of a chunk", left);
        data += k;
        n -= k;
    }
    return OW_OK;
}

static void check_stream(ow_link *tx, ow_link *rx, int sender, int sealed, int rounds) {
    static uint8_t stream[4 << 20];
    static ow_reader r;
    enum { MAXF = 4096 };
    static got gots[MAXF];
    static size_t want_off[MAXF], want_len[MAXF];
    static uint16_t want_type[MAXF];
    static uint8_t payloads[4 << 20];
    int dir = sender == OW_ROLE_CLIENT ? OW_DIR_C2S : OW_DIR_S2C;
    for (int round = 0; round < rounds; round++) {
        size_t used = 0, pused = 0;
        int nf = 0;
        for (int i = 0; i < ow_msg_count() && nf < MAXF; i++) {
            const ow_msg_info *m = ow_msg_at(i);
            if (!(m->dir & dir) || !(m->stage & (sealed ? OW_STAGE_SEALED : OW_STAGE_PLAIN))) continue;
            for (int rep = 0; rep < 3; rep++) {
                uint8_t *s = calloc(1, m->desc->size);
                size_t pn = gen_message(m, s, enc1, sizeof enc1), fn;
                if (used + OW_FRAME_MAX > sizeof stream || pused + pn > sizeof payloads) { free(s); break; }
                int e = ow_frame_write(tx, m->type, s, stream + used, sizeof stream - used, &fn);
                CHECK(e == OW_OK, "%s: frame write: %s", m->name, ow_strerror(e));
                free(s);
                if (e) continue;
                memcpy(payloads + pused, enc1, pn);
                want_off[nf] = pused;
                want_len[nf] = pn;
                want_type[nf] = m->type;
                pused += pn;
                used += fn;
                nf++;
            }
        }
        ow_reader_init(&r);
        int count;
        int e = feed_chunks(rx, &r, stream, used, gots, MAXF, &count);
        CHECK(e == OW_OK, "stream (%s, %s): %s", sender == OW_ROLE_CLIENT ? "c2s" : "s2c", sealed ? "sealed" : "plain",
              ow_strerror(e));
        CHECK(count == nf, "stream: %d frames of %d", count, nf);
        for (int i = 0; i < count && i < nf; i++) {
            CHECK(gots[i].type == want_type[i] && gots[i].len == want_len[i] &&
                  !memcmp(gots[i].bytes, payloads + want_off[i], want_len[i]), "stream frame %d differs", i);
            free(gots[i].bytes);
        }
        /* Every cut of the stream inside a frame waits for more and returns no frame early. */
        if (round == 0 && !sealed) {
            for (size_t cut = 1; cut < used && cut < 4000; cut++) {
                ow_reader_init(&r);
                ow_link copy = *rx;
                e = feed_chunks(&copy, &r, stream, cut, gots, MAXF, &count);
                int whole = 0;
                size_t pos = 0;
                while (pos < used) {
                    uint32_t L = (uint32_t)stream[pos] | (uint32_t)stream[pos + 1] << 8 |
                                 (uint32_t)stream[pos + 2] << 16 | (uint32_t)stream[pos + 3] << 24;
                    if (pos + OW_HEADER + L > cut) break;
                    pos += OW_HEADER + L;
                    whole++;
                }
                CHECK(e == OW_OK && count == whole, "cut at %zu: %d frames, %d whole (%s)", cut, count, whole,
                      ow_strerror(e));
                for (int i = 0; i < count && i < MAXF; i++) free(gots[i].bytes);
            }
        }
    }
}

/* A raw header for type t with length len. */
static void header(uint8_t *h, uint32_t len, uint16_t t) {
    h[0] = (uint8_t)len; h[1] = (uint8_t)(len >> 8); h[2] = (uint8_t)(len >> 16); h[3] = (uint8_t)(len >> 24);
    h[4] = (uint8_t)t; h[5] = (uint8_t)(t >> 8);
}

static int header_verdict(ow_link *l, uint32_t len, uint16_t t) {
    static ow_reader r;
    uint8_t h[6];
    header(h, len, t);
    ow_reader_init(&r);
    const uint8_t *d = guarded_input(h, 6);
    size_t n = 6;
    ow_frame f;
    return ow_reader_take(l, &r, &d, &n, &f);
}

static void check_headers(pair *p) {
    ow_link plain_srv, plain_cli;
    ow_link_init(&plain_srv, OW_ROLE_SERVER);
    ow_link_init(&plain_cli, OW_ROLE_CLIENT);
    for (int i = 0; i < ow_msg_count(); i++) {
        const ow_msg_info *m = ow_msg_at(i);
        uint32_t limit = ow_msg_limit(m), lo = ow_msg_min(m);
        for (int sealed = 0; sealed < 2; sealed++) {
            for (int side = 0; side < 2; side++) {
                ow_link *l = sealed ? (side ? &p->client : &p->server) : (side ? &plain_cli : &plain_srv);
                ow_link copy = *l;
                int from = side ? OW_DIR_S2C : OW_DIR_C2S;
                int stage_ok = (m->stage & (sealed ? OW_STAGE_SEALED : OW_STAGE_PLAIN)) != 0;
                int dir_ok = (m->dir & from) != 0;
                uint32_t tag = sealed ? OW_TAG : 0;
                int e = header_verdict(&copy, limit + tag, m->type);
                if (!dir_ok) CHECK(e == OW_E_DIRECTION, "%s from the wrong side: %s", m->name, ow_strerror(e));
                else if (!stage_ok) CHECK(e == OW_E_STAGE, "%s at the wrong stage: %s", m->name, ow_strerror(e));
                else {
                    CHECK(e == 0, "%s: a header at the limit: %s", m->name, ow_strerror(e));
                    e = header_verdict(&copy, limit + tag + 1, m->type);
                    CHECK(e == OW_E_TOO_BIG, "%s: one over the limit: %s", m->name, ow_strerror(e));
                    e = header_verdict(&copy, 0xFFFFFFFFu, m->type);
                    CHECK(e == OW_E_TOO_BIG, "%s: length 2^32-1: %s", m->name, ow_strerror(e));
                    e = header_verdict(&copy, lo + tag - 1, m->type);
                    CHECK(e == OW_E_TOO_SMALL, "%s: one under the smallest: %s", m->name, ow_strerror(e));
                }
            }
        }
    }
    /* A link that was granted no add-on refuses every add-on type, read or written, and passes every core one. */
    for (int i = 0; i < ow_msg_count(); i++) {
        const ow_msg_info *m = ow_msg_at(i);
        if (m->stage != OW_STAGE_SEALED) continue;
        ow_link core = m->dir & OW_DIR_C2S ? p->server : p->client;
        ow_link_set_features(&core, OW_FEAT_CORE);
        int e = header_verdict(&core, ow_msg_limit(m) + OW_TAG, m->type);
        if (m->feature) CHECK(e == OW_E_FEATURE, "%s on a link without its feature: %s", m->name, ow_strerror(e));
        else CHECK(e == 0, "%s on a core link: %s", m->name, ow_strerror(e));
        ow_link_set_features(&core, OW_FEAT_ALL & ~m->feature);
        e = header_verdict(&core, ow_msg_limit(m) + OW_TAG, m->type);
        if (m->feature) CHECK(e == OW_E_FEATURE, "%s with every other feature: %s", m->name, ow_strerror(e));
        ow_link w = m->dir & OW_DIR_C2S ? p->client : p->server;
        ow_link_set_features(&w, OW_FEAT_CORE);
        uint8_t *s = calloc(1, m->desc->size);
        size_t fn;
        e = ow_frame_write(&w, m->type, s, enc1, sizeof enc1, &fn);
        CHECK(m->feature ? e == OW_E_FEATURE : e == OW_OK, "%s written on a core link: %s", m->name, ow_strerror(e));
        free(s);
    }
    for (uint32_t t = 0; t < 0x10000; t++) {
        if (ow_msg((uint16_t)t)) continue;
        ow_link copy = plain_srv;
        int e = header_verdict(&copy, 10, (uint16_t)t);
        if (e != OW_E_TYPE) { fail("unknown type 0x%04X: %s", t, ow_strerror(e)); break; }
    }
    /* Errors stick. */
    static ow_reader r;
    uint8_t junk[12] = { 1, 0, 0, 0, 0x77, 0x77 };
    ow_reader_init(&r);
    const uint8_t *d = junk;
    size_t n = sizeof junk;
    ow_frame f;
    ow_link copy = plain_srv;
    CHECK(ow_reader_take(&copy, &r, &d, &n, &f) == OW_E_TYPE, "junk");
    uint8_t ok[64];
    size_t okn;
    ow_greet g;
    memset(&g, 0, sizeof g);
    ow_link cw = plain_cli;
    ow_frame_write(&cw, OW_T_greet, &g, ok, sizeof ok, &okn);
    d = ok;
    CHECK(ow_reader_take(&copy, &r, &d, &okn, &f) == OW_E_TYPE, "error did not stick");
    /* The writer refuses what the reader would refuse. */
    ow_link w = plain_srv;
    uint8_t buf[256];
    size_t bn;
    CHECK(ow_frame_write(&w, OW_T_greet, &g, buf, sizeof buf, &bn) == OW_E_DIRECTION, "server writes greet");
    ow_ping pg = { 1, 2 };
    CHECK(ow_frame_write(&w, OW_T_ping, &pg, buf, sizeof buf, &bn) == OW_E_STAGE, "ping before sealing");
    ow_link sw = p->client;
    CHECK(ow_frame_write(&sw, OW_T_greet, &g, buf, sizeof buf, &bn) == OW_E_STAGE, "greet after sealing");
    CHECK(ow_frame_write(&sw, 0x7777, &g, buf, sizeof buf, &bn) == OW_E_TYPE, "unknown type written");
    CHECK(ow_frame_write(&sw, OW_T_ping, &pg, buf, 10, &bn) == OW_E_SPACE, "small frame buffer");
    sw.tx.counter = UINT64_MAX;
    CHECK(ow_frame_write(&sw, OW_T_ping, &pg, buf, sizeof buf, &bn) == OW_E_COUNTER, "counter exhausted");
}

static void check_sealing(pair *p) {
    static ow_reader r;
    static uint8_t a[OW_FRAME_MAX], b[OW_FRAME_MAX];
    size_t an, bn;
    ow_ping ping = { 7, 99 };
    ow_frame f;
    /* Every flipped bit of a sealed frame, header included, is refused or waits; none is accepted. */
    for (int t = 0; t < 40; t++) {
        const ow_msg_info *m;
        do m = ow_msg_at((int)below((uint32_t)ow_msg_count()));
        while (!(m->dir & OW_DIR_C2S) || !(m->stage & OW_STAGE_SEALED));
        uint8_t *s = calloc(1, m->desc->size);
        gen_message(m, s, enc1, sizeof enc1);
        ow_link tx = p->client, rx = p->server;
        CHECK(ow_frame_write(&tx, m->type, s, a, sizeof a, &an) == OW_OK, "%s sealed write", m->name);
        free(s);
        size_t positions = an <= 400 ? an * 8 : 800;
        for (size_t i = 0; i < positions; i++) {
            size_t at = an <= 400 ? i / 8 : below((uint32_t)an);
            int bit = an <= 400 ? (int)(i % 8) : (int)below(8);
            memcpy(b, a, an);
            b[at] ^= (uint8_t)(1u << bit);
            ow_link copy = rx;
            ow_reader_init(&r);
            const uint8_t *d = guarded_input(b, an);
            size_t n = an;
            int e = ow_reader_take(&copy, &r, &d, &n, &f);
            CHECK(e != 1, "%s: a frame with bit %d of byte %zu flipped was accepted", m->name, bit, at);
        }
        ow_link copy = rx;
        ow_reader_init(&r);
        const uint8_t *d = a;
        size_t n = an;
        CHECK(ow_reader_take(&copy, &r, &d, &n, &f) == 1 && f.type == m->type, "%s: the unchanged frame", m->name);
    }
    /* Replay, reordering, dropping, the wrong direction's key. */
    ow_link tx = p->client, rx = p->server;
    ow_frame_write(&tx, OW_T_ping, &ping, a, sizeof a, &an);
    ow_frame_write(&tx, OW_T_ping, &ping, b, sizeof b, &bn);
    ow_link c1 = rx;
    ow_reader_init(&r);
    const uint8_t *d = a;
    size_t n = an;
    CHECK(ow_reader_take(&c1, &r, &d, &n, &f) == 1, "first frame");
    d = a; n = an;
    CHECK(ow_reader_take(&c1, &r, &d, &n, &f) == OW_E_SEAL, "replayed frame accepted");
    c1 = rx;
    ow_reader_init(&r);
    d = b; n = bn;
    CHECK(ow_reader_take(&c1, &r, &d, &n, &f) == OW_E_SEAL, "second frame before the first accepted");
    ow_link back = p->client; /* the client reading its own direction */
    ow_reader_init(&r);
    uint8_t own[256];
    size_t on;
    ow_link srv = p->server;
    ow_frame_write(&srv, OW_T_ping, &ping, own, sizeof own, &on);
    ow_link wrong = p->server; /* server frames read with the server's own receive key */
    d = own; n = on;
    CHECK(ow_reader_take(&wrong, &r, &d, &n, &f) == OW_E_SEAL, "a frame read with the other direction's key");
    ow_reader_init(&r);
    d = own; n = on;
    CHECK(ow_reader_take(&back, &r, &d, &n, &f) == 1, "the right direction's key");
    /* A sealed frame given to a plain link is refused for its stage. */
    ow_link plain;
    ow_link_init(&plain, OW_ROLE_SERVER);
    ow_reader_init(&r);
    d = a; n = an;
    CHECK(ow_reader_take(&plain, &r, &d, &n, &f) == OW_E_STAGE, "sealed ping on a plain link");
}

/* ---- fuzz ---- */

static size_t mutate(uint8_t *p, size_t n, size_t cap, const uint8_t *other, size_t on) {
    int rounds = 1 + (int)below(8);
    for (int i = 0; i < rounds; i++) {
        switch (below(9)) {
        case 0: if (n) p[below((uint32_t)n)] ^= (uint8_t)(1u << below(8)); break;
        case 1: if (n) p[below((uint32_t)n)] = (uint8_t)rnd(); break;
        case 2: { static const uint8_t edges[] = { 0, 1, 0x7F, 0x80, 0xFF, 0xFE, 0x20, 0xC0, 0xED, 0xF4 };
                  if (n) p[below((uint32_t)n)] = edges[below(sizeof edges)]; break; }
        case 3: if (n) n = below((uint32_t)n); break;
        case 4: { size_t k = 1 + below(8), at = below((uint32_t)n + 1);
                  if (n + k <= cap) { memmove(p + at + k, p + at, n - at); rnd_bytes(p + at, k); n += k; } break; }
        case 5: if (n) { size_t at = below((uint32_t)n), k = 1 + below((uint32_t)(n - at));
                         memmove(p + at, p + at + k, n - at - k); n -= k; } break;
        case 6: if (n) { size_t at = below((uint32_t)n), k = 1 + below((uint32_t)(n - at < 16 ? n - at : 16));
                         if (n + k <= cap) { memmove(p + at + k, p + at, n - at); memcpy(p + at, p + at + k, k); n += k; } }
                break;
        case 7: if (n && on) { size_t at = below((uint32_t)n), from = below((uint32_t)on), k = 1 + below(32);
                               if (at + k > n) k = n - at; if (from + k > on) k = on - from; memcpy(p + at, other + from, k); }
                break;
        default: /* a u16 count or length nudged by one */
                if (n >= 2) { size_t at = below((uint32_t)n - 1); uint16_t v = (uint16_t)(p[at] | p[at + 1] << 8);
                              v = (uint16_t)(v + (below(2) ? 1 : -1)); p[at] = (uint8_t)v; p[at + 1] = (uint8_t)(v >> 8); }
                break;
        }
    }
    return n;
}

static int fuzz(double seconds) {
    static uint8_t other[OW_FRAME_MAX], stream[1 << 20];
    static ow_reader r;
    double end = now_s() + seconds;
    uint64_t decodes = 0, accepted = 0, frames = 0, forgeries_tried = 0;
    outbuf outs[256];
    for (int i = 0; i < ow_msg_count(); i++) outs[i] = out_new(ow_msg_at(i)->desc->size);
    pair p;
    make_pair(&p);
    uint64_t iter = 0;
    while (failures == 0) {
        if ((iter & 255) == 0 && now_s() > end) break;
        iter++;
        int mi = (int)below((uint32_t)ow_msg_count());
        const ow_msg_info *m = ow_msg_at(mi);
        uint8_t *s = calloc(1, m->desc->size);
        size_t n = gen_message(m, s, enc1, sizeof enc1);
        free(s);
        const ow_msg_info *om = ow_msg_at((int)below((uint32_t)ow_msg_count()));
        s = calloc(1, om->desc->size);
        size_t on = gen_message(om, s, other, sizeof other);
        free(s);
        switch (below(4)) {
        case 0: case 1: {
            /* A mutated payload: refused, or canonical. */
            memcpy(mut, enc1, n);
            size_t mn = mutate(mut, n, sizeof mut, other, on), re;
            if (below(8) == 0) { mn = below(300); rnd_bytes(mut, mn); }
            int e = decode_guarded(m->type, mut, mn, &outs[mi], &re);
            decodes++;
            if (e == OW_OK) {
                accepted++;
                if (re != mn || memcmp(mut, enc2, mn)) fail("fuzz iter %llu: %s mutation decodes to other bytes",
                                                            (unsigned long long)iter, m->name);
                check_components_of(m->desc, outs[mi].mem);
            }
            break;
        }
        case 2: {
            /* Plain frames into a reader, mutated, in random chunks: never a crash, every frame within limits. */
            ow_link srv;
            ow_link_init(&srv, below(2) ? OW_ROLE_SERVER : OW_ROLE_CLIENT);
            size_t used = 0;
            for (int k = 0; k < 4 && used + 8 + n < sizeof stream; k++) {
                header(stream + used, (uint32_t)n, m->type);
                memcpy(stream + used + 6, enc1, n);
                used += 6 + n;
            }
            used = mutate(stream, used, sizeof stream, other, on);
            ow_reader_init(&r);
            size_t pos = 0;
            while (pos < used) {
                size_t k = 1 + below(below(4) ? 7 : 4096);
                if (k > used - pos) k = used - pos;
                const uint8_t *d = guarded_input(stream + pos, k);
                size_t left = k;
                int e;
                ow_frame f;
                while ((e = ow_reader_take(&srv, &r, &d, &left, &f)) == 1) {
                    frames++;
                    const ow_msg_info *fm = ow_msg(f.type);
                    if (!fm || f.len > ow_msg_limit(fm) || f.len < ow_msg_min(fm))
                        fail("fuzz iter %llu: reader passed a frame outside its limits", (unsigned long long)iter);
                    size_t re;
                    int fi = (int)(fm - ow_msg_at(0));
                    decode_guarded(f.type, f.payload, f.len, &outs[fi], &re);
                }
                if (e < 0) break;
                pos += k;
            }
            break;
        }
        default: {
            /* A sealed frame, changed: never accepted. */
            if (!(m->dir & OW_DIR_C2S) || !(m->stage & OW_STAGE_SEALED)) break;
            uint8_t *s2 = calloc(1, m->desc->size);
            if (ow_decode(m->type, enc1, n, s2) != OW_OK) { free(s2); break; }
            ow_link tx = p.client, rx = p.server;
            size_t fn;
            if (ow_frame_write(&tx, m->type, s2, stream, sizeof stream, &fn) != OW_OK) {
                fail("fuzz: sealed write of %s", m->name);
                free(s2);
                break;
            }
            free(s2);
            memcpy(mut, stream, fn);
            size_t mn = mutate(mut, fn, sizeof mut, other, on);
            if (mn == fn && !memcmp(mut, stream, fn)) break;
            ow_reader_init(&r);
            const uint8_t *d = guarded_input(mut, mn);
            size_t left = mn;
            ow_frame f;
            int e = ow_reader_take(&rx, &r, &d, &left, &f);
            forgeries_tried++;
            /* A mutation may only have cut or extended the bytes after a whole, unchanged frame. */
            if (e == 1 && !(mn >= fn && !memcmp(mut, stream, fn)))
                fail("fuzz iter %llu: a changed sealed %s frame was accepted", (unsigned long long)iter, m->name);
            break;
        }
        }
    }
    printf("fuzz: %llu iterations, %llu payload decodes (%llu accepted, all canonical), %llu frames read, "
           "%llu changed sealed frames refused\n", (unsigned long long)iter, (unsigned long long)decodes,
           (unsigned long long)accepted, (unsigned long long)frames, (unsigned long long)forgeries_tried);
    return failures ? 1 : 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    guard_init();
    if (argc >= 2 && !strcmp(argv[1], "list")) {
        list_all();
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "check")) {
        rng_state = strtoull(argv[2], NULL, 0);
        int per = argc >= 4 ? atoi(argv[3]) : 60;
        check_tables();
        printf("tables: %d messages, %d components\n", ow_msg_count(), ow_comp_count());
        check_handshake();
        printf("handshake: done\n");
        check_messages(per);
        printf("messages: %d round trips per type with truncation and flips\n", per);
        check_bad_values();
        printf("bad values and oversize: done\n");
        check_components();
        printf("components: done\n");
        pair p;
        make_pair(&p);
        check_headers(&p);
        printf("headers: done\n");
        ow_link pc, ps;
        ow_link_init(&pc, OW_ROLE_CLIENT);
        ow_link_init(&ps, OW_ROLE_SERVER);
        check_stream(&pc, &ps, OW_ROLE_CLIENT, 0, 4);
        check_stream(&ps, &pc, OW_ROLE_SERVER, 0, 4);
        check_stream(&p.client, &p.server, OW_ROLE_CLIENT, 1, 4);
        check_stream(&p.server, &p.client, OW_ROLE_SERVER, 1, 4);
        printf("streams in 1 to 7 byte chunks: done\n");
        check_sealing(&p);
        printf("sealing: done\n");
        printf(failures ? "check: %d failures\n" : "check: passed\n", failures);
        return failures ? 1 : 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "fuzz")) {
        rng_state = strtoull(argv[3], NULL, 0);
        return fuzz(atof(argv[2]));
    }
    fprintf(stderr, "usage: net_proto_check list | check SEED [PER_TYPE] | fuzz SECONDS SEED\n");
    return 2;
}
