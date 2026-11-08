/* The wire codec: descriptors built from ow_messages.h, encoding, canonical decoding, the message and component
 * tables.
 *
 * Described in docs/online-wire.md ("Fields", "Components", "Limits").
 * Every check the decoder makes, the encoder makes too, so whatever one side writes the other side reads, and a
 * payload that decodes encodes back to the same bytes (tests/net_proto_check.py relies on that in its fuzz run).
 */
#include "ow_wire.h"

#include <string.h>

/* ---- descriptors ---- */

#define OW_AUX_U8(T, n) 0
#define OW_AUX_BOOL(T, n) 0
#define OW_AUX_U16(T, n) 0
#define OW_AUX_U32(T, n) 0
#define OW_AUX_U64(T, n) 0
#define OW_AUX_I16(T, n) 0
#define OW_AUX_I32(T, n) 0
#define OW_AUX_F32(T, n) 0
#define OW_AUX_VEC3(T, n) 0
#define OW_AUX_QUAT(T, n) 0
#define OW_AUX_B32(T, n) 0
#define OW_AUX_B64(T, n) 0
#define OW_AUX_STR(T, n) 0
#define OW_AUX_OBJ(T, n) 0
#define OW_AUX_BLOB(T, n) (uint32_t)offsetof(T, n##_len)
#define OW_AUX_LIST(T, n) (uint32_t)offsetof(T, n##_count)
#define OW_AUX_COMPS(T, n) (uint32_t)offsetof(T, n##_count)

#define OW_SUB_U8(s) NULL
#define OW_SUB_BOOL(s) NULL
#define OW_SUB_U16(s) NULL
#define OW_SUB_U32(s) NULL
#define OW_SUB_U64(s) NULL
#define OW_SUB_I16(s) NULL
#define OW_SUB_I32(s) NULL
#define OW_SUB_F32(s) NULL
#define OW_SUB_VEC3(s) NULL
#define OW_SUB_QUAT(s) NULL
#define OW_SUB_B32(s) NULL
#define OW_SUB_B64(s) NULL
#define OW_SUB_STR(s) NULL
#define OW_SUB_BLOB(s) NULL
#define OW_SUB_COMPS(s) NULL
#define OW_SUB_OBJ(s) &ow_desc_##s
#define OW_SUB_LIST(s) &ow_desc_##s

#define OW_FIELD(T, K, n, m, s) { OW_K_##K, (uint16_t)(m), (uint32_t)offsetof(T, n), OW_AUX_##K(T, n), OW_SUB_##K(s), #n },
#define OW_COUNT(a) ((uint16_t)(sizeof(a) / sizeof((a)[0])))

#define OW_DESC_SUB_FWD(name) static const ow_struct ow_desc_##name;
OW_SUBSTRUCTS(OW_DESC_SUB_FWD)

#define OW_DESC_SUB(name) \
    static const ow_field ow_fields_##name[] = { OW_F_##name(OW_FIELD, ow_##name) }; \
    static const ow_struct ow_desc_##name = { #name, ow_fields_##name, OW_COUNT(ow_fields_##name), sizeof(ow_##name) };
OW_SUBSTRUCTS(OW_DESC_SUB)

#define OW_DESC_MSG(id, name, dir, stage, cap, feat) \
    static const ow_field ow_fields_##name[] = { OW_F_##name(OW_FIELD, ow_##name) }; \
    static const ow_struct ow_desc_##name = { #name, ow_fields_##name, OW_COUNT(ow_fields_##name), sizeof(ow_##name) };
OW_MESSAGES(OW_DESC_MSG)

