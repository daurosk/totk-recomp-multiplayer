// The local player's body for the puppets add-on: once per presented frame, reads the resident "Player" actor
// (frame_rate.c, player_actor) and hands its position, velocity and rotation to the online client (net_set_self in
// net/net_client.h), which sends it to the other games about 20 times a second.
//
// Position: actor+0x2b4, the field teleport.c moves. Velocity: the difference of two reads over the host clock, smoothed
// a little so a present that lands between two game updates does not read as a stop. Rotation: the 3x3 matrix in the
// 36 bytes just before the position (actor+0x290) when it reads as a rotation (unit, orthogonal rows, determinant 1);
// the layout is a guess checked every frame, so a game version that keeps something else there falls back to a facing
// taken from the direction of travel. Animation and gliding are not read yet (nanim 0), so puppets show the body's
// default animation.
#include <math.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "net/net_client.h"
#include "platform/platform.h"
#include "puppet_self.h"
#include "teleport.h"

#define POS_OFF 0x2b4
#define ROT_OFF 0x290
#define WARP_M 30.0f            // a step this long between two reads is a teleport or a load: no velocity from it
#define VEL_KEEP 0.6f           // share of the old velocity kept per read (a light low-pass)
#define FACE_MIN_SPEED 0.5f     // m/s of horizontal speed before the travel direction sets the facing

static struct {
    int have;
    float pos[3], vel[3], yaw;
    uint64_t t_ns, actor;
    int rot_mode;               // -1 not yet logged, 0 travel direction, 1 actor matrix
} g = {.rot_mode = -1};

static int is_rotation(const float m[9]) {
    for (int r = 0; r < 3; r++) {
        float n = m[r * 3] * m[r * 3] + m[r * 3 + 1] * m[r * 3 + 1] + m[r * 3 + 2] * m[r * 3 + 2];
        if (!isfinite(n) || fabsf(n - 1.0f) > 0.02f) return 0;
    }
    for (int a = 0; a < 3; a++)
        for (int b = a + 1; b < 3; b++) {
            float d = m[a * 3] * m[b * 3] + m[a * 3 + 1] * m[b * 3 + 1] + m[a * 3 + 2] * m[b * 3 + 2];
            if (fabsf(d) > 0.02f) return 0;
        }
    float det = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) + m[2] * (m[3] * m[7] - m[4] * m[6]);
    return fabsf(det - 1.0f) < 0.05f;
}

// A rotation matrix (rows) to a quaternion x, y, z, w; the standard branch on the largest diagonal term.
static void mat_to_quat(const float m[9], float q[4]) {
    float tr = m[0] + m[4] + m[8];
    if (tr > 0) {
        float s = sqrtf(tr + 1.0f) * 2;
        q[3] = 0.25f * s, q[0] = (m[7] - m[5]) / s, q[1] = (m[2] - m[6]) / s, q[2] = (m[3] - m[1]) / s;
    } else if (m[0] > m[4] && m[0] > m[8]) {
        float s = sqrtf(1.0f + m[0] - m[4] - m[8]) * 2;
        q[3] = (m[7] - m[5]) / s, q[0] = 0.25f * s, q[1] = (m[1] + m[3]) / s, q[2] = (m[2] + m[6]) / s;
    } else if (m[4] > m[8]) {
        float s = sqrtf(1.0f + m[4] - m[0] - m[8]) * 2;
        q[3] = (m[2] - m[6]) / s, q[0] = (m[1] + m[3]) / s, q[1] = 0.25f * s, q[2] = (m[5] + m[7]) / s;
    } else {
        float s = sqrtf(1.0f + m[8] - m[0] - m[4]) * 2;
        q[3] = (m[3] - m[1]) / s, q[0] = (m[2] + m[6]) / s, q[1] = (m[5] + m[7]) / s, q[2] = 0.25f * s;
    }
}

static void yaw_to_quat(float yaw, float q[4]) {
    q[0] = 0, q[1] = sinf(yaw * 0.5f), q[2] = 0, q[3] = cosf(yaw * 0.5f);
}

void puppet_self_frame(void) {
    NetSelf s;
    memset(&s, 0, sizeof s);
    s.rot[3] = 1;
    uint64_t a = frame_step_player_actor();
    float p[3];
    if (!a || teleport_busy() || !teleport_player_pos(p)) {
        g.have = 0;
        net_set_self(&s);           // in_world 0: the others drop this body after their stale time
        return;
    }
    uint64_t now = plat_now_ns();
    if (g.have && a == g.actor && now > g.t_ns) {
        float dt = (float)((now - g.t_ns) * 1e-9), d[3] = {p[0] - g.pos[0], p[1] - g.pos[1], p[2] - g.pos[2]};
        float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (len > WARP_M) {
            memset(g.vel, 0, sizeof g.vel);
        } else if (dt > 0.002f && (len > 0 || dt > 0.05f)) {   // two presents within one game update read the same position
            for (int i = 0; i < 3; i++) g.vel[i] = g.vel[i] * VEL_KEEP + d[i] / dt * (1 - VEL_KEEP);
        } else {
            goto keep_time;     // same position, too soon to call it a stop: keep the last read as the base
        }
    } else {
        memset(g.vel, 0, sizeof g.vel);
    }
    memcpy(g.pos, p, sizeof p);
    g.t_ns = now;
keep_time:
    g.actor = a;
    g.have = 1;

    s.in_world = 1;
    memcpy(s.pos, p, sizeof p);
    memcpy(s.vel, g.vel, sizeof g.vel);
    float m[9];
    int mode = plat_mem_read(a + ROT_OFF, m, sizeof m) && is_rotation(m);
    if (mode) {
        mat_to_quat(m, s.rot);
    } else {
        float hs = hypotf(g.vel[0], g.vel[2]);
        if (hs > FACE_MIN_SPEED) g.yaw = atan2f(g.vel[0], g.vel[2]);
        yaw_to_quat(g.yaw, s.rot);
    }
    if (mode != g.rot_mode) {
        g.rot_mode = mode;
        rt_log("puppets: local facing from %s", mode ? "the actor's rotation matrix" : "the direction of travel");
    }
    net_set_self(&s);
}
