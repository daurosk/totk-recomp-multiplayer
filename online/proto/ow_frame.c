/* Frames, the stream reader and sealing.
 *
 * Described in docs/online-wire.md ("Frames", "Sealing"). A frame is u32 length, u16 type, then `length`
 * bytes. The header is checked against the type's direction, stage, feature and limits
 * before a payload byte is buffered. Once the link is sealed, each payload is encrypted with XChaCha20-Poly1305
 * (Monocypher's crypto_aead_lock), the 6 header bytes are its additional data, and the 24-byte nonce is the
 * direction's 16-byte prefix followed by its frame counter, so a frame that is changed, dropped, replayed or moved
 * fails its tag.
 */
#include "ow_wire.h"

#include <string.h>

#include "../third_party/monocypher/monocypher.h"

void ow_link_init(ow_link *l, int role) {
    memset(l, 0, sizeof *l);
    l->role = (uint8_t)role;
}

void ow_link_wipe(ow_link *l) { crypto_wipe(l, sizeof *l); }

void ow_link_set_features(ow_link *l, uint32_t features) { l->features = features; }

static void nonce_of(const ow_seal_dir *d, uint8_t nonce[24]) {
    memcpy(nonce, d->prefix, 16);
    for (int i = 0; i < 8; i++) nonce[16 + i] = (uint8_t)(d->counter >> (8 * i));
}

static void put_header(uint8_t *h, uint32_t len, uint16_t type) {
    for (int i = 0; i < 4; i++) h[i] = (uint8_t)(len >> (8 * i));
    h[4] = (uint8_t)type;
    h[5] = (uint8_t)(type >> 8);
}

/* Whether a frame of type m may be sent by `sender` on link l. */
static int frame_allowed(const ow_msg_info *m, int sender, const ow_link *l) {
    int dir = sender == OW_ROLE_CLIENT ? OW_DIR_C2S : OW_DIR_S2C;
    if (!(m->dir & dir)) return OW_E_DIRECTION;
    if (!(m->stage & (l->sealed ? OW_STAGE_SEALED : OW_STAGE_PLAIN))) return OW_E_STAGE;
    if (m->feature && !(l->features & m->feature)) return OW_E_FEATURE;
    return OW_OK;
}

int ow_frame_write(ow_link *l, uint16_t type, const void *msg, uint8_t *out, size_t cap, size_t *len) {
    const ow_msg_info *m = ow_msg(type);
    if (!m) return OW_E_TYPE;
    int e = frame_allowed(m, l->role, l);
    if (e) return e;
    size_t tag = l->sealed ? OW_TAG : 0;
    if (cap < OW_HEADER + tag) return OW_E_SPACE;
    size_t n;
    if ((e = ow_encode(type, msg, out + OW_HEADER, cap - OW_HEADER - tag, &n))) return e;
    put_header(out, (uint32_t)(n + tag), type);
    if (l->sealed) {
        uint8_t nonce[24];
        if (l->tx.counter == UINT64_MAX) return OW_E_COUNTER;
        nonce_of(&l->tx, nonce);
        crypto_aead_lock(out + OW_HEADER, out + OW_HEADER + n, l->tx.key, nonce, out, OW_HEADER, out + OW_HEADER, n);
        l->tx.counter++;
    }
    *len = OW_HEADER + n + tag;
    return OW_OK;
}

void ow_reader_init(ow_reader *r) {
    r->have = 0;
    r->want = OW_HEADER;
    r->err = OW_OK;
}

/* Checks a complete header; on success sets r->want to the whole frame's size. */
static int check_header(ow_link *l, ow_reader *r) {
    const uint8_t *h = r->buf;
    uint32_t len = (uint32_t)h[0] | (uint32_t)h[1] << 8 | (uint32_t)h[2] << 16 | (uint32_t)h[3] << 24;
    uint16_t type = (uint16_t)(h[4] | h[5] << 8);
    const ow_msg_info *m = ow_msg(type);
    if (!m) return OW_E_TYPE;
    int sender = l->role == OW_ROLE_CLIENT ? OW_ROLE_SERVER : OW_ROLE_CLIENT;
    int e = frame_allowed(m, sender, l);
    if (e) return e;
    uint32_t tag = l->sealed ? OW_TAG : 0;
    if (len > ow_msg_limit(m) + tag) return OW_E_TOO_BIG;
    if (len < ow_msg_min(m) + tag) return OW_E_TOO_SMALL;
    r->want = OW_HEADER + len;
    return OW_OK;
}

int ow_reader_take(ow_link *l, ow_reader *r, const uint8_t **data, size_t *n, ow_frame *f) {
    if (r->err) return r->err;
    if (r->have == r->want && r->want > OW_HEADER) {
        /* The previous call returned this frame: start the next one. */
        r->have = 0;
        r->want = OW_HEADER;
    }
    for (;;) {
        size_t room = r->want - r->have;
        size_t k = *n < room ? *n : room;
        if (k) {
            memcpy(r->buf + r->have, *data, k);
            r->have += (uint32_t)k;
            *data += k;
            *n -= k;
        }
        if (r->have < r->want) return 0;
        if (r->want == OW_HEADER) {
            int e = check_header(l, r);
            if (e) return r->err = e;
            if (r->want > OW_HEADER) continue;
        }
        break;
    }
    uint32_t len = r->want - OW_HEADER;
    uint8_t *body = r->buf + OW_HEADER;
    if (l->sealed) {
        uint8_t nonce[24];
        if (l->rx.counter == UINT64_MAX) return r->err = OW_E_COUNTER;
        nonce_of(&l->rx, nonce);
        len -= OW_TAG;
        if (crypto_aead_unlock(body, body + len, l->rx.key, nonce, r->buf, OW_HEADER, body, len))
            return r->err = OW_E_SEAL;
        l->rx.counter++;
    }
    f->type = (uint16_t)(r->buf[4] | r->buf[5] << 8);
    f->len = len;
    f->payload = body;
    /* A frame with an empty payload has want == OW_HEADER; mark it as returned so the next call starts afresh. */
    if (r->want == OW_HEADER) r->have = 0;
    return 1;
}
