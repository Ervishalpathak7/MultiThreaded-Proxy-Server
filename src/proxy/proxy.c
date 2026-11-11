/*
 * proxy.c - reverse proxy forwarding (see proxy.h).
 *
 * Header handling follows RFC 9110 §7.6: hop-by-hop headers and anything
 * named in the Connection header are stripped in both directions, a Via
 * entry is appended, and the usual X-Forwarded-* headers tell the backend
 * who the real client is.
 */
#include "proxy/proxy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/event_loop.h"
#include "core/log.h"
#include "core/util.h"
#include "net/worker.h"
#include "proxy/upstream.h"
#include "server.h"

#define VIA_VALUE "1.1 aproxy"
/* How many different servers we try for one request before giving up. */
#define MAX_ATTEMPTS 3

typedef struct proxy_txn {
    http_stream_t *stream;
    const config_route_t *route;
    lb_group_t *group;
    http_request_t fwd; /* rewritten request sent upstream */
    upstream_req_t *u;
    int attempts;
    proxy_response_hook hook;
    void *hook_arg;
} proxy_txn_t;

/* ------------------------------------------------------------------------
 * Header rewriting
 * ---------------------------------------------------------------------- */

/* Remove headers listed as connection options ("Connection: foo, bar"). */
static void strip_connection_options(http_headers_t *h)
{
    const char *val;
    size_t vlen;
    if (!headers_get(h, "connection", &val, &vlen))
        return;
    /* Copy, since removing headers may move arena offsets around. */
    char *copy = xstrndup(val, vlen);
    char *save = NULL;
    for (char *tok = strtok_r(copy, ", \t", &save); tok; tok = strtok_r(NULL, ", \t", &save))
        headers_remove(h, tok);
    free(copy);
}

/* Append `add` to a comma separated list header, creating it if absent
 * (used for X-Forwarded-For and Via chains). */
static void append_list_header(http_headers_t *h, const char *name, const char *add)
{
    const char *cur;
    size_t curlen;
    if (!headers_get(h, name, &cur, &curlen)) {
        headers_add_str(h, name, add);
        return;
    }
    size_t n = curlen + 2 + strlen(add) + 1;
    char *combined = xmalloc(n);
    snprintf(combined, n, "%.*s, %s", (int)curlen, cur, add);
    headers_set_str(h, name, combined);
    free(combined);
}

void proxy_prepare_request(http_request_t *out, const http_request_t *in)
{
    memcpy(out->method, in->method, in->method_len);
    out->method_len = in->method_len;
    http_request_set_target(out, in->target, in->target_len);
    out->version_major = 1;
    out->version_minor = 1;

    /* Copy end-to-end headers only. */
    for (size_t i = 0; i < headers_count(&in->headers); i++) {
        size_t nl, vl;
        const char *n = headers_name(&in->headers, i, &nl);
        const char *v = headers_value(&in->headers, i, &vl);
        if (http_header_is_hop_by_hop(n, nl))
            continue;
        headers_add(&out->headers, n, nl, v, vl);
    }
    /* Headers named in Connection are hop-by-hop too; drop them based on
     * the original list. */
    const char *cv;
    size_t cvl;
    if (headers_get(&in->headers, "connection", &cv, &cvl)) {
        char *copy = xstrndup(cv, cvl);
        char *save = NULL;
        for (char *tok = strtok_r(copy, ", \t", &save); tok; tok = strtok_r(NULL, ", \t", &save))
            headers_remove(&out->headers, tok);
        free(copy);
    }

    /* HTTP/2 and HTTP/3 requests carry :authority instead of Host. */
    const char *host;
    size_t hostlen;
    if (!headers_get(&out->headers, "host", &host, &hostlen) && in->scheme)
        headers_add_str(&out->headers, "Host", "localhost");

    /* X-Forwarded-For: append our client to any existing chain. */
    append_list_header(&out->headers, "X-Forwarded-For", in->client_ip);
    headers_set_str(&out->headers, "X-Real-IP", in->client_ip);
    headers_set_str(&out->headers, "X-Forwarded-Proto", in->scheme ? in->scheme : "http");
    if (headers_get(&in->headers, "host", &host, &hostlen))
        headers_add(&out->headers, "X-Forwarded-Host", 16, host, hostlen);

    /* Via: append to an existing chain. */
    append_list_header(&out->headers, "Via", VIA_VALUE);

    /* Framing for the upstream hop: we always send a fully buffered body
     * with Content-Length, and keep the connection alive for pooling. */
    headers_remove(&out->headers, "content-length");
    size_t blen = buf_rlen(&in->body);
    bool method_has_body = http_request_method_is(in, "POST") || http_request_method_is(in, "PUT") ||
                           http_request_method_is(in, "PATCH");
    if (blen > 0 || method_has_body) {
        char cl[32];
        snprintf(cl, sizeof(cl), "%zu", blen);
        headers_add_str(&out->headers, "Content-Length", cl);
        buf_append(&out->body, buf_rptr(&in->body), blen);
    }
    headers_add_str(&out->headers, "Connection", "keep-alive");
}

