/* The handshake: identity keys, the greet / challenge / proof exchange, key derivation, and the clock sample.
 *
 * Described in docs/online-wire.md ("Handshake"). Each side makes a fresh X25519 key per connection; the server signs the transcript with its
 * Ed25519 identity, the client signs it with the player's, and both derive the two directions' keys from the X25519
 * secret and the transcript hash with BLAKE2b. All primitives are Monocypher's.
 */
#include "ow_wire.h"

#include <string.h>

#include "../third_party/monocypher/monocypher.h"
#include "../third_party/monocypher/monocypher-ed25519.h"

enum { STEP_NONE, STEP_GREETED, STEP_CHALLENGED, STEP_SEALABLE, STEP_SEALED };

static const char LABEL_TRANSCRIPT[] = "TKRW1 transcript";
static const char LABEL_SERVER[] = "TKRW1 server signs";
static const char LABEL_CLIENT[] = "TKRW1 client signs";
static const char LABEL_C2S[] = "TKRW1 client to server";
static const char LABEL_S2C[] = "TKRW1 server to client";

void ow_identity_from_seed(ow_identity *id, const uint8_t seed[32]) {
    uint8_t s[32];
    memcpy(s, seed, 32);
    crypto_ed25519_key_pair(id->secret, id->pub, s); /* wipes s */
}

void ow_identity_wipe(ow_identity *id) { crypto_wipe(id, sizeof *id); }

void ow_hs_wipe(ow_handshake *h) { crypto_wipe(h, sizeof *h); }

/* transcript = BLAKE2b-512(label, greet payload, challenge nonce, identity and exchange key) */
static void transcript_of(ow_handshake *h, const ow_challenge *ch) {
    crypto_blake2b_ctx c;
    crypto_blake2b_init(&c, 64);
    crypto_blake2b_update(&c, (const uint8_t *)LABEL_TRANSCRIPT, sizeof LABEL_TRANSCRIPT - 1);
    crypto_blake2b_update(&c, h->greet, h->greet_len);
    crypto_blake2b_update(&c, ch->nonce, 32);
    crypto_blake2b_update(&c, ch->identity, 32);
    crypto_blake2b_update(&c, ch->exchange, 32);
    crypto_blake2b_final(&c, h->transcript);
}

/* The message a side signs: its label, then the transcript. */
static size_t signed_message(uint8_t *out, const char *label, size_t label_len, const uint8_t transcript[64]) {
    memcpy(out, label, label_len);
    memcpy(out + label_len, transcript, 64);
    return label_len + 64;
}

static int derive(ow_handshake *h, const uint8_t peer_exchange[32]) {
    static const uint8_t zero[32];
    uint8_t shared[32], msg[64 + 32];
    crypto_x25519(shared, h->exchange_secret, peer_exchange);
    crypto_wipe(h->exchange_secret, 32);
    /* A low-order point gives an all-zero secret: refuse it rather than seal with a known key. */
    if (crypto_verify32(shared, zero) == 0) {
        crypto_wipe(shared, 32);
        return OW_E_KEY;
    }
    const char *labels[2] = { LABEL_C2S, LABEL_S2C };
    size_t lens[2] = { sizeof LABEL_C2S - 1, sizeof LABEL_S2C - 1 };
    for (int i = 0; i < 2; i++) {
        memcpy(msg, labels[i], lens[i]);
        memcpy(msg + lens[i], h->transcript, 64);
        crypto_blake2b_keyed(h->keys + 48 * i, 48, shared, 32, msg, lens[i] + 64);
    }
    crypto_wipe(shared, 32);
    crypto_wipe(msg, sizeof msg);
    return OW_OK;
}

static int copy_text(char *dst, size_t max, const char *src) {
    size_t n = src ? strlen(src) : 0;
    if (n > max) return OW_E_LIMIT;
    if (n) memcpy(dst, src, n);
    dst[n] = 0;
    return OW_OK;
}

int ow_hs_client_greet(ow_handshake *h, const uint8_t random[64], const char *build, ow_greet *out) {
    memset(h, 0, sizeof *h);
    h->role = OW_ROLE_CLIENT;
    memset(out, 0, sizeof *out);
    out->magic = OW_MAGIC;
    out->version = OW_VERSION;
    int e = copy_text(out->build, OW_BUILD_MAX, build);
    if (e) return e;
    memcpy(out->nonce, random, 32);
    memcpy(h->exchange_secret, random + 32, 32);
    crypto_x25519_public_key(out->exchange, h->exchange_secret);
    size_t n;
    if ((e = ow_encode(OW_T_greet, out, h->greet, sizeof h->greet, &n))) return e;
    h->greet_len = (uint32_t)n;
    h->step = STEP_GREETED;
    return OW_OK;
}

