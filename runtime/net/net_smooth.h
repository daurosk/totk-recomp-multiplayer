// Smoothing of another player's body from the states the server relays (player_state, docs/online-wire.md).
//
// Each state carries the sender's own server-clock time. The puppet is shown at a render time a little behind the
// server clock: render = now - delay, with the delay learned from how late the states arrive (their mean lateness,
// plus 2.5 times its mean deviation, plus the gap between states, between NS_DELAY_MIN_US and NS_DELAY_MAX_US). At
// the render time the position follows a cubic Hermite curve through the two states around it, with the states' own
// velocities as tangents; past the newest state it carries on at the last velocity, damped, for at most
// NS_EXTRAP_US and NS_EXTRAP_M. When a new state changes the path at the render time, the step goes into a
// correction offset that fades with a NS_CORRECT_TAU_US time constant. A move of over NS_WARP_M between two
// states (a teleport, a respawn) snaps instead of sliding. Rotation: the two quaternions slerped.
//
// Pure C with no state outside the struct, so the offline checks (tests/net_client_check.py) drive it directly.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NS_RING 32
#define NS_DELAY_MIN_US 60000ull
#define NS_DELAY_MAX_US 300000ull
#define NS_DELAY_START_US 100000ull
#define NS_EXTRAP_US 150000ull
#define NS_EXTRAP_M 2.0f
#define NS_CORRECT_TAU_US 120000.0f
#define NS_SNAP_M 6.0f                  // a correction this large snaps
#define NS_WARP_M 30.0f
#define NS_STILL_US 400000ull            // states further apart than this: the body was still between them

typedef struct {
    uint64_t t;                         // sender's server-clock time, microseconds
    float p[3], v[3], q[4];             // position, velocity (m/s), rotation quaternion x, y, z, w
    int tag;                            // the caller's index for the rest of the state (animation and the like)
} NsSample;

typedef struct {
    NsSample s[NS_RING];
    int n, head;                        // head: index of the oldest
    uint64_t newest_t;
    double late_mean, late_dev, gap_mean;   // microseconds
    int counted;
    double delay;                       // microseconds, moves toward the target slowly
    uint64_t last_eval;                 // render time of the last output, 0 before it
    uint64_t last_now;                  // the clock at the last output
    float last_raw[3], off[3];
    int changed;                        // a state came in since the last output
} NetSmooth;

typedef struct {
    float p[3], v[3], q[4];
    int tag;                            // tag of the state in effect at the render time
    uint64_t render_t;
    int extrapolating;
} NsOut;

void ns_init(NetSmooth *s);
// A state, with the time it arrived on the receiver's estimate of the server clock. States older than the newest are
// dropped (the relay keeps order, so these only come from a clock step). Returns 1 when kept.
int ns_push(NetSmooth *s, const NsSample *in, uint64_t arrived);
// The body at now (the receiver's estimate of the server clock). Returns 0 while no state arrived.
int ns_eval(NetSmooth *s, uint64_t now, NsOut *out);
// The current delay in microseconds.
uint64_t ns_delay(const NetSmooth *s);

#ifdef __cplusplus
}
#endif
