/*
 * event_loop.c - kqueue / epoll reactor (see event_loop.h).
 *
 * Both backends share the same bookkeeping: a table indexed by fd holding
 * the callback, its argument, the current interest set and a generation
 * counter. The generation guards against a classic reactor bug: a callback
 * closes fd 7 and a new connection is accepted on fd 7 in the same batch;
 * a second stale event for the old fd 7 must not reach the new connection.
 */
#include "core/event_loop.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "core/log.h"
#include "core/util.h"

#if defined(__linux__)
#define USE_EPOLL 1
#include <sys/epoll.h>
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#define USE_KQUEUE 1
#include <sys/event.h>
#else
#error "No supported event backend (need epoll or kqueue)"
#endif

#define MAX_EVENTS 256

typedef struct slot {
    ev_io_cb cb;
    void *arg;
    int events;   /* current interest set */
    uint32_t gen; /* bumped on every ev_del */
} slot_t;

struct event_loop {
    int pfd; /* kqueue() or epoll_create1() descriptor */
    slot_t *slots;
    size_t nslots;
    timer_heap_t timers;
    uint64_t now;
    atomic_bool stop;
    int wake_r, wake_w; /* self-pipe used by ev_stop() */
};

/* Make sure slots[] can hold `fd`. fds are small dense integers so a flat
 * array is both the fastest and the simplest lookup. */
static slot_t *slot_for(event_loop_t *loop, int fd)
{
    if ((size_t)fd >= loop->nslots) {
        size_t n = loop->nslots ? loop->nslots : 1024;
        while (n <= (size_t)fd)
            n *= 2;
        loop->slots = xrealloc(loop->slots, n * sizeof(slot_t));
        memset(loop->slots + loop->nslots, 0, (n - loop->nslots) * sizeof(slot_t));
        loop->nslots = n;
    }
    return &loop->slots[fd];
}

/* ------------------------------------------------------------------------
 * Backend-specific interest set manipulation
 * ---------------------------------------------------------------------- */
