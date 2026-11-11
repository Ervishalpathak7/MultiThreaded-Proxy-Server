/*
 * h2_server.c - HTTP/2 frontend (see h2_server.h).
 *
 * Data flow:
 *
 *   socket ──▶ c->in ──▶ nghttp2_session_mem_recv2 ──▶ callbacks build
 *                                                      http_stream_t
 *                                                      and dispatch
 *   c->out ◀── nghttp2_session_mem_send2 ◀── nghttp2_submit_response2
 *                                               (from the respond callback)
 */
#include "http/h2_server.h"

#include <ctype.h>
#include <nghttp2/nghttp2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config/config.h"
#include "core/event_loop.h"
#include "core/log.h"
#include "core/util.h"
#include "http/stream.h"
#include "net/worker.h"
#include "server.h"

#define H2_MAX_CONCURRENT_STREAMS 128
/* Stop pulling frames out of nghttp2 once this much is queued on the
 * socket; h2_on_drained resumes. Keeps a slow client from making us buffer
 * every in-flight response twice. */
#define H2_OUT_HIGH_WATER (1u << 20)

typedef struct h2_conn h2_conn_t;

typedef struct h2_stream {
    h2_conn_t *h;
    http_stream_t *s;
    int32_t id;
    bool dispatched; /* handed to the pipeline */
    bool responded;  /* response submitted to nghttp2 */
    size_t body_off; /* progress of the data provider */
    struct h2_stream *next;
} h2_stream_t;

struct h2_conn {
    conn_t *c;
    nghttp2_session *session;
    h2_stream_t *streams;
    size_t nstreams;
    bool in_callback; /* inside nghttp2_session_mem_recv2: defer h2_send */
    bool goaway_sent; /* worker is draining, no new streams accepted */
};

static void h2_send(h2_conn_t *h);
static void h2_respond(http_stream_t *s);

/* ------------------------------------------------------------------------
 * Stream bookkeeping
 * ---------------------------------------------------------------------- */
static h2_stream_t *h2_stream_new(h2_conn_t *h, int32_t id)
{
    h2_stream_t *hs = xcalloc(1, sizeof(*hs));
    hs->h = h;
    hs->id = id;
    hs->s = stream_new(h->c->w, h2_respond, hs);
    hs->s->stream_id = id;
    hs->s->req.version_major = 2;
    hs->s->req.version_minor = 0;
    hs->s->req.scheme = h->c->ssl ? "https" : "http";
    snprintf(hs->s->req.client_ip, sizeof(hs->s->req.client_ip), "%s", h->c->peer_ip);
    hs->next = h->streams;
    h->streams = hs;
    h->nstreams++;
    return hs;
}

static void h2_stream_release(h2_conn_t *h, h2_stream_t *hs)
{
    for (h2_stream_t **pp = &h->streams; *pp; pp = &(*pp)->next)
        if (*pp == hs) {
            *pp = hs->next;
            break;
        }
    h->nstreams--;
    if (hs->s) {
        if (hs->dispatched && !hs->responded)
            stream_abort(hs->s); /* cancels upstream work */
        else
            stream_free(hs->s);
    }
    free(hs);
}

/* ------------------------------------------------------------------------
 * Response path
 * ---------------------------------------------------------------------- */
static nghttp2_ssize body_read_cb(nghttp2_session *session, int32_t stream_id, uint8_t *buf,
                                  size_t length, uint32_t *data_flags, nghttp2_data_source *source,
                                  void *user_data)
{
    h2_stream_t *hs = source->ptr;
    const buf_t *body = &hs->s->resp.body;
    size_t remaining = buf_rlen(body) - hs->body_off;
    size_t n = MIN(length, remaining);
    memcpy(buf, buf_rptr(body) + hs->body_off, n);
    hs->body_off += n;
    if (hs->body_off == buf_rlen(body))
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
    return (nghttp2_ssize)n;
}

/* Headers that only make sense for HTTP/1 connections (RFC 9113 §8.2.2).
 * content-length is regenerated from the buffered body. */
static bool h2_forbidden_header(const char *name, size_t len)
{
    return str_ieq_lit(name, len, "connection") || str_ieq_lit(name, len, "keep-alive") ||
           str_ieq_lit(name, len, "transfer-encoding") || str_ieq_lit(name, len, "upgrade") ||
           str_ieq_lit(name, len, "proxy-connection") || str_ieq_lit(name, len, "content-length");
}

