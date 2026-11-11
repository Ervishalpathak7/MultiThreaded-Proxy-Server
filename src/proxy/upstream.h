/*
 * upstream.h - asynchronous HTTP/1.1 client for talking to backends.
 *
 * One upstream_req_t performs one request/response exchange with one
 * server, entirely on the worker's event loop:
 *
 *   pool_take() or connect() -> write request -> read response -> callback
 *
 * The response is buffered completely (up to max_body_size) before the
 * callback fires. That keeps the cache and the HTTP/2 / HTTP/3 frontends
 * simple (they can serve the body from memory), at the cost of not
 * streaming very large responses; see docs/DESIGN.md for the trade-off.
 *
 * A request that fails on a *pooled* connection before any response byte
 * arrived is transparently retried once on a fresh connection: the server
 * may simply have closed an idle keep-alive socket a moment earlier.
 */
#ifndef APROXY_PROXY_UPSTREAM_H
#define APROXY_PROXY_UPSTREAM_H

#include <stdbool.h>

#include "http/request.h"
#include "lb/lb.h"
#include "net/worker.h"

typedef struct upstream_req upstream_req_t;

/* status: 0 on success (u->resp is filled in), otherwise the HTTP status
 * that best describes the failure: 502 (connect/protocol error), 504
 * (timeout). After the callback the caller must upstream_free(u). */
typedef void (*upstream_cb)(upstream_req_t *u, int status, const char *reason, void *arg);

struct upstream_req {
    worker_t *w;
    lb_group_t *group;
    lb_server_t *server;
    http_response_t resp;
    bool head_request;

    /* private */
    int fd;
    int state;
    bool from_pool;
    bool retried;
    buf_t out;
    buf_t in;
    struct h1_parser *parser;
    timer_t_ timer;
    upstream_cb cb;
    void *arg;
};

upstream_req_t *upstream_send(worker_t *w, lb_group_t *g, lb_server_t *s,
                              const http_request_t *req, upstream_cb cb, void *arg);
/* Abort a pending request; the callback is not invoked. */
void upstream_cancel(upstream_req_t *u);
/* Release a request after its callback ran. */
void upstream_free(upstream_req_t *u);

#endif /* APROXY_PROXY_UPSTREAM_H */