#ifdef USE_KQUEUE
static int backend_update(event_loop_t *loop, int fd, int old_events, int new_events)
{
    struct kevent changes[2];
    int n = 0;
    /* kqueue tracks read and write as two separate filters; translate the
     * diff of the interest sets into ADD/DELETE operations. */
    if ((new_events & EV_READ) && !(old_events & EV_READ))
        EV_SET(&changes[n++], fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);
    else if (!(new_events & EV_READ) && (old_events & EV_READ))
        EV_SET(&changes[n++], fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    if ((new_events & EV_WRITE) && !(old_events & EV_WRITE))
        EV_SET(&changes[n++], fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, NULL);
    else if (!(new_events & EV_WRITE) && (old_events & EV_WRITE))
        EV_SET(&changes[n++], fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    if (n == 0)
        return 0;
    if (kevent(loop->pfd, changes, n, NULL, 0, NULL) < 0) {
        /* ENOENT on delete just means the fd was already closed. */
        if (errno != ENOENT)
            return -1;
    }
    return 0;
}
#else
static uint32_t to_epoll(int events)
{
    uint32_t e = EPOLLRDHUP;
    if (events & EV_READ)
        e |= EPOLLIN;
    if (events & EV_WRITE)
        e |= EPOLLOUT;
    return e;
}

static int backend_update(event_loop_t *loop, int fd, int old_events, int new_events)
{
    struct epoll_event ee = {.events = to_epoll(new_events), .data.fd = fd};
    int op;
    if (old_events == 0 && new_events != 0)
        op = EPOLL_CTL_ADD;
    else if (old_events != 0 && new_events == 0)
        op = EPOLL_CTL_DEL;
    else if (old_events != new_events)
        op = EPOLL_CTL_MOD;
    else
        return 0;
    if (epoll_ctl(loop->pfd, op, fd, &ee) < 0) {
        if (op == EPOLL_CTL_DEL && (errno == ENOENT || errno == EBADF))
            return 0;
        return -1;
    }
    return 0;
}
#endif

/* ------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */
static void on_wakeup(event_loop_t *loop, int fd, int events, void *arg)
{
    char junk[64];
    while (read(fd, junk, sizeof(junk)) > 0)
        ;
}

event_loop_t *ev_new(void)
{
    event_loop_t *loop = xcalloc(1, sizeof(*loop));
#ifdef USE_KQUEUE
    loop->pfd = kqueue();
#else
    loop->pfd = epoll_create1(EPOLL_CLOEXEC);
#endif
    if (loop->pfd < 0) {
        LOG_ERROR("event backend init failed: %s", strerror(errno));
        free(loop);
        return NULL;
    }
    fd_set_cloexec(loop->pfd);
    timer_heap_init(&loop->timers);
    loop->now = now_ms();
    atomic_init(&loop->stop, false);

    int p[2];
    if (pipe(p) < 0) {
        LOG_ERROR("pipe: %s", strerror(errno));
        close(loop->pfd);
        free(loop);
        return NULL;
    }
    loop->wake_r = p[0];
    loop->wake_w = p[1];
    fd_set_nonblocking(p[0]);
    fd_set_nonblocking(p[1]);
    fd_set_cloexec(p[0]);
    fd_set_cloexec(p[1]);
    ev_add(loop, loop->wake_r, EV_READ, on_wakeup, NULL);
    return loop;
}

void ev_free(event_loop_t *loop)
{
    if (!loop)
        return;
    close(loop->wake_r);
    close(loop->wake_w);
    close(loop->pfd);
    timer_heap_free(&loop->timers);
    free(loop->slots);
    free(loop);
}

int ev_add(event_loop_t *loop, int fd, int events, ev_io_cb cb, void *arg)
{
    slot_t *s = slot_for(loop, fd);
    if (backend_update(loop, fd, 0, events) < 0)
        return -1;
    s->cb = cb;
    s->arg = arg;
    s->events = events;
    return 0;
}

int ev_mod(event_loop_t *loop, int fd, int events)
{
    slot_t *s = slot_for(loop, fd);
    if (!s->cb) {
        errno = ENOENT;
        return -1;
    }
    if (s->events == events)
        return 0;
    if (backend_update(loop, fd, s->events, events) < 0)
        return -1;
    s->events = events;
    return 0;
}

int ev_del(event_loop_t *loop, int fd)
{
    if ((size_t)fd >= loop->nslots)
        return 0;
    slot_t *s = &loop->slots[fd];
    if (!s->cb)
        return 0;
    backend_update(loop, fd, s->events, 0);
    s->cb = NULL;
    s->arg = NULL;
    s->events = 0;
    s->gen++;
    return 0;
}

timer_heap_t *ev_timers(event_loop_t *loop)
{
    return &loop->timers;
}

uint64_t ev_now(event_loop_t *loop)
{
    return loop->now;
}

void ev_timer_after(event_loop_t *loop, timer_t_ *t, uint64_t delay_ms)
{
    timer_schedule(&loop->timers, t, loop->now + delay_ms);
}

void ev_stop(event_loop_t *loop)
{
    atomic_store(&loop->stop, true);
    char c = 1;
    ssize_t rc = write(loop->wake_w, &c, 1);
    (void)rc;
}

int ev_run_once(event_loop_t *loop, int timeout_ms)
{
    /* Clamp the wait to the earliest timer so timers fire on time. */
    int64_t next = timer_heap_next_ms(&loop->timers, loop->now);
    if (next >= 0 && (timeout_ms < 0 || next < timeout_ms))
        timeout_ms = (int)next;

#ifdef USE_KQUEUE
    struct kevent evs[MAX_EVENTS];
    struct timespec ts, *tsp = NULL;
    if (timeout_ms >= 0) {
        ts.tv_sec = timeout_ms / 1000;
        ts.tv_nsec = (timeout_ms % 1000) * 1000000L;
        tsp = &ts;
    }
    int n = kevent(loop->pfd, NULL, 0, evs, MAX_EVENTS, tsp);
#else
    struct epoll_event evs[MAX_EVENTS];
    int n = epoll_wait(loop->pfd, evs, MAX_EVENTS, timeout_ms);
#endif
    if (n < 0 && errno != EINTR) {
        LOG_ERROR("poll failed: %s", strerror(errno));
        return -1;
    }
    loop->now = now_ms();

    /* Snapshot generations before dispatching anything (see file header). */
    uint32_t gens[MAX_EVENTS];
    for (int i = 0; i < n; i++) {
#ifdef USE_KQUEUE
        int fd = (int)evs[i].ident;
#else
        int fd = evs[i].data.fd;
#endif
        gens[i] = (size_t)fd < loop->nslots ? loop->slots[fd].gen : 0;
    }

    for (int i = 0; i < n; i++) {
        int fd, events = 0;
#ifdef USE_KQUEUE
        fd = (int)evs[i].ident;
        if (evs[i].filter == EVFILT_READ)
            events |= EV_READ;
        else if (evs[i].filter == EVFILT_WRITE)
            events |= EV_WRITE;
        if (evs[i].flags & EV_EOF)
            events |= EV_HUP;
        if (evs[i].flags & EV_ERROR)
            events |= EV_ERROR;
#else
        fd = evs[i].data.fd;
        uint32_t e = evs[i].events;
        if (e & EPOLLIN)
            events |= EV_READ;
        if (e & EPOLLOUT)
            events |= EV_WRITE;
        if (e & (EPOLLHUP | EPOLLRDHUP))
            events |= EV_HUP;
        if (e & EPOLLERR)
            events |= EV_ERROR;
#endif
        if ((size_t)fd >= loop->nslots)
            continue;
        slot_t *s = &loop->slots[fd];
        if (!s->cb || s->gen != gens[i])
            continue; /* closed (and maybe reused) during this batch */
        s->cb(loop, fd, events, s->arg);
    }

    timer_heap_run(&loop->timers, loop->now);
    return n;
}

void ev_run(event_loop_t *loop)
{
    while (!atomic_load(&loop->stop)) {
        if (ev_run_once(loop, -1) < 0)
            break;
    }
}
