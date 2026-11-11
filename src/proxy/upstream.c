/*
 * upstream.c - async upstream HTTP/1.1 client (see upstream.h).
 */
#include "proxy/upstream.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>

#include "config/config.h"
#include "core/event_loop.h"
#include "core/log.h"
#include "core/util.h"
#include "http/h1_parser.h"
#include "proxy/pool.h"
#include "server.h"

enum { U_CONNECTING, U_WRITING, U_READING, U_FAILED, U_DONE };

#define UPSTREAM_READ_CHUNK (64 * 1024)

static void u_on_io(event_loop_t *loop, int fd, int events, void *arg);
static void u_on_timeout(timer_t_ *t);
static int u_start_connection(upstream_req_t *u);

/* ------------------------------------------------------------------------
 * Completion
 * ---------------------------------------------------------------------- */
static void u_release_socket(upstream_req_t *u, bool reusable)
{
    if (u->fd < 0)
        return;
    ev_del(u->w->loop, u->fd);
    if (reusable)
        pool_put(u->w->pool, u->server, u->fd, u->group->cfg->keepalive);
    else
        close(u->fd);
    u->fd = -1;
}

static void u_finish(upstream_req_t *u, int status, const char *reason)
{
    timer_cancel(ev_timers(u->w->loop), &u->timer);
    u->state = U_DONE;
    lb_server_end(u->server);

    bool reusable = status == 0 && u->resp.keep_alive;
    u_release_socket(u, reusable);

    uint64_t now = ev_now(u->w->loop);
    if (status == 0) {
        lb_server_success(u->server);
    } else {
        lb_server_failure(u->server, now);
        atomic_fetch_add_explicit(&u->w->srv->stats.upstream_errors, 1, memory_order_relaxed);
        LOG_WARN("upstream %s: %s", u->server->label, reason);
    }
    u->cb(u, status, reason, u->arg);
}

/* Error on a pooled connection before any response data: retry on a fresh
 * socket (the server probably closed an idle keep-alive connection). */
static bool u_try_retry(upstream_req_t *u)
{
    if (!u->from_pool || u->retried || buf_rlen(&u->in) > 0)
        return false;
    LOG_DEBUG("upstream %s: stale pooled connection, retrying", u->server->label);
    u->retried = true;
    ev_del(u->w->loop, u->fd);
    close(u->fd);
    u->fd = -1;
    u->out.rpos = 0; /* resend the whole request */
    http_response_reset(&u->resp);
    h1_parser_reset(u->parser);
    return u_start_connection(u) == 0;
}

/* ------------------------------------------------------------------------
 * I/O
 * ---------------------------------------------------------------------- */
static void u_do_write(upstream_req_t *u)
{
    while (!buf_empty(&u->out)) {
        ssize_t n = send(u->fd, buf_rptr(&u->out), buf_rlen(&u->out),
#ifdef MSG_NOSIGNAL
                         MSG_NOSIGNAL
#else
                         0
#endif
        );
        if (n > 0) {
            buf_consume(&u->out, (size_t)n);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;
        if (n < 0 && errno == EINTR)
            continue;
        if (u_try_retry(u))
            return;
        u_finish(u, 502, "write to upstream failed");
        return;
    }
    /* Request fully sent; switch to reading with the read timeout. */
    u->state = U_READING;
    ev_mod(u->w->loop, u->fd, EV_READ);
    ev_timer_after(u->w->loop, &u->timer, u->group->cfg->read_timeout_ms);
}

static void u_do_read(upstream_req_t *u)
{
    buf_reserve(&u->in, UPSTREAM_READ_CHUNK);
    ssize_t n = read(u->fd, buf_wptr(&u->in), buf_wcap(&u->in));
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            return;
        if (u_try_retry(u))
            return;
        u_finish(u, 502, "read from upstream failed");
        return;
    }
    if (n == 0) {
        /* EOF: fine only if the body was delimited by close. */
        if (h1_parse_eof(u->parser) == H1_COMPLETE) {
            u->resp.keep_alive = false;
            u_finish(u, 0, NULL);
            return;
        }
        if (u_try_retry(u))
            return;
        u_finish(u, 502, "upstream closed connection prematurely");
        return;
    }
    buf_commit(&u->in, (size_t)n);

    h1_result_t r = h1_parse_response(u->parser, &u->in, &u->resp, u->head_request);
    if (r == H1_COMPLETE) {
        /* Leftover bytes would be a protocol violation; never pool then. */
        if (!buf_empty(&u->in))
            u->resp.keep_alive = false;
        u_finish(u, 0, NULL);
    } else if (r == H1_ERROR) {
        u_finish(u, 502, u->parser->error_reason);
    }
}

