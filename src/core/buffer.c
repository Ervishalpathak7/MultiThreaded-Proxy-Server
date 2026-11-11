/*
 * buffer.c - growable byte buffer (see buffer.h).
 */
#include "core/buffer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/util.h"

#define BUF_MIN_CAP 1024

void buf_init(buf_t *b)
{
    b->data = NULL;
    b->len = b->cap = b->rpos = 0;
}

void buf_free(buf_t *b)
{
    free(b->data);
    buf_init(b);
}

void buf_reset(buf_t *b)
{
    b->len = b->rpos = 0;
}

void buf_compact(buf_t *b)
{
    if (b->rpos == 0)
        return;
    size_t unread = b->len - b->rpos;
    if (unread)
        memmove(b->data, b->data + b->rpos, unread);
    b->len = unread;
    b->rpos = 0;
}

void buf_reserve(buf_t *b, size_t extra)
{
    if (b->cap - b->len >= extra)
        return;

    /* First try to reclaim the consumed prefix before growing. */
    if (b->rpos > 0 && b->cap - (b->len - b->rpos) >= extra) {
        buf_compact(b);
        return;
    }

    size_t need = b->len + extra;
    size_t cap = b->cap ? b->cap : BUF_MIN_CAP;
    while (cap < need)
        cap *= 2;
    b->data = xrealloc(b->data, cap);
    b->cap = cap;
}

void buf_append(buf_t *b, const void *src, size_t n)
{
    if (n == 0)
        return;
    buf_reserve(b, n);
    memcpy(b->data + b->len, src, n);
    b->len += n;
}

void buf_append_str(buf_t *b, const char *s)
{
    buf_append(b, s, strlen(s));
}

int buf_vprintf(buf_t *b, const char *fmt, va_list ap)
{
    va_list ap2;
    va_copy(ap2, ap);
    /* Measure first; most headers fit in the slack space anyway. */
    int n = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (n < 0)
        return n;
    buf_reserve(b, (size_t)n + 1);
    vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap);
    b->len += (size_t)n;
    return n;
}

int buf_printf(buf_t *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = buf_vprintf(b, fmt, ap);
    va_end(ap);
    return n;
}

char *buf_wptr(buf_t *b)
{
    return b->data + b->len;
}

size_t buf_wcap(const buf_t *b)
{
    return b->cap - b->len;
}

void buf_commit(buf_t *b, size_t n)
{
    b->len += n;
}

void buf_consume(buf_t *b, size_t n)
{
    b->rpos += n;
    if (b->rpos == b->len) {
        /* Everything consumed: rewind for free. */
        b->rpos = b->len = 0;
    } else if (b->rpos >= b->cap / 2) {
        /* Consumed prefix dominates the allocation: reclaim it. */
        buf_compact(b);
    }
}