#define OW_DESC_COMP(kind, name) \
    static const ow_field ow_fields_c_##name[] = { OW_F_c_##name(OW_FIELD, ow_c_##name) }; \
    static const ow_struct ow_desc_c_##name = { #name, ow_fields_c_##name, OW_COUNT(ow_fields_c_##name), \
                                                sizeof(ow_c_##name) };
OW_COMPONENTS(OW_DESC_COMP)

#define OW_INFO(id, name, dir, stage, cap, feat) \
    { id, #name, OW_DIR_##dir, OW_STAGE_##stage, cap, OW_FEAT_##feat, &ow_desc_##name },
static const ow_msg_info ow_msgs[] = { OW_MESSAGES(OW_INFO) };

typedef struct { uint8_t kind; const ow_struct *desc; } ow_comp_info;
#define OW_CINFO(kind, name) { kind, &ow_desc_c_##name },
static const ow_comp_info ow_comps[] = { OW_COMPONENTS(OW_CINFO) };

int ow_msg_count(void) { return (int)OW_COUNT(ow_msgs); }

const ow_msg_info *ow_msg_at(int i) {
    return i >= 0 && i < ow_msg_count() ? &ow_msgs[i] : NULL;
}

const ow_msg_info *ow_msg(uint16_t type) {
    /* The table is in ascending type order (tests/net_proto_check.py checks it). */
    int lo = 0, hi = ow_msg_count() - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (ow_msgs[mid].type == type) return &ow_msgs[mid];
        if (ow_msgs[mid].type < type) lo = mid + 1;
        else hi = mid - 1;
    }
    return NULL;
}

void ow_struct_bounds(const ow_struct *d, uint32_t *min, uint32_t *max) {
    uint32_t lo = 0, hi = 0;
    for (uint16_t i = 0; i < d->nfields; i++) {
        const ow_field *f = &d->fields[i];
        uint32_t a = 0, b = 0;
        switch (f->kind) {
        case OW_K_U8: case OW_K_BOOL: a = b = 1; break;
        case OW_K_U16: case OW_K_I16: a = b = 2; break;
        case OW_K_U32: case OW_K_I32: case OW_K_F32: a = b = 4; break;
        case OW_K_U64: a = b = 8; break;
        case OW_K_VEC3: a = b = 12; break;
        case OW_K_QUAT: a = b = 16; break;
        case OW_K_B32: a = b = 32; break;
        case OW_K_B64: a = b = 64; break;
        case OW_K_STR: case OW_K_BLOB: a = 2; b = 2u + f->max; break;
        case OW_K_OBJ: ow_struct_bounds(f->sub, &a, &b); break;
        case OW_K_LIST: {
            uint32_t sa, sb;
            ow_struct_bounds(f->sub, &sa, &sb);
            a = 2; b = 2u + (uint32_t)f->max * sb;
            break;
        }
        case OW_K_COMPS: a = 1; b = 1u + (uint32_t)f->max * (3u + OW_COMP_BODY_MAX); break;
        }
        lo += a; hi += b;
    }
    *min = lo; *max = hi;
}

uint32_t ow_msg_limit(const ow_msg_info *m) {
    uint32_t lo, hi;
    if (m->cap) return m->cap;
    ow_struct_bounds(m->desc, &lo, &hi);
    return hi;
}

uint32_t ow_msg_min(const ow_msg_info *m) {
    uint32_t lo, hi;
    ow_struct_bounds(m->desc, &lo, &hi);
    return lo;
}

int ow_comp_count(void) { return (int)OW_COUNT(ow_comps); }
uint8_t ow_comp_kind_at(int i) { return i >= 0 && i < ow_comp_count() ? ow_comps[i].kind : 0; }

const ow_struct *ow_comp_desc(uint8_t kind) {
    for (int i = 0; i < ow_comp_count(); i++)
        if (ow_comps[i].kind == kind) return ow_comps[i].desc;
    return NULL;
}

/* ---- scalar helpers ---- */

static int float_ok(uint32_t bits) { return ((bits >> 23) & 0xFF) != 0xFF; }

/* Strict UTF-8 (no overlong forms, no surrogates, nothing over U+10FFFF) and no C0 control characters or DEL. */
static int text_ok(const uint8_t *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        uint8_t c = s[i];
        if (c < 0x80) {
            if (c < 0x20 || c == 0x7F) return 0;
            i++;
            continue;
        }
        size_t k;
        uint32_t cp, lo;
        if (c >= 0xC2 && c <= 0xDF) { k = 1; cp = c & 0x1F; lo = 0x80; }
        else if (c >= 0xE0 && c <= 0xEF) { k = 2; cp = c & 0x0F; lo = 0x800; }
        else if (c >= 0xF0 && c <= 0xF4) { k = 3; cp = c & 0x07; lo = 0x10000; }
        else return 0;
        if (n - i - 1 < k) return 0;
        for (size_t j = 1; j <= k; j++) {
            uint8_t t = s[i + j];
            if ((t & 0xC0) != 0x80) return 0;
            cp = (cp << 6) | (t & 0x3F);
        }
        if (cp < lo || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
        i += k + 1;
    }
    return 1;
}

typedef struct { uint8_t *p; size_t cap, pos; } wbuf;
typedef struct { const uint8_t *p; size_t len, pos; } rbuf;

static int put(wbuf *w, const void *src, size_t n) {
    if (w->cap - w->pos < n) return OW_E_SPACE;
    memcpy(w->p + w->pos, src, n);
    w->pos += n;
    return OW_OK;
}

static int put_le(wbuf *w, uint64_t v, int n) {
    uint8_t b[8];
    for (int i = 0; i < n; i++) b[i] = (uint8_t)(v >> (8 * i));
    return put(w, b, (size_t)n);
}

static int get(rbuf *r, void *dst, size_t n) {
    if (r->len - r->pos < n) return OW_E_SHORT;
    memcpy(dst, r->p + r->pos, n);
    r->pos += n;
    return OW_OK;
}

static int get_le(rbuf *r, uint64_t *v, int n) {
    if (r->len - r->pos < (size_t)n) return OW_E_SHORT;
    uint64_t x = 0;
    for (int i = 0; i < n; i++) x |= (uint64_t)r->p[r->pos + (size_t)i] << (8 * i);
    r->pos += (size_t)n;
    *v = x;
    return OW_OK;
}

static int put_floats(wbuf *w, const float *v, int n) {
    for (int i = 0; i < n; i++) {
        uint32_t bits;
        memcpy(&bits, &v[i], 4);
        if (!float_ok(bits)) return OW_E_FLOAT;
        int e = put_le(w, bits, 4);
        if (e) return e;
    }
    return OW_OK;
}

static int get_floats(rbuf *r, float *v, int n) {
    for (int i = 0; i < n; i++) {
        uint64_t x;
        int e = get_le(r, &x, 4);
        if (e) return e;
        uint32_t bits = (uint32_t)x;
        if (!float_ok(bits)) return OW_E_FLOAT;
        memcpy(&v[i], &bits, 4);
    }
    return OW_OK;
}

/* ---- encode ---- */

static int enc(const ow_struct *d, const uint8_t *s, wbuf *w);

static int enc_field(const ow_field *f, const uint8_t *s, wbuf *w) {
    const uint8_t *p = s + f->off;
    switch (f->kind) {
    case OW_K_U8:
        if (f->max && *p > f->max) return OW_E_RANGE;
        return put(w, p, 1);
    case OW_K_BOOL:
        if (*p > 1) return OW_E_RANGE;
        return put(w, p, 1);
    case OW_K_U16: {
        uint16_t v;
        memcpy(&v, p, 2);
        if (f->max && v > f->max) return OW_E_RANGE;
        return put_le(w, v, 2);
    }
    case OW_K_I16: { uint16_t v; memcpy(&v, p, 2); return put_le(w, v, 2); }
    case OW_K_U32: case OW_K_I32: { uint32_t v; memcpy(&v, p, 4); return put_le(w, v, 4); }
    case OW_K_U64: { uint64_t v; memcpy(&v, p, 8); return put_le(w, v, 8); }
    case OW_K_F32: return put_floats(w, (const float *)p, 1);
    case OW_K_VEC3: return put_floats(w, (const float *)p, 3);
    case OW_K_QUAT: return put_floats(w, (const float *)p, 4);
    case OW_K_B32: return put(w, p, 32);
    case OW_K_B64: return put(w, p, 64);
    case OW_K_STR: {
        size_t n = 0;
        while (n <= f->max && p[n]) n++;
        if (n > f->max) return OW_E_LIMIT;
        if (!text_ok(p, n)) return OW_E_UTF8;
        int e = put_le(w, n, 2);
        return e ? e : put(w, p, n);
    }
    case OW_K_BLOB: {
        uint16_t n;
        memcpy(&n, s + f->aux, 2);
        if (n > f->max) return OW_E_LIMIT;
        int e = put_le(w, n, 2);
        return e ? e : put(w, p, n);
    }
    case OW_K_OBJ: return enc(f->sub, p, w);
    case OW_K_LIST: {
        uint16_t n;
        memcpy(&n, s + f->aux, 2);
        if (n > f->max) return OW_E_LIMIT;
        int e = put_le(w, n, 2);
        for (uint16_t i = 0; !e && i < n; i++) e = enc(f->sub, p + (size_t)i * f->sub->size, w);
        return e;
    }
    case OW_K_COMPS: {
        uint8_t n = s[f->aux];
        if (n > f->max) return OW_E_LIMIT;
        int e = put(w, &n, 1);
        for (uint8_t i = 0; !e && i < n; i++) {
            const ow_comp *c = (const ow_comp *)p + i;
            if (c->kind == 0) return OW_E_RANGE;
            if (c->len > OW_COMP_BODY_MAX) return OW_E_LIMIT;
            e = put(w, &c->kind, 1);
            if (!e) e = put_le(w, c->len, 2);
            if (!e) e = put(w, c->data, c->len);
        }
        return e;
    }
    }
    return OW_E_RANGE;
}

static int enc(const ow_struct *d, const uint8_t *s, wbuf *w) {
    for (uint16_t i = 0; i < d->nfields; i++) {
        int e = enc_field(&d->fields[i], s, w);
        if (e) return e;
    }
    return OW_OK;
}

int ow_struct_encode(const ow_struct *d, const void *s, uint8_t *out, size_t cap, size_t *len) {
    wbuf w = { out, cap, 0 };
    int e = enc(d, (const uint8_t *)s, &w);
    if (e) return e;
    *len = w.pos;
    return OW_OK;
}

/* ---- decode ---- */

static int dec(const ow_struct *d, uint8_t *s, rbuf *r);

static int dec_field(const ow_field *f, uint8_t *s, rbuf *r) {
    uint8_t *p = s + f->off;
    uint64_t v;
    int e;
    switch (f->kind) {
    case OW_K_U8:
        if ((e = get(r, p, 1))) return e;
        return f->max && *p > f->max ? OW_E_RANGE : OW_OK;
    case OW_K_BOOL:
        if ((e = get(r, p, 1))) return e;
        return *p > 1 ? OW_E_RANGE : OW_OK;
    case OW_K_U16: case OW_K_I16: {
        if ((e = get_le(r, &v, 2))) return e;
        uint16_t x = (uint16_t)v;
        if (f->kind == OW_K_U16 && f->max && x > f->max) return OW_E_RANGE;
        memcpy(p, &x, 2);
        return OW_OK;
    }
    case OW_K_U32: case OW_K_I32: {
        if ((e = get_le(r, &v, 4))) return e;
        uint32_t x = (uint32_t)v;
        memcpy(p, &x, 4);
        return OW_OK;
    }
    case OW_K_U64:
        if ((e = get_le(r, &v, 8))) return e;
        memcpy(p, &v, 8);
        return OW_OK;
    case OW_K_F32: return get_floats(r, (float *)p, 1);
    case OW_K_VEC3: return get_floats(r, (float *)p, 3);
    case OW_K_QUAT: return get_floats(r, (float *)p, 4);
    case OW_K_B32: return get(r, p, 32);
    case OW_K_B64: return get(r, p, 64);
    case OW_K_STR: {
        if ((e = get_le(r, &v, 2))) return e;
        if (v > f->max) return OW_E_LIMIT;
        if (r->len - r->pos < v) return OW_E_SHORT;
        if (!text_ok(r->p + r->pos, (size_t)v)) return OW_E_UTF8;
        e = get(r, p, (size_t)v);
        p[v] = 0;
        return e;
    }
    case OW_K_BLOB: {
        if ((e = get_le(r, &v, 2))) return e;
        if (v > f->max) return OW_E_LIMIT;
        uint16_t n = (uint16_t)v;
        memcpy(s + f->aux, &n, 2);
        return get(r, p, n);
    }
    case OW_K_OBJ: return dec(f->sub, p, r);
    case OW_K_LIST: {
        if ((e = get_le(r, &v, 2))) return e;
        if (v > f->max) return OW_E_LIMIT;
        uint16_t n = (uint16_t)v;
        memcpy(s + f->aux, &n, 2);
        for (uint16_t i = 0; i < n; i++)
            if ((e = dec(f->sub, p + (size_t)i * f->sub->size, r))) return e;
        return OW_OK;
    }
    case OW_K_COMPS: {
        uint8_t n;
        if ((e = get(r, &n, 1))) return e;
        if (n > f->max) return OW_E_LIMIT;
        s[f->aux] = n;
        for (uint8_t i = 0; i < n; i++) {
            ow_comp *c = (ow_comp *)p + i;
            if ((e = get(r, &c->kind, 1))) return e;
            if (c->kind == 0) return OW_E_RANGE;
            if ((e = get_le(r, &v, 2))) return e;
            if (v > OW_COMP_BODY_MAX) return OW_E_LIMIT;
            c->len = (uint16_t)v;
            if ((e = get(r, c->data, c->len))) return e;
        }
        return OW_OK;
    }
    }
    return OW_E_RANGE;
}

static int dec(const ow_struct *d, uint8_t *s, rbuf *r) {
    for (uint16_t i = 0; i < d->nfields; i++) {
        int e = dec_field(&d->fields[i], s, r);
        if (e) return e;
    }
    return OW_OK;
}

int ow_struct_decode(const ow_struct *d, const uint8_t *in, size_t len, void *s) {
    rbuf r = { in, len, 0 };
    memset(s, 0, d->size);
    int e = dec(d, (uint8_t *)s, &r);
    if (e) return e;
    return r.pos == len ? OW_OK : OW_E_TRAILING;
}

int ow_encode(uint16_t type, const void *msg, uint8_t *out, size_t cap, size_t *len) {
    const ow_msg_info *m = ow_msg(type);
    if (!m) return OW_E_TYPE;
    uint32_t limit = ow_msg_limit(m);
    size_t n;
    /* Encode into at most limit + 1 bytes so an oversize message stops early and reports as such. */
    int e = ow_struct_encode(m->desc, msg, out, cap < (size_t)limit + 1 ? cap : (size_t)limit + 1, &n);
    if (e == OW_E_SPACE && cap > limit) return OW_E_TOO_BIG;
    if (e) return e;
    if (n > limit) return OW_E_TOO_BIG;
    *len = n;
    return OW_OK;
}

int ow_decode(uint16_t type, const uint8_t *in, size_t len, void *msg) {
    const ow_msg_info *m = ow_msg(type);
    if (!m) return OW_E_TYPE;
    if (len > ow_msg_limit(m)) return OW_E_TOO_BIG;
    return ow_struct_decode(m->desc, in, len, msg);
}

/* ---- components ---- */

int ow_comp_put(ow_comp *c, uint8_t kind, const void *body) {
    const ow_struct *d = ow_comp_desc(kind);
    if (!d) return OW_E_COMPONENT;
    size_t n;
    int e = ow_struct_encode(d, body, c->data, sizeof c->data, &n);
    if (e) return e == OW_E_SPACE ? OW_E_LIMIT : e;
    c->kind = kind;
    c->len = (uint16_t)n;
    return OW_OK;
}

int ow_comp_get(const ow_comp *c, void *body) {
    const ow_struct *d = ow_comp_desc(c->kind);
    if (!d) return OW_E_COMPONENT;
    if (c->len > OW_COMP_BODY_MAX) return OW_E_LIMIT;
    return ow_struct_decode(d, c->data, c->len, body);
}

/* ---- errors ---- */

const char *ow_strerror(int err) {
    switch (err) {
    case OW_OK: return "ok";
    case OW_E_SHORT: return "payload ends inside a field";
    case OW_E_TRAILING: return "bytes after the last field";
    case OW_E_RANGE: return "value out of range";
    case OW_E_FLOAT: return "float is NaN or infinite";
    case OW_E_UTF8: return "string is not clean UTF-8";
    case OW_E_LIMIT: return "count or length over the field limit";
    case OW_E_TOO_BIG: return "frame over the type's limit";
    case OW_E_TOO_SMALL: return "frame under the type's smallest size";
    case OW_E_TYPE: return "unknown message type";
    case OW_E_DIRECTION: return "message type sent the wrong way";
    case OW_E_STAGE: return "message type not allowed at this stage";
    case OW_E_SEAL: return "sealed frame did not check";
    case OW_E_COUNTER: return "frame counter exhausted";
    case OW_E_SPACE: return "output buffer too small";
    case OW_E_COMPONENT: return "unknown component kind";
    case OW_E_VERSION: return "protocol version differs";
    case OW_E_MAGIC: return "not a TotK Recomp Online client";
    case OW_E_AUTH: return "handshake signature did not check";
    case OW_E_KEY: return "exchange key is unusable";
    case OW_E_STEP: return "handshake step out of order";
    case OW_E_PIN: return "server identity is not the expected one";
    case OW_E_FEATURE: return "message of a feature the link does not have";
    }
    return "unknown error";
}
