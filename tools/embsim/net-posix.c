/* net-posix.c -- net.h on POSIX sockets. The only file of EmbSim's that
 * uses an operating system's API; a host without BSD sockets replaces it
 * (net.h).
 *
 * A listener is up to two sockets, IPv4 and IPv6 loopback by default, so
 * a client that resolves "localhost" to either finds the server. The
 * connection has Nagle off: the protocol is a ping-pong of small
 * packets, and every one would otherwise wait for the next. A write to a
 * connection the client has closed fails instead of raising SIGPIPE. */
#define _POSIX_C_SOURCE 200809L
#include "net.h"

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAXL 4

static int lfds[MAXL][2];               /* the listeners' sockets */
static int nl;

int net_file_ready(FILE *f)
{
    return net_ready(fileno(f), 0);
}

int net_listen(const char *host, int port)
{
    struct addrinfo hints, *res, *a;
    char ps[16];
    int n = 0;
    if (nl == MAXL)
        return -1;
    signal(SIGPIPE, SIG_IGN);
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = host ? AI_PASSIVE : 0;
    snprintf(ps, sizeof ps, "%d", port);
    if (getaddrinfo(host ? host : "localhost", ps, &hints, &res) != 0) {
        fprintf(stderr, "embsim: cannot resolve %s\n", host ? host : "localhost");
        return -1;
    }
    lfds[nl][0] = lfds[nl][1] = -1;
    for (a = res; a && n < 2; a = a->ai_next) {
        int fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        int one = 1;
        if (fd < 0)
            continue;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#ifdef IPV6_V6ONLY
        if (a->ai_family == AF_INET6)
            setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof one);
#endif
        if (bind(fd, a->ai_addr, a->ai_addrlen) < 0 || listen(fd, 1) < 0) {
            close(fd);
            continue;
        }
        lfds[nl][n++] = fd;
    }
    freeaddrinfo(res);
    if (!n) {
        fprintf(stderr, "embsim: cannot listen on port %d: %s\n", port,
                strerror(errno));
        return -1;
    }
    return nl++;
}

int net_accept(int l, int ms)
{
    fd_set set;
    struct timeval tv, *tp = 0;
    int max = -1;
    FD_ZERO(&set);
    for (int i = 0; i < 2; i++)
        if (lfds[l][i] >= 0) {
            FD_SET(lfds[l][i], &set);
            if (lfds[l][i] > max)
                max = lfds[l][i];
        }
    if (ms >= 0) {
        tv.tv_sec = ms / 1000;
        tv.tv_usec = (ms % 1000) * 1000;
        tp = &tv;
    }
    int r;
    do
        r = select(max + 1, &set, 0, 0, tp);
    while (r < 0 && errno == EINTR);
    if (r <= 0)
        return -1;
    for (int i = 0; i < 2; i++)
        if (lfds[l][i] >= 0 && FD_ISSET(lfds[l][i], &set)) {
            int fd = accept(lfds[l][i], 0, 0), one = 1;
            if (fd < 0)
                return -1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
            setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
            return fd;
        }
    return -1;
}

void net_unlisten(int l)
{
    for (int i = 0; i < 2; i++)
        if (lfds[l][i] >= 0) {
            close(lfds[l][i]);
            lfds[l][i] = -1;
        }
}

int net_ready(int fd, int ms)
{
    fd_set set;
    struct timeval tv, *tp = 0;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    if (ms >= 0) {
        tv.tv_sec = ms / 1000;
        tv.tv_usec = (ms % 1000) * 1000;
        tp = &tv;
    }
    int r;
    do
        r = select(fd + 1, &set, 0, 0, tp);
    while (r < 0 && errno == EINTR);
    return r > 0;
}

int net_read(int fd, void *buf, int n)
{
    for (;;) {
        ssize_t r = read(fd, buf, (size_t)n);
        if (r < 0 && errno == EINTR)
            continue;
        return (int)r;
    }
}

int net_write(int fd, const void *buf, int n)
{
    const char *p = buf;
    while (n > 0) {
        ssize_t w = write(fd, p, (size_t)n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += w;
        n -= (int)w;
    }
    return 0;
}

void net_close(int fd)
{
    if (fd >= 0)
        close(fd);
}