void proxy_prepare_response(http_response_t *resp)
{
    strip_connection_options(&resp->headers);
    /* Rebuild the header list without hop-by-hop entries. */
    http_headers_t clean;
    headers_init(&clean);
    for (size_t i = 0; i < headers_count(&resp->headers); i++) {
        size_t nl, vl;
        const char *n = headers_name(&resp->headers, i, &nl);
        const char *v = headers_value(&resp->headers, i, &vl);
        if (http_header_is_hop_by_hop(n, nl))
            continue;
        headers_add(&clean, n, nl, v, vl);
    }
    headers_free(&resp->headers);
    resp->headers = clean;

    append_list_header(&resp->headers, "Via", VIA_VALUE);
}

/* ------------------------------------------------------------------------
 * Transaction
 * ---------------------------------------------------------------------- */
static void txn_free(proxy_txn_t *t)
{
    http_request_free(&t->fwd);
    free(t);
}

static void proxy_cancel(http_stream_t *s)
{
    proxy_txn_t *t = s->pipeline;
    if (!t)
        return;
    upstream_cancel(t->u);
    t->u = NULL;
    txn_free(t);
    s->pipeline = NULL;
}

static bool txn_attempt(proxy_txn_t *t);

static void on_upstream_done(upstream_req_t *u, int status, const char *reason, void *arg)
{
    proxy_txn_t *t = arg;
    http_stream_t *s = t->stream;

    if (status != 0) {
        upstream_free(u);
        t->u = NULL;
        /* Retry on another server for transport failures of idempotent
         * requests (nginx's proxy_next_upstream default behaviour). */
        bool idempotent = http_request_method_is(&s->req, "GET") ||
                          http_request_method_is(&s->req, "HEAD") ||
                          http_request_method_is(&s->req, "OPTIONS");
        if (idempotent && t->attempts < MAX_ATTEMPTS && txn_attempt(t))
            return;
        s->cancel = NULL;
        s->pipeline = NULL;
        txn_free(t);
        stream_respond_error(s, status, reason);
        return;
    }

    /* Move the upstream response into the stream and clean it up. */
    http_response_free(&s->resp);
    s->resp = u->resp;
    http_response_init(&u->resp); /* so upstream_free does not free ours */
    upstream_free(u);
    t->u = NULL;

    proxy_prepare_response(&s->resp);
    if (t->hook)
        t->hook(s, t->hook_arg);

    s->cancel = NULL;
    s->pipeline = NULL;
    txn_free(t);
    stream_finish(s);
}

static bool txn_attempt(proxy_txn_t *t)
{
    http_stream_t *s = t->stream;
    lb_server_t *server =
        lb_pick(t->group, s->req.client_ip, strlen(s->req.client_ip), ev_now(s->worker->loop));
    if (!server)
        return false;
    t->attempts++;
    LOG_DEBUG("forwarding %.*s %.*s to %s (attempt %d)", (int)s->req.method_len, s->req.method,
              (int)s->req.target_len, s->req.target, server->label, t->attempts);
    t->u = upstream_send(s->worker, t->group, server, &t->fwd, on_upstream_done, t);
    return true;
}

void proxy_forward(http_stream_t *s, const config_route_t *route, lb_group_t *group,
                   proxy_response_hook hook, void *hook_arg)
{
    proxy_txn_t *t = xcalloc(1, sizeof(*t));
    t->stream = s;
    t->route = route;
    t->group = group;
    t->hook = hook;
    t->hook_arg = hook_arg;
    http_request_init(&t->fwd);
    proxy_prepare_request(&t->fwd, &s->req);

    s->pipeline = t;
    s->cancel = proxy_cancel;

    if (!txn_attempt(t)) {
        s->cancel = NULL;
        s->pipeline = NULL;
        txn_free(t);
        stream_respond_error(s, 503, "no healthy upstream servers");
    }
}
