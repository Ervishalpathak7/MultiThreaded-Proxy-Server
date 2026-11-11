/*
 * conn.c - connection transport (see conn.h).
 */
#include "net/conn.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "core/event_loop.h"
#include "core/log.h"
#include "core/util.h"
#include "net/worker.h"
#include "server.h"
#include "tls/tls.h"

/* How much we try to read per readiness event. Big enough to drain a whole
 * TLS record or a typical request in one syscall, small enough to keep one
 * busy client from starving others on the same loop. */
#define READ_CHUNK (64 * 1024)

static void on_io(event_loop_t *loop, int fd, int events, void *arg);
static void on_timer(timer_t_ *t);

/* ------------------------------------------------------------------------
 * Construction / destruction
 * ---------------------------------------------------------------------- */
static void format_peer(conn_t *c, const struct sockaddr *addr)
{
    if (addr->sa_family == AF_INET) {
        const struct sockaddr_in *a = (const struct sockaddr_in *)addr;
        inet_ntop(AF_INET, &a->sin_addr, c->peer_ip, sizeof(c->peer_ip));
        c->peer_port = ntohs(a->sin_port);
    } else if (addr->sa_family == AF_INET6) {
        const struct sockaddr_in6 *a = (const struct sockaddr_in6 *)addr;
        inet_ntop(AF_INET6, &a->sin6_addr, c->peer_ip, sizeof(c->peer_ip));
        c->peer_port = ntohs(a->sin6_port);
    } else {
        strcpy(c->peer_ip, "unknown");
    }
}

conn_t *conn_new(worker_t *w, int fd, const listener_t *l, const struct sockaddr *addr,
                 socklen_t addrlen)
{
    conn_t *c = xcalloc(1, sizeof(*c));
    c->fd = fd;
    c->w = w;
    c->listener = l;
    c->refs = 1; /* the socket's own reference */
    buf_init(&c->in);
    buf_init(&c->out);
    timer_init(&c->timer, on_timer, c);
    format_peer(c, addr);

    fd_set_nonblocking(fd);
    fd_set_cloexec(fd);
    int one = 1;
    /* Responses are written in one go; Nagle would only add latency. */
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif

    worker_add_conn(w, c);
    atomic_fetch_add_explicit(&w->srv->stats.total_conns, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&w->srv->stats.active_conns, 1, memory_order_relaxed);
    return c;
}

static void conn_destroy(conn_t *c)
{
    buf_free(&c->in);
    buf_free(&c->out);
    free(c);
}

void conn_ref(conn_t *c)
{
    c->refs++;
}

void conn_unref(conn_t *c)
{
    if (--c->refs == 0)
        conn_destroy(c);
}

void conn_set_ops(conn_t *c, const conn_ops_t *ops, void *proto)
{
    c->ops = ops;
    c->proto = proto;
}

void conn_start(conn_t *c)
{
    c->read_enabled = true;
    c->interest = EV_READ;
    if (ev_add(c->w->loop, c->fd, c->interest, on_io, c) < 0) {
        LOG_ERROR("ev_add(fd=%d): %s", c->fd, strerror(errno));
        conn_close(c);
    }
}

void conn_close(conn_t *c)
{
    if (c->closed)
        return;
    c->closed = true;
    conn_clear_timeout(c);
    ev_del(c->w->loop, c->fd);
    if (c->ops && c->ops->on_close)
        c->ops->on_close(c);
    if (c->ssl)
        tls_conn_free(c);
    close(c->fd);
    c->fd = -1;
    worker_remove_conn(c->w, c);
    atomic_fetch_sub_explicit(&c->w->srv->stats.active_conns, 1, memory_order_relaxed);
    conn_unref(c); /* drop the socket's reference */
}

/* ------------------------------------------------------------------------
 * Interest management
 * ---------------------------------------------------------------------- */
void conn_update_interest(conn_t *c)
{
    if (c->closed)
        return;
    int want = 0;
    if (c->read_enabled || c->tls_handshaking)
        want |= EV_READ;
    if (!buf_empty(&c->out) || c->tls_want_write)
        want |= EV_WRITE;
    if (want != c->interest) {
        c->interest = want;
        ev_mod(c->w->loop, c->fd, want);
    }
}

