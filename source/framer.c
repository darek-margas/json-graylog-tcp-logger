/*
* framer.c
*
* Copyright (c) 2024, Darek Margas  All rights reserved.
* Copyrights licensed under the GNU LESSER GENERAL PUBLIC LICENSE Version 2.1, February 1999.
* See the accompanying LICENSE file for terms.
* https://github.com/darek-margas/json-graylog-tcp-logger
*/

#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#include "framer.h"

int framer_init(Framer *f, size_t max)
{
    memset(f, 0, sizeof(*f));
    if (max < 2) {
        return -1;
    }
    f->line = (char *)malloc(max);
    if (f->line == NULL) {
        return -1;
    }
    f->max = max;
    return 0;
}

void framer_free(Framer *f)
{
    free(f->line);
    memset(f, 0, sizeof(*f));
}

static void framer_emit(Framer *f, RingBuffer *rb)
{
    if (f->len == 0) {
        return; /* ignore empty lines */
    }
    f->line[f->len] = '\0';
    if (rb_push(rb, f->line, f->len + 1) != 0) {
        f->oversized++;
    } else if (f->verbose) {
        syslog(LOG_INFO, "Read and buffered: %s", f->line);
    }
}

void framer_feed(Framer *f, RingBuffer *rb, const char *data, size_t n)
{
    const char *nl;
    size_t chunk;

    while (n > 0) {
        nl = (const char *)memchr(data, '\n', n);
        chunk = nl != NULL ? (size_t)(nl - data) : n;

        if (!f->discarding) {
            if (f->len + chunk + 1 > f->max) {
                f->discarding = 1;
                f->oversized++;
                f->len = 0;
            } else {
                memcpy(f->line + f->len, data, chunk);
                f->len += chunk;
            }
        }

        if (nl == NULL) {
            break;
        }
        if (!f->discarding) {
            framer_emit(f, rb);
        }
        f->discarding = 0;
        f->len = 0;
        data = nl + 1;
        n -= chunk + 1;
    }
}

void framer_flush(Framer *f, RingBuffer *rb)
{
    if (!f->discarding) {
        framer_emit(f, rb);
    }
    f->discarding = 0;
    f->len = 0;
}
