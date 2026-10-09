/*
* test_util.c
*/

#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "test_util.h"

int test_failures = 0;

double tu_now(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

void tu_sleep_ms(int ms)
{
    struct timespec ts;

    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

int tu_listen(int port)
{
    struct sockaddr_in a;
    int fd, one = 1;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    /* Must not leak into the forked GELFsender, or closing it here would not
     * take the "server" down */
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(fd, 8) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int tu_port(int fd)
{
    struct sockaddr_in a;
    socklen_t len = sizeof(a);

    if (getsockname(fd, (struct sockaddr *)&a, &len) < 0) {
        return -1;
    }
    return ntohs(a.sin_port);
}

int tu_free_port(void)
{
    int fd = tu_listen(0);
    int port = tu_port(fd);

    close(fd);
    return port;
}

int tu_accept(int lfd, int timeout_ms)
{
    struct pollfd p;
    int fd;

    p.fd = lfd;
    p.events = POLLIN;
    if (poll(&p, 1, timeout_ms) != 1) {
        return -1;
    }
    fd = accept(lfd, NULL, NULL);
    if (fd >= 0) {
        fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
    return fd;
}