void conn_want_read(conn_t *c, bool enable)
{
    c->read_enabled = enable;
    conn_update_interest(c);
}

/* ------------------------------------------------------------------------
 * Timeouts
 * ---------------------------------------------------------------------- */
void conn_set_timeout(conn_t *c, uint64_t ms)
{
    ev_timer_after(c->w->loop, &c->timer, ms);
}

void conn_clear_timeout(conn_t *c)
{
    timer_cancel(ev_timers(c->w->loop), &c->timer);
}

static void on_timer(timer_t_ *t)
{
    conn_t *c = t->arg;
    if (c->ops && c->ops->on_timeout)
        c->ops->on_timeout(c);
    else
        conn_close(c);
}

/* ------------------------------------------------------------------------
 * Raw I/O
 * ---------------------------------------------------------------------- */
ssize_t conn_read_raw(conn_t *c, void *buf, size_t len)
{
    if (c->ssl)
        return tls_conn_read(c, buf, len);
    for (;;) {
        ssize_t n = read(c->fd, buf, len);
        if (n < 0 && errno == EINTR)
            continue;
        return n;
    }
}

ssize_t conn_write_raw(conn_t *c, const void *buf, size_t len)
{
    if (c->ssl)
        return tls_conn_write(c, buf, len);
    for (;;) {
#ifdef MSG_NOSIGNAL
        ssize_t n = send(c->fd, buf, len, MSG_NOSIGNAL);
#else
        ssize_t n = write(c->fd, buf, len);
#endif
        if (n < 0 && errno == EINTR)
            continue;
        return n;
    }
}

/* ------------------------------------------------------------------------
 * Writing
 * ---------------------------------------------------------------------- */
void conn_flush(conn_t *c)
{
    if (c->closed)
        return;
    bool wrote_any = false;
    while (!buf_empty(&c->out)) {
        ssize_t n = conn_write_raw(c, buf_rptr(&c->out), buf_rlen(&c->out));
        if (n > 0) {
            buf_consume(&c->out, (size_t)n);
            wrote_any = true;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            conn_update_interest(c); /* wait for EV_WRITE */
            return;
        }
        LOG_DEBUG("write to %s failed: %s", c->peer_ip, strerror(errno));
        conn_close(c);
        return;
    }
    conn_update_interest(c);
    if (wrote_any && c->ops && c->ops->on_drained)
        c->ops->on_drained(c);
}

/* ------------------------------------------------------------------------
 * Reading
 * ---------------------------------------------------------------------- */
static void do_read(conn_t *c)
{
    buf_reserve(&c->in, READ_CHUNK);
    ssize_t n = conn_read_raw(c, buf_wptr(&c->in), buf_wcap(&c->in));
    if (n > 0) {
        buf_commit(&c->in, (size_t)n);
        atomic_fetch_add_explicit(&c->w->srv->stats.bytes_in, (uint64_t)n, memory_order_relaxed);
        if (c->ops && c->ops->on_data)
            c->ops->on_data(c);
        return;
    }
    if (n == 0) {
        LOG_DEBUG("%s closed the connection", c->peer_ip);
        conn_close(c);
        return;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK)
        return;
    LOG_DEBUG("read from %s failed: %s", c->peer_ip, strerror(errno));
    conn_close(c);
}

static void on_io(event_loop_t *loop, int fd, int events, void *arg)
{
    conn_t *c = arg;
    conn_ref(c); /* keep c alive even if a callback closes it */

    if (events & EV_ERROR) {
        conn_close(c);
        goto out;
    }
    if (c->tls_handshaking) {
        /* Any readiness just means "try the handshake again". */
        tls_conn_handshake(c);
        goto out;
    }
    if (events & EV_WRITE) {
        if (c->tls_want_write) {
            /* TLS wanted the socket writable; let the read side continue. */
            c->tls_want_write = false;
            do_read(c);
        } else {
            conn_flush(c);
        }
        if (c->closed)
            goto out;
    }
    if (events & EV_READ) {
        do_read(c);
        if (c->closed)
            goto out;
    }
    if ((events & EV_HUP) && !(events & EV_READ)) {
        /* Peer hung up and nothing left to read. */
        conn_close(c);
    }
out:
    conn_unref(c);
}
