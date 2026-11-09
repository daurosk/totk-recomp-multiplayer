// Smoothing of another player's body; the method is described in net_smooth.h.
#include "net/net_smooth.h"

#include <math.h>
#include <string.h>

static const NsSample *at(const NetSmooth *s, int i) { return &s->s[(s->head + i) % NS_RING]; }

static float dist3(const float a[3], const float b[3]) {
    float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return sqrtf(x * x + y * y + z * z);
}

static void slerp(const float a[4], const float b_in[4], float u, float out[4]) {
    float b[4] = {b_in[0], b_in[1], b_in[2], b_in[3]};
    float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    if (d < 0) {   // the shorter way round
        for (int k = 0; k < 4; k++) b[k] = -b[k];
        d = -d;
    }
    float wa, wb;
    if (d > 0.9995f) {
        wa = 1 - u, wb = u;
    } else {
        float th = acosf(d), sn = sinf(th);
        wa = sinf((1 - u) * th) / sn, wb = sinf(u * th) / sn;
    }
    float n = 0;
    for (int k = 0; k < 4; k++) out[k] = wa * a[k] + wb * b[k], n += out[k] * out[k];
    n = n > 0 ? 1.0f / sqrtf(n) : 1.0f;
    for (int k = 0; k < 4; k++) out[k] *= n;
}

void ns_init(NetSmooth *s) {
    memset(s, 0, sizeof *s);
    s->delay = (double)NS_DELAY_START_US;
}

uint64_t ns_delay(const NetSmooth *s) { return (uint64_t)s->delay; }

int ns_push(NetSmooth *s, const NsSample *in, uint64_t arrived) {
    if (s->n && in->t <= s->newest_t) return 0;
    if (s->n) {
        double gap = (double)(in->t - s->newest_t);
        if (gap > 1e6) gap = 1e6;   // a pause in the stream is not the cadence
        s->gap_mean = s->counted ? s->gap_mean + (gap - s->gap_mean) * 0.1 : gap;
    }
    double late = (double)((int64_t)(arrived - in->t));
    if (late < 0) late = 0;
    if (!s->counted) {
        s->late_mean = late;
        s->late_dev = 0;
    } else {
        double d = late - s->late_mean;
        s->late_mean += d * 0.05;
        s->late_dev += (fabs(d) - s->late_dev) * 0.05;
    }
    s->counted++;
    if (s->n == NS_RING) {
        s->head = (s->head + 1) % NS_RING;
        s->n--;
    }
    s->s[(s->head + s->n) % NS_RING] = *in;
    s->n++;
    s->newest_t = in->t;
    s->changed = 1;
    return 1;
}

