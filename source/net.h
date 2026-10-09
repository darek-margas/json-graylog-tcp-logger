/*
* net.h
*
* Copyright (c) 2024, Darek Margas  All rights reserved.
* Copyrights licensed under the GNU LESSER GENERAL PUBLIC LICENSE Version 2.1, February 1999.
* See the accompanying LICENSE file for terms.
* https://github.com/darek-margas/json-graylog-tcp-logger
*
* Non-blocking TCP connect helpers, so an unreachable server can never stall
* reading from stdin (and with it, httpd).
*/

#ifndef NET_H
#define NET_H

#include <netinet/in.h>

/* Fills addr from a dotted IPv4 address and port. Returns 0 or -1. */
int net_parse_addr(const char *ip, int port, struct sockaddr_in *addr);

/* Starts a non-blocking connect. Returns 0 if already connected, 1 if in
 * progress (wait for POLLOUT, then call net_connect_result), or -1 on
 * failure with errno set. *fd_out is set unless -1 is returned. */
int net_connect_start(const struct sockaddr_in *addr, int *fd_out);

/* Outcome of an in-progress connect: 0 if connected, else an errno value. */
int net_connect_result(int fd);

#endif
