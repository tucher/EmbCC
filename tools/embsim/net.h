/* net.h -- the one thing EmbSim needs from an operating system: a TCP
 * connection for the GDB server. net-posix.c implements it with POSIX
 * sockets; a host without them (the portable-host goal) supplies its own
 * file with these functions, or none, and builds without --gdb (net-none.c). */
#ifndef EMBSIM_NET_H
#define EMBSIM_NET_H

#include <stdio.h>

/* Listen on TCP `port` of `host` (0: this machine's loopback addresses).
 * Returns a listener, or -1 (and says why on stderr). */
int net_listen(const char *host, int port);
/* Wait up to `ms` milliseconds (-1: for ever) for a connection: its
 * handle, or -1 when none came. */
int net_accept(int listener, int ms);
/* Stop listening: a listener is not a connection's handle, so net_close
 * does not take one. */
void net_unlisten(int listener);
/* 1 when `fd` has bytes to read (or has been closed) within `ms`
 * milliseconds (0: now, -1: for ever), else 0. */
int net_ready(int fd, int ms);
/* Up to n bytes: how many, 0 when the other end has closed, -1 on an
 * error. */
int net_read(int fd, void *buf, int n);
/* All n bytes: 0, or -1 when the connection is gone. */
int net_write(int fd, const void *buf, int n);
void net_close(int fd);
/* 1 when the host file `f` (the UART's --input) has a byte to read now,
 * without waiting; a host that cannot tell says 1, and the read waits */
int net_file_ready(FILE *f);

#endif
