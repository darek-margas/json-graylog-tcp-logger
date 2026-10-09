/*
* framer.h
*
* Copyright (c) 2024, Darek Margas  All rights reserved.
* Copyrights licensed under the GNU LESSER GENERAL PUBLIC LICENSE Version 2.1, February 1999.
* See the accompanying LICENSE file for terms.
* https://github.com/darek-margas/json-graylog-tcp-logger
*
* Splits a byte stream into newline-terminated lines. A single read() from a
* pipe may return several lines or only part of one, so lines are assembled
* here and each complete one is pushed to the ring buffer with the 0x0a
* replaced by the 0x00 delimiter Graylog TCP input expects.
*/

#ifndef FRAMER_H
#define FRAMER_H

#include <stddef.h>

#include "ringbuf.h"

typedef struct {
    char *line;
    size_t max;               /* largest message, trailing 0x00 included */
    size_t len;               /* bytes of the current line collected so far */
    int discarding;           /* current line is too long, skip to newline */
    int verbose;              /* syslog every buffered line */
    unsigned long oversized;  /* lines dropped for exceeding max */
} Framer;

/* Returns 0 on success, -1 if max < 2 or out of memory. */
int framer_init(Framer *f, size_t max);
void framer_free(Framer *f);

/* Feeds n bytes of input, pushing every completed line to rb. */
void framer_feed(Framer *f, RingBuffer *rb, const char *data, size_t n);

/* Pushes an unterminated last line, if any (call at end of input). */
void framer_flush(Framer *f, RingBuffer *rb);

#endif