static void h2_respond(http_stream_t *s)
{
    h2_stream_t *hs = s->frontend;
    h2_conn_t *h = hs->h;
    http_response_t *resp = &s->resp;
    bool head = http_request_method_is(&s->req, "HEAD");

    /* nghttp2 wants lowercase names; build them in a scratch buffer. */
    size_t nh = headers_count(&resp->headers);
    nghttp2_nv *nva = xcalloc(nh + 3, sizeof(*nva));
    buf_t scratch;
    buf_init(&scratch);
    buf_reserve(&scratch, resp->headers.arena.len + 64);
    size_t n = 0;

    char status[4];
    snprintf(status, sizeof(status), "%d", resp->status);
    nva[n++] = (nghttp2_nv){(uint8_t *)":status", (uint8_t *)status, 7, 3, NGHTTP2_NV_FLAG_NONE};

    /* Remember offsets, since scratch may be reallocated while filling. */
    size_t *offs = xcalloc(nh * 2, sizeof(size_t));
    size_t m = 0;
    for (size_t i = 0; i < nh; i++) {
        size_t nl, vl;
        const char *nm = headers_name(&resp->headers, i, &nl);
        const char *v = headers_value(&resp->headers, i, &vl);
        if (h2_forbidden_header(nm, nl))
            continue;
        offs[m * 2] = scratch.len;
        for (size_t k = 0; k < nl; k++) {
            char lc = (char)tolower((unsigned char)nm[k]);
            buf_append(&scratch, &lc, 1);
        }
        offs[m * 2 + 1] = scratch.len;
        buf_append(&scratch, v, vl);
        nva[n + m] = (nghttp2_nv){NULL, NULL, nl, vl, NGHTTP2_NV_FLAG_NONE};
        m++;
    }
    for (size_t i = 0; i < m; i++) {
        nva[n + i].name = (uint8_t *)scratch.data + offs[i * 2];
        nva[n + i].value = (uint8_t *)scratch.data + offs[i * 2 + 1];
    }
    n += m;

    char clen[24];
    snprintf(clen, sizeof(clen), "%zu", buf_rlen(&resp->body));
    nva[n++] = (nghttp2_nv){(uint8_t *)"content-length", (uint8_t *)clen, 14, strlen(clen),
                            NGHTTP2_NV_FLAG_NONE};

    nghttp2_data_provider2 prd = {.source.ptr = hs, .read_callback = body_read_cb};
    bool has_body = !head && buf_rlen(&resp->body) > 0;
    int rv = nghttp2_submit_response2(h->session, hs->id, nva, n, has_body ? &prd : NULL);
    free(offs);
    buf_free(&scratch);
    free(nva);
    if (rv != 0) {
        LOG_WARN("h2: submit_response failed: %s", nghttp2_strerror(rv));
        nghttp2_submit_rst_stream(h->session, NGHTTP2_FLAG_NONE, hs->id, NGHTTP2_INTERNAL_ERROR);
    }
    hs->responded = true;
    /* The stream object stays alive until nghttp2 reports the stream closed
     * (on_stream_close_cb), since the data provider reads from it. */
    if (!h->in_callback)
        h2_send(h);
}

/* ------------------------------------------------------------------------
 * nghttp2 callbacks (request path)
 * ---------------------------------------------------------------------- */
static int on_begin_headers_cb(nghttp2_session *session, const nghttp2_frame *frame,
                               void *user_data)
{
    h2_conn_t *h = user_data;
    if (frame->hd.type != NGHTTP2_HEADERS || frame->headers.cat != NGHTTP2_HCAT_REQUEST)
        return 0;
    h2_stream_t *hs = h2_stream_new(h, frame->hd.stream_id);
    nghttp2_session_set_stream_user_data(session, frame->hd.stream_id, hs);
    return 0;
}

