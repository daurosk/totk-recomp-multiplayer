/* The operating system layer of sv_os.h: Winsock and Win32 threads on Windows, BSD sockets and pthreads elsewhere. */
#include "sv_os.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#include <io.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifdef _WIN32
int sv_net_init(void) {
    WSADATA w;
    return WSAStartup(MAKEWORD(2, 2), &w) != 0;
}
static int would_block(void) { return WSAGetLastError() == WSAEWOULDBLOCK; }
static void set_nonblocking(sv_sock s) {
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
}
void sv_close(sv_sock s) {
    if (s != SV_BAD_SOCK) closesocket(s);
}
int sv_poll(sv_pollfd *fds, size_t n, int timeout_ms) {
    if (!n) {
        Sleep((DWORD)timeout_ms);
        return 0;
    }
    return WSAPoll(fds, (ULONG)n, timeout_ms);
}
#else
int sv_net_init(void) {
    signal(SIGPIPE, SIG_IGN);
    return 0;
}
static int would_block(void) { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR; }
static void set_nonblocking(sv_sock s) { fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK); }
void sv_close(sv_sock s) {
    if (s != SV_BAD_SOCK) close(s);
}
int sv_poll(sv_pollfd *fds, size_t n, int timeout_ms) {
    int r = poll(fds, (nfds_t)n, timeout_ms);
    return r < 0 && errno == EINTR ? 0 : r;
}
#endif

static void set_nodelay(sv_sock s) {
    int on = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&on, sizeof on);
}

sv_sock sv_listen(const char *bind_addr, uint16_t port, char *err, size_t errlen) {
    struct addrinfo hints, *res = NULL;
    char portstr[8];
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE | AI_NUMERICHOST;
    snprintf(portstr, sizeof portstr, "%u", port);
    const char *host = bind_addr && *bind_addr ? bind_addr : "0.0.0.0";
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) {
        snprintf(err, errlen, "bind address %s is not a numeric IPv4 or IPv6 address", host);
        return SV_BAD_SOCK;
    }
    sv_sock s = socket(res->ai_family, SOCK_STREAM, IPPROTO_TCP);
    if (s == SV_BAD_SOCK) {
        snprintf(err, errlen, "no socket");
        freeaddrinfo(res);
        return SV_BAD_SOCK;
    }
    int on = 1, off = 0;
#ifdef _WIN32
    /* On Windows SO_REUSEADDR lets a second process take the same port; exclusive use is the safe choice. */
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&on, sizeof on);
#else
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&on, sizeof on);
#endif
    if (res->ai_family == AF_INET6) setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&off, sizeof off);
    if (bind(s, res->ai_addr, (int)res->ai_addrlen) != 0 || listen(s, 64) != 0) {
        snprintf(err, errlen, "port %u on %s is in use or not allowed", port, host);
        sv_close(s);
        freeaddrinfo(res);
        return SV_BAD_SOCK;
    }
    freeaddrinfo(res);
    set_nonblocking(s);
    return s;
}

sv_sock sv_accept(sv_sock l, char *addr, size_t addrlen) {
    struct sockaddr_storage sa;
    socklen_t len = sizeof sa;
    sv_sock s = accept(l, (struct sockaddr *)&sa, &len);
    if (s == SV_BAD_SOCK) return SV_BAD_SOCK;
    set_nonblocking(s);
    set_nodelay(s);
    char host[NI_MAXHOST];
    if (getnameinfo((struct sockaddr *)&sa, len, host, sizeof host, NULL, 0, NI_NUMERICHOST) != 0)
        snprintf(host, sizeof host, "?");
    /* An IPv4 peer on a dual-stack socket shows as ::ffff:a.b.c.d; keep the IPv4 form so limits per address and
     * the log see one address, not two spellings of it. */
    const char *h = strncmp(host, "::ffff:", 7) == 0 && strchr(host + 7, '.') ? host + 7 : host;
    snprintf(addr, addrlen, "%s", h);
    return s;
}

sv_sock sv_connect(const char *host, uint16_t port) {
    struct addrinfo hints, *res = NULL;
    char portstr[8];
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(portstr, sizeof portstr, "%u", port);
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) return SV_BAD_SOCK;
    sv_sock s = socket(res->ai_family, SOCK_STREAM, IPPROTO_TCP);
    if (s != SV_BAD_SOCK) {
        set_nonblocking(s);
        set_nodelay(s);
        if (connect(s, res->ai_addr, (int)res->ai_addrlen) != 0) {
#ifdef _WIN32
            int pending = WSAGetLastError() == WSAEWOULDBLOCK;
#else
            int pending = errno == EINPROGRESS;
#endif
            if (!pending) {
                sv_close(s);
                s = SV_BAD_SOCK;
            }
        }
    }
    freeaddrinfo(res);
    return s;
}

