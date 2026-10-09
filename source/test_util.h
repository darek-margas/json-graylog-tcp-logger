/*
* test_util.h
*
* Shared helpers for unit and integration tests: a CHECK macro that keeps
* going after a failure, and loopback TCP listeners.
*/

#ifndef TEST_UTIL_H
#define TEST_UTIL_H

#include <stdio.h>

extern int test_failures;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        test_failures++; \
    } \
} while (0)

double tu_now(void);
void tu_sleep_ms(int ms);

/* Listening socket on 127.0.0.1 (port 0 = any free one), close-on-exec. */
int tu_listen(int port);
int tu_port(int fd);
/* A loopback port nothing listens on (bound once, then released). */
int tu_free_port(void);
/* Accepts a connection, waiting up to timeout_ms. Returns fd or -1. */
int tu_accept(int lfd, int timeout_ms);

#endif
