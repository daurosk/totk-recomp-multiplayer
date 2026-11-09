/* The moving puppet of tools/net_bot.c ("walk"), as a function of the server clock, so the offline client check
 * (tests/net_client_main.c, "puppetcheck") knows where the bot really was at any moment and can measure the error of
 * the smoothed puppet against it.
 *
 * The tour repeats every 16 seconds around (cx, cy, cz): 4 s walking a circle of 6 m at 3 m/s, 4 s running a
 * circle of 10 m at 7 m/s, 4 s gliding (12 m/s level with 2 m/s of sink) with a sharp turn halfway, 4 s climbing
 * straight up a wall at 1.5 m/s then dropping back to the start of the tour in its last second (a fall). Each piece
 * starts where the previous one ended, so the path is continuous; the velocity is the path's derivative.
 */
#ifndef NET_BOT_PATH_H
#define NET_BOT_PATH_H

#include <math.h>

typedef struct {
    double p[3], v[3];
    double yaw;      /* facing, radians about +y, 0 facing +z */
    int piece;       /* 0 walk, 1 run, 2 glide, 3 climb, 4 fall */
} bot_path_pt;

static void bot_path_at(double t, double cx, double cy, double cz, bot_path_pt *o) {
    const double TAU = 6.283185307179586;
    double T = fmod(t, 16.0);
    if (T < 0) T += 16.0;
    double x = 0, y = 0, z = 0, vx = 0, vy = 0, vz = 0;
    /* piece 0: walk a circle of radius 6 from angle 0, 3 m/s */
    double w0 = 3.0 / 6.0, a0 = w0 * (T < 4 ? T : 4);
    double e0x = 6 * cos(a0) - 6, e0z = 6 * sin(a0);
    if (T < 4) {
        x = e0x, z = e0z, vx = -6 * w0 * sin(a0), vz = 6 * w0 * cos(a0), o->piece = 0;
    } else {
        double w1 = 7.0 / 10.0, s = T < 8 ? T - 4 : 4, a1 = w1 * s;
        double e1x = e0x + 10 * cos(a1) - 10, e1z = e0z + 10 * sin(a1);
        if (T < 8) {
            x = e1x, z = e1z, vx = -10 * w1 * sin(a1), vz = 10 * w1 * cos(a1), o->piece = 1;
        } else {
            /* glide: 2 s along +x, then 2 s along +z, at 12 m/s, sinking 2 m/s */
            double g = T < 12 ? T - 8 : 4;
            double gx = g < 2 ? 12 * g : 24, gz = g < 2 ? 0 : 12 * (g - 2);
            double ex = e1x + gx, ez = e1z + gz, ey = -2 * g;
            if (T < 12) {
                x = ex, z = ez, y = ey, vy = -2;
                vx = g < 2 ? 12 : 0, vz = g < 2 ? 0 : 12, o->piece = 2;
            } else if (T < 15) {
                double c = T - 12;
                x = ex, z = ez, y = ey + 1.5 * c, vy = 1.5, o->piece = 3;
            } else {
                /* fall: straight back to the start of the tour in one second, as a teleport would not */
                double c = T - 15;
                double sx = e1x + 24, sz = e1z + 24, sy = -8 + 4.5;
                x = sx + (0 - sx) * c, y = sy + (0 - sy) * c, z = sz + (0 - sz) * c;
                vx = -sx, vy = -sy, vz = -sz, o->piece = 4;
            }
        }
    }
    o->p[0] = cx + x, o->p[1] = cy + y, o->p[2] = cz + z;
    o->v[0] = vx, o->v[1] = vy, o->v[2] = vz;
    double h = sqrt(vx * vx + vz * vz);
    o->yaw = h > 1e-6 ? atan2(vx, vz) : 0;
    (void)TAU;
}

#endif
