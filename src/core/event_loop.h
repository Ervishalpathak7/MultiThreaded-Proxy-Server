/*
 * event_loop.h - the reactor: readiness notification + timers.
 *
 * One event loop runs per worker thread. It wraps kqueue (macOS/BSD) or
 * epoll (Linux) behind a single API so the rest of the server never sees
 * the platform difference:
 *
 *     ev_add(loop, fd, EV_READ, on_readable, conn);
 *     ev_mod(loop, fd, EV_READ | EV_WRITE);
 *     ev_del(loop, fd);
 *
 * Callbacks are level-triggered: as long as the fd stays readable the
 * callback keeps firing, which is the simpler model for a proxy that may
 * stop reading (back-pressure) and resume later.
 *
 * The loop is *not* thread-safe except for ev_stop(), which may be called
 * from any thread (it writes to a wakeup pipe).
 */
#ifndef APROXY_CORE_EVENT_LOOP_H
#define APROXY_CORE_EVENT_LOOP_H

#include <stdint.h>

#include "core/timer_heap.h"

enum {
    EV_READ = 1 << 0,
    EV_WRITE = 1 << 1,
    EV_ERROR = 1 << 2, /* reported only, never requested */
    EV_HUP = 1 << 3,   /* reported only: peer closed */
};

typedef struct event_loop event_loop_t;
typedef void (*ev_io_cb)(event_loop_t *loop, int fd, int events, void *arg);

event_loop_t *ev_new(void);
void ev_free(event_loop_t *loop);

/* Register fd with interest `events` (EV_READ and/or EV_WRITE). */
int ev_add(event_loop_t *loop, int fd, int events, ev_io_cb cb, void *arg);
/* Change the interest set of an already registered fd. */
int ev_mod(event_loop_t *loop, int fd, int events);
/* Unregister fd. Call this *before* close(2). */
int ev_del(event_loop_t *loop, int fd);

/* Run until ev_stop() is called. */
void ev_run(event_loop_t *loop);
/* Stop the loop at the next iteration; safe to call from another thread. */
void ev_stop(event_loop_t *loop);
/* Run exactly one iteration with the given max wait (ms, -1 = forever).
 * Exposed for tests. */
int ev_run_once(event_loop_t *loop, int timeout_ms);

/* Timers live in the loop so timeout computation and dispatch share one
 * clock reading per iteration. */
timer_heap_t *ev_timers(event_loop_t *loop);
/* Cached monotonic time (ms) for the current iteration. */
uint64_t ev_now(event_loop_t *loop);
/* Schedule t to fire `delay_ms` from now; cancel with timer_cancel(). */
void ev_timer_after(event_loop_t *loop, timer_t_ *t, uint64_t delay_ms);

#endif /* APROXY_CORE_EVENT_LOOP_H */
