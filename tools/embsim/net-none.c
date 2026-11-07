/* net-none.c -- net.h for a host without sockets: EmbSim builds and runs
 * images, and --gdb says it is not available. Build with this file in
 * place of net-posix.c (make EMBSIM_NET=tools/embsim/net-none.c). */
#include <stdio.h>

#include "net.h"

int net_listen(const char *host, int port)
{
    (void)host;
    (void)port;
    fprintf(stderr, "embsim: this build has no network connection "
                    "(net-none.c)\n");
    return -1;
}

int net_accept(int listener, int ms)
{
    (void)listener;
    (void)ms;
    return -1;
}

int net_ready(int fd, int ms)
{
    (void)fd;
    (void)ms;
    return 0;
}

int net_read(int fd, void *buf, int n)
{
    (void)fd;
    (void)buf;
    (void)n;
    return -1;
}

int net_write(int fd, const void *buf, int n)
{
    (void)fd;
    (void)buf;
    (void)n;
    return -1;
}

void net_unlisten(int listener)
{
    (void)listener;
}

void net_close(int fd)
{
    (void)fd;
}

int net_file_ready(FILE *f)
{
    (void)f;
    return 1;
}