static int on_header_cb(nghttp2_session *session, const nghttp2_frame *frame, const uint8_t *name,
                        size_t namelen, const uint8_t *value, size_t valuelen, uint8_t flags,
                        void *user_data)
{
    h2_stream_t *hs = nghttp2_session_get_stream_user_data(session, frame->hd.stream_id);
    if (!hs || frame->hd.type != NGHTTP2_HEADERS)
        return 0;
    http_request_t *req = &hs->s->req;
    const char *n = (const char *)name, *v = (const char *)value;

    if (namelen > 0 && n[0] == ':') {
        if (str_ieq_lit(n, namelen, ":method")) {
            if (valuelen >= HTTP_METHOD_MAX)
                return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
            memcpy(req->method, v, valuelen);
            req->method_len = valuelen;
        } else if (str_ieq_lit(n, namelen, ":path")) {
            http_request_set_target(req, v, valuelen);
        } else if (str_ieq_lit(n, namelen, ":authority")) {
            /* Upstreams speak HTTP/1.1, which wants Host. A literal "host"
             * header (allowed by RFC 9113 §8.3.1) would duplicate it. */
            headers_remove(&req->headers, "host");
            headers_add(&req->headers, "Host", 4, v, valuelen);
        } else if (str_ieq_lit(n, namelen, ":scheme")) {
            req->scheme = valuelen == 5 && memcmp(v, "https", 5) == 0 ? "https" : "http";
        }
        return 0;
    }
    const char *hv;
    size_t hvl;
    if (str_ieq_lit(n, namelen, "host") && headers_get(&req->headers, "Host", &hv, &hvl))
        return 0; /* :authority already produced Host */
    if (hs->s->worker->srv->cfg->max_header_size <
        req->headers.arena.len + namelen + valuelen) {
        nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE, frame->hd.stream_id,
                                  NGHTTP2_ENHANCE_YOUR_CALM);
        return 0;
    }
    headers_add(&req->headers, n, namelen, v, valuelen);
    return 0;
}

static int on_data_chunk_recv_cb(nghttp2_session *session, uint8_t flags, int32_t stream_id,
                                 const uint8_t *data, size_t len, void *user_data)
{
    h2_stream_t *hs = nghttp2_session_get_stream_user_data(session, stream_id);
    if (!hs)
        return 0;
    if (buf_rlen(&hs->s->req.body) + len > hs->s->worker->srv->cfg->max_body_size) {
        nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE, stream_id, NGHTTP2_ENHANCE_YOUR_CALM);
        return 0;
    }
    buf_append(&hs->s->req.body, data, len);
    return 0;
}

static void h2_dispatch(h2_conn_t *h, h2_stream_t *hs)
{
    http_request_t *req = &hs->s->req;
    if (req->method_len == 0 || !req->target) {
        nghttp2_submit_rst_stream(h->session, NGHTTP2_FLAG_NONE, hs->id, NGHTTP2_PROTOCOL_ERROR);
        return;
    }
    /* CONNECT has no :path and is not supported by this proxy. */
    hs->dispatched = true;
    server_dispatch(h->c->w->srv, hs->s);
}

static int on_frame_recv_cb(nghttp2_session *session, const nghttp2_frame *frame, void *user_data)
{
    h2_conn_t *h = user_data;
    switch (frame->hd.type) {
    case NGHTTP2_HEADERS:
    case NGHTTP2_DATA:
        if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) {
            h2_stream_t *hs = nghttp2_session_get_stream_user_data(session, frame->hd.stream_id);
            if (hs && !hs->dispatched)
                h2_dispatch(h, hs);
        }
        break;
    default:
        break;
    }
    return 0;
}

static int on_stream_close_cb(nghttp2_session *session, int32_t stream_id, uint32_t error_code,
                              void *user_data)
{
    h2_conn_t *h = user_data;
    h2_stream_t *hs = nghttp2_session_get_stream_user_data(session, stream_id);
    if (hs)
        h2_stream_release(h, hs);
    return 0;
}

/* ------------------------------------------------------------------------
 * Transport glue
 * ---------------------------------------------------------------------- */
static void h2_send(h2_conn_t *h)
{
    conn_t *c = h->c;
    if (c->closed)
        return;
    while (buf_rlen(&c->out) < H2_OUT_HIGH_WATER) {
        const uint8_t *data;
        nghttp2_ssize n = nghttp2_session_mem_send2(h->session, &data);
        if (n <= 0)
            break;
        buf_append(&c->out, data, (size_t)n);
    }
    if (!nghttp2_session_want_read(h->session) && !nghttp2_session_want_write(h->session) &&
        buf_empty(&c->out)) {
        conn_close(c);
        return;
    }
    conn_flush(c);
}