// The path at render time t without the correction. Returns the index of the state in effect.
static int path(const NetSmooth *s, uint64_t t, NsOut *o) {
    const NsSample *last = at(s, s->n - 1);
    o->extrapolating = 0;
    if (t >= last->t) {
        double e = (double)(t - last->t);
        if (e > (double)NS_EXTRAP_US) e = (double)NS_EXTRAP_US;
        // damped carry-on: x = v * tau * (1 - exp(-e / tau)), with tau the extrapolation limit
        double tau = (double)NS_EXTRAP_US, k = tau * (1.0 - exp(-e / tau)) / 1e6;
        float d[3] = {(float)(last->v[0] * k), (float)(last->v[1] * k), (float)(last->v[2] * k)};
        float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        float sc = len > NS_EXTRAP_M ? NS_EXTRAP_M / len : 1.0f;
        for (int j = 0; j < 3; j++) o->p[j] = last->p[j] + d[j] * sc, o->v[j] = last->v[j];
        memcpy(o->q, last->q, sizeof o->q);
        o->tag = last->tag;
        o->extrapolating = t > last->t;
        return s->n - 1;
    }
    int i = s->n - 2;
    while (i > 0 && at(s, i)->t > t) i--;
    const NsSample *a = at(s, i), *b = at(s, i + 1);
    if (t <= a->t) {   // before the oldest kept
        memcpy(o->p, a->p, sizeof o->p);
        memcpy(o->v, a->v, sizeof o->v);
        memcpy(o->q, a->q, sizeof o->q);
        o->tag = a->tag;
        return i;
    }
    double dt_us = (double)(b->t - a->t);
    float u = (float)((double)(t - a->t) / dt_us);
    if (dist3(a->p, b->p) > NS_WARP_M) {   // a teleport: hold, then step at the new state
        memcpy(o->p, a->p, sizeof o->p);
        memset(o->v, 0, sizeof o->v);
        memcpy(o->q, a->q, sizeof o->q);
        o->tag = a->tag;
        return i;
    }
    if (b->t - a->t > NS_STILL_US) {
        // A long gap: the sender only resumed sending when it moved again, so it was still at a until the gap's last
        // stretch. Hold a, then move over the last nominal period.
        double move = s->gap_mean > 0 && s->gap_mean < (double)NS_STILL_US ? s->gap_mean : 50000.0;
        double start = (double)(b->t - a->t) - move;
        double w = (double)(t - a->t) - start;
        u = w <= 0 ? 0 : (float)(w / move);
        for (int j = 0; j < 3; j++) o->p[j] = a->p[j] + (b->p[j] - a->p[j]) * u, o->v[j] = b->v[j];
    } else {
        float dt = (float)(dt_us / 1e6), u2 = u * u, u3 = u2 * u;
        float h00 = 2 * u3 - 3 * u2 + 1, h10 = u3 - 2 * u2 + u, h01 = -2 * u3 + 3 * u2, h11 = u3 - u2;
        for (int j = 0; j < 3; j++) {
            o->p[j] = h00 * a->p[j] + h10 * dt * a->v[j] + h01 * b->p[j] + h11 * dt * b->v[j];
            o->v[j] = a->v[j] + (b->v[j] - a->v[j]) * u;
        }
    }
    slerp(a->q, b->q, u, o->q);
    o->tag = u < 0.5f ? a->tag : b->tag;
    return i;
}

int ns_eval(NetSmooth *s, uint64_t now, NsOut *out) {
    if (!s->n) return 0;
    // the delay the stream needs, approached at a tenth of real time so the puppet never visibly speeds or stalls
    double want = s->late_mean + 2.5 * s->late_dev + (s->gap_mean > 0 ? s->gap_mean : 50000.0);
    if (want < (double)NS_DELAY_MIN_US) want = (double)NS_DELAY_MIN_US;
    if (want > (double)NS_DELAY_MAX_US) want = (double)NS_DELAY_MAX_US;
    double step = s->last_now && now > s->last_now ? (double)(now - s->last_now) * 0.1 : 0;
    s->last_now = now;
    if (!s->last_eval) s->delay = want;
    else if (s->delay < want) s->delay = s->delay + step < want ? s->delay + step : want;
    else s->delay = s->delay - step > want ? s->delay - step : want;
    uint64_t t = now > (uint64_t)s->delay ? now - (uint64_t)s->delay : 0;
    if (s->last_eval && t < s->last_eval) t = s->last_eval;   // never backwards

    NsOut raw;
    path(s, t, &raw);
    if (!s->last_eval) {
        memset(s->off, 0, sizeof s->off);
    } else {
        if (s->changed) {
            // what the new states change at the previous render time goes into the offset
            NsOut prev;
            path(s, s->last_eval, &prev);
            for (int j = 0; j < 3; j++) s->off[j] += s->last_raw[j] - prev.p[j];
        }
        float dt = (float)(t - s->last_eval);
        float keep = expf(-dt / NS_CORRECT_TAU_US);
        float len = 0;
        for (int j = 0; j < 3; j++) s->off[j] *= keep, len += s->off[j] * s->off[j];
        if (sqrtf(len) > NS_SNAP_M) memset(s->off, 0, sizeof s->off);
    }
    s->changed = 0;
    memcpy(s->last_raw, raw.p, sizeof raw.p);
    s->last_eval = t;
    *out = raw;
    for (int j = 0; j < 3; j++) out->p[j] += s->off[j];
    out->render_t = t;
    return 1;
}
