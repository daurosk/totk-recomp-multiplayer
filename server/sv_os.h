/* What the server and the test bot need from the operating system: non-blocking TCP sockets and poll, clocks, a
 * thread that reads standard input line by line, and file replacement. One implementation for Windows (Winsock) and
 * one for Linux and other POSIX systems, in sv_os.c.
 */
#ifndef SV_OS_H
#define SV_OS_H

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sv_sock;
typedef WSAPOLLFD sv_pollfd;
#define SV_BAD_SOCK INVALID_SOCKET
#else
#include <poll.h>
#include <sys/socket.h>
typedef int sv_sock;
typedef struct pollfd sv_pollfd;
#define SV_BAD_SOCK (-1)
#endif

/* Starts the socket layer. Returns 0 on success. */
int sv_net_init(void);
/* A listening socket on bind_addr:port (bind_addr "" or "0.0.0.0" for every IPv4 address, "::" for every address of
 * both families). Writes why into err on failure. */
sv_sock sv_listen(const char *bind_addr, uint16_t port, char *err, size_t errlen);
/* The next pending connection, non-blocking and with TCP_NODELAY, or SV_BAD_SOCK when none is waiting. Its address
 * without the port goes into addr. */
sv_sock sv_accept(sv_sock l, char *addr, size_t addrlen);
/* Starts a non-blocking connect; the socket becomes writable once connected. */
sv_sock sv_connect(const char *host, uint16_t port);
/* recv: bytes read (> 0), 0 when the peer closed, -1 when nothing is waiting, -2 on an error. */
int sv_recv(sv_sock s, void *buf, size_t n);
/* send: bytes written (>= 0, 0 when the socket buffer is full), -2 on an error. */
int sv_send(sv_sock s, const void *buf, size_t n);
void sv_close(sv_sock s);
/* Ends the sending half: queued bytes still go out, then the peer reads the end of the stream. */
void sv_shutdown_send(sv_sock s);
int sv_poll(sv_pollfd *fds, size_t n, int timeout_ms);
void sv_pollfd_set(sv_pollfd *p, sv_sock s, int want_read, int want_write);
int sv_poll_readable(const sv_pollfd *p); /* readable, closed or failed: a recv will tell which */
int sv_poll_writable(const sv_pollfd *p);

/* Monotonic microseconds since an arbitrary start. */
uint64_t sv_now_us(void);
/* Seconds since 1970, for bans and mutes that outlive a restart, and for log time stamps. */
int64_t sv_unix_time(void);
void sv_sleep_ms(int ms);

/* Standard input, read by a thread so the poll loop never blocks on it. sv_lines_get copies the next whole line
 * (without its line end) and returns 1, returns 0 when none is waiting, and -1 once input has ended and every line
 * was taken. */
void sv_lines_start(void);
int sv_lines_get(char *buf, size_t n);

/* Replaces dst with tmp in one step. Returns 0 on success. */
int sv_replace_file(const char *tmp, const char *dst);
/* Creates a directory; an existing one is fine. Returns 0 on success. */
int sv_mkdir(const char *path);
/* Makes a file readable by its owner only, where the system has such permissions. */
void sv_private_file(const char *path);

#endif