int ow_hs_client_challenge(ow_handshake *h, const ow_challenge *ch, const uint8_t *pin, const ow_identity *me,
                           const char *nickname, uint8_t character, const char *claim, uint32_t features,
                           ow_proof *out) {
    if (h->role != OW_ROLE_CLIENT || h->step != STEP_GREETED) return OW_E_STEP;
    uint8_t msg[sizeof LABEL_SERVER + 64];
    if (pin && crypto_verify32(pin, ch->identity) != 0) return OW_E_PIN;
    transcript_of(h, ch);
    size_t n = signed_message(msg, LABEL_SERVER, sizeof LABEL_SERVER - 1, h->transcript);
    if (crypto_ed25519_check(ch->signature, ch->identity, msg, n) != 0) return OW_E_AUTH;
    int e = derive(h, ch->exchange);
    if (e) return e;
    memcpy(h->peer_identity, ch->identity, 32);
    memset(out, 0, sizeof *out);
    memcpy(out->identity, me->pub, 32);
    n = signed_message(msg, LABEL_CLIENT, sizeof LABEL_CLIENT - 1, h->transcript);
    crypto_ed25519_sign(out->signature, me->secret, msg, n);
    if ((e = copy_text(out->nickname, OW_NICK_MAX, nickname))) return e;
    if ((e = copy_text(out->claim, OW_CLAIM_MAX, claim))) return e;
    out->character = character;
    out->features = features;
    h->step = STEP_SEALABLE;
    return OW_OK;
}

int ow_hs_server_greet(ow_handshake *h, const ow_identity *server, const uint8_t random[64], const ow_greet *g,
                       ow_challenge *out) {
    memset(h, 0, sizeof *h);
    h->role = OW_ROLE_SERVER;
    if (g->magic != OW_MAGIC) return OW_E_MAGIC;
    if (g->version != OW_VERSION) return OW_E_VERSION;
    size_t n;
    int e = ow_encode(OW_T_greet, g, h->greet, sizeof h->greet, &n);
    if (e) return e;
    h->greet_len = (uint32_t)n;
    memset(out, 0, sizeof *out);
    memcpy(out->nonce, random, 32);
    memcpy(h->exchange_secret, random + 32, 32);
    crypto_x25519_public_key(out->exchange, h->exchange_secret);
    memcpy(out->identity, server->pub, 32);
    transcript_of(h, out);
    uint8_t msg[sizeof LABEL_SERVER + 64];
    n = signed_message(msg, LABEL_SERVER, sizeof LABEL_SERVER - 1, h->transcript);
    crypto_ed25519_sign(out->signature, server->secret, msg, n);
    if ((e = derive(h, g->exchange))) return e;
    h->step = STEP_SEALABLE;
    return OW_OK;
}

int ow_hs_server_proof(ow_handshake *h, const ow_proof *p) {
    if (h->role != OW_ROLE_SERVER || h->step != STEP_SEALED) return OW_E_STEP;
    uint8_t msg[sizeof LABEL_CLIENT + 64];
    size_t n = signed_message(msg, LABEL_CLIENT, sizeof LABEL_CLIENT - 1, h->transcript);
    if (crypto_ed25519_check(p->signature, p->identity, msg, n) != 0) return OW_E_AUTH;
    memcpy(h->peer_identity, p->identity, 32);
    h->step = STEP_NONE; /* done: the next step is the server's own admit or refuse */
    return OW_OK;
}

int ow_hs_seal(ow_handshake *h, ow_link *l) {
    if (h->step != STEP_SEALABLE || l->role != h->role) return OW_E_STEP;
    const uint8_t *c2s = h->keys, *s2c = h->keys + 48;
    ow_seal_dir *tx = &l->tx, *rx = &l->rx;
    if (l->role == OW_ROLE_SERVER) { tx = &l->rx; rx = &l->tx; }
    memcpy(tx->key, c2s, 32);
    memcpy(tx->prefix, c2s + 32, 16);
    tx->counter = 0;
    memcpy(rx->key, s2c, 32);
    memcpy(rx->prefix, s2c + 32, 16);
    rx->counter = 0;
    l->sealed = 1;
    crypto_wipe(h->keys, sizeof h->keys);
    /* The client's part is finished; the server still checks the proof against the transcript. */
    h->step = h->role == OW_ROLE_SERVER ? STEP_SEALED : STEP_NONE;
    return OW_OK;
}

int ow_greet_peek(const uint8_t *payload, size_t len, uint32_t *magic, uint16_t *version) {
    if (len < 6) return OW_E_SHORT;
    *magic = (uint32_t)payload[0] | (uint32_t)payload[1] << 8 | (uint32_t)payload[2] << 16 |
             (uint32_t)payload[3] << 24;
    *version = (uint16_t)(payload[4] | payload[5] << 8);
    return OW_OK;
}

void ow_refuse_make(ow_refuse *r, uint16_t reason, const char *build, const char *detail) {
    memset(r, 0, sizeof *r);
    r->reason = reason;
    r->version = OW_VERSION;
    /* Over-long text is cut at a character boundary, not refused: a refusal always goes out. */
    const char *src[2] = { build ? build : "", detail ? detail : "" };
    char *dst[2] = { r->build, r->detail };
    size_t max[2] = { OW_BUILD_MAX, OW_DETAIL_MAX };
    for (int i = 0; i < 2; i++) {
        size_t n = strlen(src[i]);
        if (n > max[i]) {
            n = max[i];
            while (n && ((unsigned char)src[i][n] & 0xC0) == 0x80) n--;
        }
        memcpy(dst[i], src[i], n);
        dst[i][n] = 0;
    }
}

void ow_clock_sample(uint64_t t0, uint64_t peer_us, uint64_t t1, int64_t *offset_us, uint64_t *rtt_us) {
    uint64_t rtt = t1 >= t0 ? t1 - t0 : 0;
    *rtt_us = rtt;
    /* The peer's clock read peer_us half a round trip after t0. */
    *offset_us = (int64_t)(peer_us - (t0 + rtt / 2));
}