static void u_on_io(event_loop_t *loop, int fd, int events, void *arg)
{
    upstream_req_t *u = arg;
    if (u->state == U_CONNECTING) {
        int err = 0;
        socklen_t len = sizeof(err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
        if (err != 0 || (events & EV_ERROR)) {
            u_finish(u, 502, err ? strerror(err) : "connect failed");
            return;
        }
        u->state = U_WRITING;
    }
    if (u->state == U_WRITING) {
        if (events & EV_ERROR) {
            if (!u_try_retry(u))
                u_finish(u, 502, "upstream socket error");
            return;
        }
        u_do_write(u);
        return;
    }
    if (u->state == U_READING)
        u_do_read(u);
}

static void u_on_timeout(timer_t_ *t)
{
    upstream_req_t *u = t->arg;
    if (u->state == U_FAILED) {
        u_finish(u, 502, "cannot open connection to upstream");
        return;
    }
    u_finish(u, 504, u->state == U_CONNECTING ? "connect timeout" : "upstream read timeout");
}

/* ------------------------------------------------------------------------
 * Start
 * ---------------------------------------------------------------------- */
static int u_start_connection(upstream_req_t *u)
{
    event_loop_t *loop = u->w->loop;

    int fd = pool_take(u->w->pool, u->server);
    if (fd >= 0) {
        u->from_pool = true;
        u->fd = fd;
        u->state = U_WRITING;
        ev_add(loop, fd, EV_WRITE, u_on_io, u);
        ev_timer_after(loop, &u->timer, u->group->cfg->read_timeout_ms);
        u_do_write(u); /* usually completes immediately */
        return 0;
    }

    u->from_pool = false;
    fd = socket(u->server->addr.ss_family, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    fd_set_nonblocking(fd);
    fd_set_cloexec(fd);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    u->fd = fd;
    int rc = connect(fd, (struct sockaddr *)&u->server->addr, u->server->addrlen);
    if (rc < 0 && errno != EINPROGRESS) {
        close(fd);
        u->fd = -1;
        return -1;
    }
    u->state = U_CONNECTING;
    ev_add(loop, fd, EV_WRITE, u_on_io, u);
    ev_timer_after(loop, &u->timer, u->group->cfg->connect_timeout_ms);
    return 0;
}

upstream_req_t *upstream_send(worker_t *w, lb_group_t *g, lb_server_t *s,
                              const http_request_t *req, upstream_cb cb, void *arg)
{
    const config_t *cfg = w->srv->cfg;
    upstream_req_t *u = xcalloc(1, sizeof(*u));
    u->w = w;
    u->group = g;
    u->server = s;
    u->cb = cb;
    u->arg = arg;
    u->fd = -1;
    u->head_request = http_request_method_is(req, "HEAD");
    http_response_init(&u->resp);
    buf_init(&u->out);
    buf_init(&u->in);
    u->parser = xmalloc(sizeof(*u->parser));
    h1_parser_init(u->parser, H1_MODE_RESPONSE, cfg->max_header_size, cfg->max_body_size);
    timer_init(&u->timer, u_on_timeout, u);

    /* Serialize the (already rewritten) request once; retries resend it. */
    h1_write_request_head(&u->out, req);
    if (buf_rlen(&req->body))
        buf_append(&u->out, buf_rptr(&req->body), buf_rlen(&req->body));

    lb_server_begin(s);
    atomic_fetch_add_explicit(&w->srv->stats.upstream_requests, 1, memory_order_relaxed);

    if (u_start_connection(u) < 0) {
        /* Never invoke the callback from inside upstream_send(): the caller
         * has not stored the handle yet. Report the failure from the loop. */
        u->state = U_FAILED;
        ev_timer_after(w->loop, &u->timer, 0);
    }
    return u;
}

void upstream_cancel(upstream_req_t *u)
{
    if (!u)
        return;
    if (u->state != U_DONE) {
        timer_cancel(ev_timers(u->w->loop), &u->timer);
        lb_server_end(u->server);
        u_release_socket(u, false); /* mid-exchange sockets are not reusable */
    }
    upstream_free(u);
}

void upstream_free(upstream_req_t *u)
{
    if (!u)
        return;
    if (u->fd >= 0) {
        ev_del(u->w->loop, u->fd);
        close(u->fd);
    }
    http_response_free(&u->resp);
    buf_free(&u->out);
    buf_free(&u->in);
    free(u->parser);
    free(u);
}
