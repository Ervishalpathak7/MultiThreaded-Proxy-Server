/*
 * timer_heap.h - binary min-heap of timers keyed by deadline.
 *
 * Timers are *intrusive*: the caller embeds a `timer_t_` in its own struct
 * (a connection, an upstream request, a health checker) and the heap only
 * stores pointers. That gives O(log n) add/cancel with zero allocations on
 * the hot path, which matters because every request arms and disarms a few
 * timeouts (header timeout, idle timeout, upstream timeout).
 *
 * The heap is single-threaded: each worker's event loop owns one.
 */
#ifndef APROXY_CORE_TIMER_HEAP_H
#define APROXY_CORE_TIMER_HEAP_H

#include <stddef.h>
#include <stdint.h>

#define TIMER_NOT_SCHEDULED ((size_t)-1)

typedef struct timer_t_ timer_t_;
typedef void (*timer_cb)(timer_t_ *t);

struct timer_t_ {
    uint64_t deadline; /* absolute time in ms (now_ms() clock) */
    timer_cb cb;       /* called when the deadline passes */
    void *arg;         /* user data, usually the enclosing object */
    size_t idx;        /* position in the heap, TIMER_NOT_SCHEDULED if idle */
};

typedef struct timer_heap {
    timer_t_ **items;
    size_t len;
    size_t cap;
} timer_heap_t;

void timer_heap_init(timer_heap_t *h);
void timer_heap_free(timer_heap_t *h);

/* Prepare a timer; must be called once before use. */
void timer_init(timer_t_ *t, timer_cb cb, void *arg);

/* Schedule or reschedule `t` to fire at `deadline`. */
void timer_schedule(timer_heap_t *h, timer_t_ *t, uint64_t deadline);
/* Remove `t` if it is scheduled; harmless otherwise. */
void timer_cancel(timer_heap_t *h, timer_t_ *t);
static inline int timer_is_scheduled(const timer_t_ *t)
{
    return t->idx != TIMER_NOT_SCHEDULED;
}

/* Milliseconds until the earliest deadline, 0 if already due, -1 if the
 * heap is empty. Used to compute the poll timeout. */
int64_t timer_heap_next_ms(const timer_heap_t *h, uint64_t now);

/* Pop and run every timer whose deadline <= now. */
void timer_heap_run(timer_heap_t *h, uint64_t now);

#endif /* APROXY_CORE_TIMER_HEAP_H */