/* Graceful shutdown: tell the client not to open new streams. Requests
 * already in flight still complete; the connection closes once nghttp2 has
 * nothing left to do (see h2_send). */
static void h2_goaway(h2_conn_t *h)
{
    if (h->goaway_sent)
        return;
    h->goaway_sent = true;
    nghttp2_submit_goaway(h->session, NGHTTP2_FLAG_NONE,
                          nghttp2_session_get_last_proc_stream_id(h->session), NGHTTP2_NO_ERROR,
                          NULL, 0);
}

static void h2_on_data(conn_t *c)
{
    h2_conn_t *h = c->proto;
    if (c->w->draining)
        h2_goaway(h);
    h->in_callback = true;
    nghttp2_ssize n = nghttp2_session_mem_recv2(h->session, (const uint8_t *)buf_rptr(&c->in),
                                                buf_rlen(&c->in));
    h->in_callback = false;
    if (n < 0) {
        LOG_DEBUG("%s: h2 protocol error: %s", c->peer_ip, nghttp2_strerror((int)n));
        conn_close(c);
        return;
    }
    buf_consume(&c->in, (size_t)n);
    conn_set_timeout(c, c->w->srv->cfg->idle_timeout_ms);
    h2_send(h);
}

static void h2_on_drained(conn_t *c)
{
    h2_conn_t *h = c->proto;
    if (nghttp2_session_want_write(h->session))
        h2_send(h);
    else if (!nghttp2_session_want_read(h->session))
        conn_close(c);
}

static void h2_on_timeout(conn_t *c)
{
    h2_conn_t *h = c->proto;
    if (h->nstreams > 0) {
        /* Requests in flight: upstream timeouts bound them. Check again. */
        conn_set_timeout(c, c->w->srv->cfg->idle_timeout_ms);
        return;
    }
    LOG_DEBUG("%s: h2 idle timeout", c->peer_ip);
    h2_goaway(h);
    h2_send(h); /* best effort: flush GOAWAY, then drop the connection */
    conn_close(c);
}

static void h2_on_close(conn_t *c)
{
    h2_conn_t *h = c->proto;
    if (!h)
        return;
    while (h->streams)
        h2_stream_release(h, h->streams);
    nghttp2_session_del(h->session);
    free(h);
    c->proto = NULL;
}

static const conn_ops_t h2_ops = {
    .on_data = h2_on_data,
    .on_drained = h2_on_drained,
    .on_timeout = h2_on_timeout,
    .on_close = h2_on_close,
};

void h2_conn_attach(conn_t *c)
{
    h2_conn_t *h = xcalloc(1, sizeof(*h));
    h->c = c;

    nghttp2_session_callbacks *cbs;
    nghttp2_session_callbacks_new(&cbs);
    nghttp2_session_callbacks_set_on_begin_headers_callback(cbs, on_begin_headers_cb);
    nghttp2_session_callbacks_set_on_header_callback(cbs, on_header_cb);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cbs, on_data_chunk_recv_cb);
    nghttp2_session_callbacks_set_on_frame_recv_callback(cbs, on_frame_recv_cb);
    nghttp2_session_callbacks_set_on_stream_close_callback(cbs, on_stream_close_cb);
    nghttp2_session_server_new(&h->session, cbs, h);
    nghttp2_session_callbacks_del(cbs);

    nghttp2_settings_entry iv[] = {
        {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, H2_MAX_CONCURRENT_STREAMS},
        {NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, 1 << 20},
        {NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE, (uint32_t)c->w->srv->cfg->max_header_size},
    };
    nghttp2_submit_settings(h->session, NGHTTP2_FLAG_NONE, iv, ARRAY_LEN(iv));
    /* Larger connection window so big responses are not throttled. */
    nghttp2_session_set_local_window_size(h->session, NGHTTP2_FLAG_NONE, 0, 8 << 20);

    conn_set_ops(c, &h2_ops, h);
    conn_set_timeout(c, c->w->srv->cfg->idle_timeout_ms);
    conn_want_read(c, true);
    h2_send(h); /* server preface: SETTINGS */
    if (!buf_empty(&c->in))
        h2_on_data(c); /* client preface may have arrived with the handshake */
}
