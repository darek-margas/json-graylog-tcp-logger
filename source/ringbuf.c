/*
* ringbuf.c
*
* Copyright (c) 2024, Darek Margas  All rights reserved.
* Copyrights licensed under the GNU LESSER GENERAL PUBLIC LICENSE Version 2.1, February 1999.
* See the accompanying LICENSE file for terms.
* https://github.com/darek-margas/json-graylog-tcp-logger
*/

#include <stdlib.h>
#include <string.h>

#include "ringbuf.h"

/* Header stores the length in 4 bytes, so cap capacity well below 4 GiB. */
#define RB_MAX_SIZE 0x40000000UL

/* Copies n bytes into the ring at pos, wrapping if needed; returns new pos. */
static size_t rb_put(RingBuffer *rb, size_t pos, const void *src, size_t n)
{
    const unsigned char *s = (const unsigned char *)src;
    size_t first = rb->capacity - pos;

    if (first > n) {
        first = n;
    }
    memcpy(rb->data + pos, s, first);
    memcpy(rb->data, s + first, n - first);
    return (pos + n) % rb->capacity;
}

/* Copies n bytes out of the ring at pos (dst may be NULL); returns new pos. */
static size_t rb_get(const RingBuffer *rb, size_t pos, void *dst, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    size_t first = rb->capacity - pos;

    if (first > n) {
        first = n;
    }
    if (d != NULL) {
        memcpy(d, rb->data + pos, first);
        memcpy(d + first, rb->data, n - first);
    }
    return (pos + n) % rb->capacity;
}

size_t rb_max_entry(size_t capacity)
{
    size_t limit = capacity / 4 * 3;

    return limit > RB_HDR ? limit - RB_HDR : 0;
}

int rb_init(RingBuffer *rb, size_t capacity)
{
    memset(rb, 0, sizeof(*rb));
    if (capacity < RB_MIN_SIZE || capacity > RB_MAX_SIZE) {
        return -1;
    }
    rb->data = (unsigned char *)malloc(capacity);
    if (rb->data == NULL) {
        return -1;
    }
    rb->capacity = capacity;
    return 0;
}

void rb_free(RingBuffer *rb)
{
    free(rb->data);
    memset(rb, 0, sizeof(*rb));
}

int rb_push(RingBuffer *rb, const char *msg, size_t len)
{
    unsigned char hdr[RB_HDR];
    size_t need = len + RB_HDR;
    size_t limit = rb->capacity / 4 * 3;
    size_t n;

    if (len == 0 || len > rb_max_entry(rb->capacity)) {
        return -1;
    }

    if (rb->used + need > rb->capacity) {
        /* Full: free space down to 3/4 so we do not evict on every push */
        while (rb->count > 0 && rb->used + need > limit) {
            n = rb_pop(rb, NULL, 0);
            rb->dropped_msgs++;
            rb->dropped_bytes += n;
        }
    }

    hdr[0] = (unsigned char)((len >> 24) & 0xff);
    hdr[1] = (unsigned char)((len >> 16) & 0xff);
    hdr[2] = (unsigned char)((len >> 8) & 0xff);
    hdr[3] = (unsigned char)(len & 0xff);
    rb->head = rb_put(rb, rb->head, hdr, RB_HDR);
    rb->head = rb_put(rb, rb->head, msg, len);
    rb->used += need;
    rb->count++;
    return 0;
}

size_t rb_pop(RingBuffer *rb, char *out, size_t out_size)
{
    unsigned char hdr[RB_HDR];
    size_t len, pos;

    if (rb->count == 0) {
        return 0;
    }

    pos = rb_get(rb, rb->tail, hdr, RB_HDR);
    len = ((size_t)hdr[0] << 24) | ((size_t)hdr[1] << 16) |
          ((size_t)hdr[2] << 8) | (size_t)hdr[3];
    if (out != NULL) {
        rb_get(rb, pos, out, len < out_size ? len : out_size);
    }
    rb->tail = (pos + len) % rb->capacity;
    rb->used -= len + RB_HDR;
    rb->count--;
    if (rb->count == 0) {
        rb->head = 0;
        rb->tail = 0;
    }
    return len;
}
