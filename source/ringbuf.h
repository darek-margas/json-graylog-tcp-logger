/*
* ringbuf.h
*
* Copyright (c) 2024, Darek Margas  All rights reserved.
* Copyrights licensed under the GNU LESSER GENERAL PUBLIC LICENSE Version 2.1, February 1999.
* See the accompanying LICENSE file for terms.
* https://github.com/darek-margas/json-graylog-tcp-logger
*
* Byte-limited FIFO of variable-length messages. Each entry is stored as a
* 4 byte length header followed by the payload and may wrap around the end
* of the storage area.
*
* When a new message does not fit, the oldest messages are evicted until the
* buffer is at most 3/4 full (including the new message), so eviction happens
* in batches rather than on every incoming line once the buffer is full.
*/

#ifndef RINGBUF_H
#define RINGBUF_H

#include <stddef.h>

#define RB_HDR 4        /* bytes of length header per entry */
#define RB_MIN_SIZE 16  /* smallest usable capacity */

typedef struct {
    unsigned char *data;
    size_t capacity;
    size_t head;                 /* offset where the next entry is written */
    size_t tail;                 /* offset of the oldest entry */
    size_t used;                 /* bytes in use, headers included */
    size_t count;                /* entries stored */
    unsigned long dropped_msgs;  /* entries evicted to make room */
    unsigned long dropped_bytes; /* payload bytes of evicted entries */
} RingBuffer;

/* Largest payload a buffer of the given capacity accepts. */
size_t rb_max_entry(size_t capacity);

/* Returns 0 on success, -1 if capacity is too small/large or out of memory. */
int rb_init(RingBuffer *rb, size_t capacity);
void rb_free(RingBuffer *rb);

/* Appends a message, evicting the oldest ones if needed.
 * Returns 0 on success, -1 if len is 0 or above rb_max_entry(). */
int rb_push(RingBuffer *rb, const char *msg, size_t len);

/* Removes the oldest message, copying up to out_size bytes of it into out
 * (out may be NULL to discard). Returns its length, or 0 if empty. */
size_t rb_pop(RingBuffer *rb, char *out, size_t out_size);

#endif