int sv_recv(sv_sock s, void *buf, size_t n) {
    int k = (int)recv(s, (char *)buf, (int)(n > 0x10000 ? 0x10000 : n), 0);
    if (k > 0) return k;
    if (k == 0) return 0;
    return would_block() ? -1 : -2;
}

int sv_send(sv_sock s, const void *buf, size_t n) {
#ifdef _WIN32
    int flags = 0;
#else
    int flags = MSG_NOSIGNAL;
#endif
    int k = (int)send(s, (const char *)buf, (int)(n > 0x10000 ? 0x10000 : n), flags);
    if (k >= 0) return k;
    return would_block() ? 0 : -2;
}

void sv_pollfd_set(sv_pollfd *p, sv_sock s, int want_read, int want_write) {
    p->fd = s;
    p->events = (short)((want_read ? POLLIN : 0) | (want_write ? POLLOUT : 0));
    p->revents = 0;
}

int sv_poll_readable(const sv_pollfd *p) { return (p->revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) != 0; }
int sv_poll_writable(const sv_pollfd *p) { return (p->revents & POLLOUT) != 0; }

uint64_t sv_now_us(void) {
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (uint64_t)(c.QuadPart / freq.QuadPart) * 1000000u +
           (uint64_t)(c.QuadPart % freq.QuadPart) * 1000000u / (uint64_t)freq.QuadPart;
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
#endif
}

int64_t sv_unix_time(void) { return (int64_t)time(NULL); }

void sv_sleep_ms(int ms) {
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&t, NULL);
#endif
}

/* Standard input lines: a ring of whole lines filled by one thread and drained by the poll loop. */
enum { LINES = 256, LINE_MAX_BYTES = 2048 };
static char g_lines[LINES][LINE_MAX_BYTES];
static int g_head, g_count, g_eof;
#ifdef _WIN32
static CRITICAL_SECTION g_lock;
static void lock(void) { EnterCriticalSection(&g_lock); }
static void unlock(void) { LeaveCriticalSection(&g_lock); }
#else
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static void lock(void) { pthread_mutex_lock(&g_lock); }
static void unlock(void) { pthread_mutex_unlock(&g_lock); }
#endif

static void lines_loop(void) {
    char buf[LINE_MAX_BYTES];
    while (fgets(buf, sizeof buf, stdin)) {
        size_t n = strlen(buf);
        int whole = n && buf[n - 1] == '\n';
        while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
        if (!whole && n == sizeof buf - 1) {
            /* A line longer than the buffer: drop the rest of it rather than run it as several commands. */
            int ch;
            while ((ch = fgetc(stdin)) != EOF && ch != '\n') {}
        }
        for (;;) {
            lock();
            if (g_count < LINES) {
                memcpy(g_lines[(g_head + g_count) % LINES], buf, n + 1);
                g_count++;
                unlock();
                break;
            }
            unlock();
            sv_sleep_ms(10);
        }
    }
    lock();
    g_eof = 1;
    unlock();
}

#ifdef _WIN32
static DWORD WINAPI lines_thread(LPVOID arg) {
    (void)arg;
    lines_loop();
    return 0;
}
void sv_lines_start(void) {
    InitializeCriticalSection(&g_lock);
    HANDLE t = CreateThread(NULL, 0, lines_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
}
#else
static void *lines_thread(void *arg) {
    (void)arg;
    lines_loop();
    return NULL;
}
void sv_lines_start(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, lines_thread, NULL) == 0) pthread_detach(t);
}
#endif

int sv_lines_get(char *buf, size_t n) {
    int r = 0;
    lock();
    if (g_count) {
        snprintf(buf, n, "%s", g_lines[g_head]);
        g_head = (g_head + 1) % LINES;
        g_count--;
        r = 1;
    } else if (g_eof) {
        r = -1;
    }
    unlock();
    return r;
}

int sv_replace_file(const char *tmp, const char *dst) {
#ifdef _WIN32
    return MoveFileExA(tmp, dst, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ? 0 : -1;
#else
    return rename(tmp, dst);
#endif
}

int sv_mkdir(const char *path) {
#ifdef _WIN32
    if (_mkdir(path) == 0) return 0;
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) ? 0 : -1;
#else
    if (mkdir(path, 0700) == 0) return 0;
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode) ? 0 : -1;
#endif
}

void sv_private_file(const char *path) {
#ifdef _WIN32
    (void)path; /* a file in the user's own folder is already private to the user's account */
#else
    chmod(path, 0600);
#endif
}

void sv_shutdown_send(sv_sock s) {
#ifdef _WIN32
    shutdown(s, SD_SEND);
#else
    shutdown(s, SHUT_WR);
#endif
}
