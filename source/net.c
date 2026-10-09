/*
* net.c
*
* Copyright (c) 2024, Darek Margas  All rights reserved.
* Copyrights licensed under the GNU LESSER GENERAL PUBLIC LICENSE Version 2.1, February 1999.
* See the accompanying LICENSE file for terms.
* https://github.com/darek-margas/json-graylog-tcp-logger
*/

#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "net.h"

int net_parse_addr(const char *ip, int port, struct sockaddr_in *addr)
{
    memset(addr, 0, sizeof(*addr));
    if (ip == NULL || port < 1 || port > 65535) {
        return -1;
    }
    addr->sin_family = AF_INET;
    addr->sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, ip, &addr->sin_addr) != 1) {
        return -1;
    }
    return 0;
}

int net_connect_start(const struct sockaddr_in *addr, int *fd_out)
{
    int fd, flags, err;
    int one = 1;

    *fd_out = -1;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        err = errno;
        close(fd);
        errno = err;
        return -1;
    }
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    /* Notice dead peers on an idle connection eventually */
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));

    if (connect(fd, (const struct sockaddr *)addr, sizeof(*addr)) == 0) {
        *fd_out = fd;
        return 0;
    }
    if (errno == EINPROGRESS || errno == EINTR) {
        *fd_out = fd;
        return 1;
    }
    err = errno;
    close(fd);
    errno = err;
    return -1;
}

int net_connect_result(int fd)
{
    int err = 0;
    socklen_t len = sizeof(err);

    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0) {
        return errno;
    }
    return err;
}
