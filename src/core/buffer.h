/*
 * buffer.h - growable byte buffer with a read cursor.
 *
 * Every connection owns an input buffer (bytes read from the socket that the
 * parser has not consumed yet) and an output buffer (bytes waiting to be
 * written). The layout is:
 *
 *     data
 *     |<-- rpos -->|<------ readable (len - rpos) ------>|<-- free -->|
 *     [ consumed   |  unread bytes                        |            ]
 *                                                        ^len         ^cap
 *
 * Consumed bytes are not moved on every consume (that would be O(n) per
 * call); the buffer is compacted lazily once the consumed prefix gets big.
 */
#ifndef APROXY_CORE_BUFFER_H
#define APROXY_CORE_BUFFER_H

#include <stdarg.h>
#include <stddef.h>

typedef struct buf {
    char *data;
    size_t len;  /* bytes written so far (end of valid data) */
    size_t cap;  /* allocated size */
    size_t rpos; /* read cursor: data[rpos..len) is unread */
} buf_t;

void buf_init(buf_t *b);
void buf_free(buf_t *b);

/* Drop everything, keep the allocation. */
void buf_reset(buf_t *b);

/* Make sure at least `extra` bytes can be appended without reallocating. */
void buf_reserve(buf_t *b, size_t extra);

/* --- writing ---------------------------------------------------------- */
void buf_append(buf_t *b, const void *src, size_t n);
void buf_append_str(buf_t *b, const char *s);
/* printf into the buffer; returns bytes appended. */
int buf_printf(buf_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int buf_vprintf(buf_t *b, const char *fmt, va_list ap);

/* Direct write access for read(2)-style calls: get a pointer to free space,
 * fill it, then commit the number of bytes actually written. */
char *buf_wptr(buf_t *b);
size_t buf_wcap(const buf_t *b);
void buf_commit(buf_t *b, size_t n);

/* --- reading ---------------------------------------------------------- */
static inline const char *buf_rptr(const buf_t *b) { return b->data + b->rpos; }
static inline size_t buf_rlen(const buf_t *b) { return b->len - b->rpos; }
static inline int buf_empty(const buf_t *b) { return b->len == b->rpos; }

/* Advance the read cursor by n bytes (n <= buf_rlen). */
void buf_consume(buf_t *b, size_t n);

/* Move unread bytes to the front so that rpos == 0. */
void buf_compact(buf_t *b);

#endif /* APROXY_CORE_BUFFER_H */
