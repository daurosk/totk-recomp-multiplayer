/* Random bytes from the operating system, for handshake nonces, exchange keys and identity seeds.
 *
 * Described in docs/online-wire.md ("Handshake"). Windows uses the system's preferred generator (link bcrypt);
 * Linux uses getrandom.
 */
#include "ow_wire.h"

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>

int ow_random(void *buf, size_t n) {
    while (n) {
        ULONG k = n > 0x40000000u ? 0x40000000u : (ULONG)n;
        if (BCryptGenRandom(NULL, (PUCHAR)buf, k, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return -1;
        buf = (uint8_t *)buf + k;
        n -= k;
    }
    return 0;
}
#else
#include <errno.h>
#include <sys/random.h>

int ow_random(void *buf, size_t n) {
    while (n) {
        ssize_t k = getrandom(buf, n, 0);
        if (k < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        buf = (uint8_t *)buf + k;
        n -= (size_t)k;
    }
    return 0;
}
#endif
