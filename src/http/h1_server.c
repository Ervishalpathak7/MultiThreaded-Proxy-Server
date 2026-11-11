/*
 * h1_server.c - HTTP/1.1 frontend (see h1_server.h).
 *
 * Connection state machine:
 *
 *   IDLE ──bytes──▶ READING_HEAD ──complete──▶ IN_FLIGHT ──respond──▶ WRITING
 *     ▲                 │ timeout/error                                   │
 *     │                 ▼                                                 │
 *     │              [error response, close]           keep-alive? ───────┘
 *     └────────────────────────────────────────────────── yes
 *                                                        no → close
 */
#include "http/h1_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config/config.h"
#include "core/event_loop.h"
#include "core/log.h"
#include "core/util.h"
#include "http/h1_parser.h"
#include "http/stream.h"
#include "net/listener.h"
#include "net/worker.h"
#include "server.h"

typedef struct h1_conn {
    conn_t *c;
    h1_parser_t parser;
    http_stream_t *stream; /* request being parsed or in flight */
    bool in_flight;        /* dispatched, waiting for the pipeline */
    bool close_after_write;
    bool head_request;
    size_t requests_served;
} h1_conn_t;

static void h1_process_input(conn_t *c);

/* ------------------------------------------------------------------------
 * Response path
 * ---------------------------------------------------------------------- */
static void h1_write_response(h1_conn_t *h, http_response_t *resp, bool keep_alive, bool head)
{
    conn_t *c = h->c;
    size_t body_len = buf_rlen(&resp->body);
    h1_write_response_head(&c->out, resp, body_len, keep_alive);
    if (!head && body_len)
        buf_append(&c->out, buf_rptr(&resp->body), body_len);
    h->close_after_write = !keep_alive;
}

/* Pipeline finished: serialize and send. */
static void h1_respond(http_stream_t *s)
{
    h1_conn_t *h = s->frontend;
    conn_t *c = h->c;

    bool keep_alive = s->req.keep_alive && !c->w->draining;
    h1_write_response(h, &s->resp, keep_alive, h->head_request);
    h->requests_served++;

    stream_free(s);
    h->stream = NULL;
    h->in_flight = false;

    conn_flush(c); /* may call on_drained synchronously */
}

/* Parse failure: answer with the parser's status and close. */
static void h1_send_error(h1_conn_t *h, int status, const char *reason)
{
    http_response_t resp;
    http_response_init(&resp);
    http_response_set_error(&resp, status, reason);
    h1_write_response(h, &resp, false, false);
    http_response_free(&resp);
    if (h->stream) {
        stream_free(h->stream);
        h->stream = NULL;
    }
    conn_want_read(h->c, false);
    conn_flush(h->c);
}

/* ------------------------------------------------------------------------
 * Request path
 * ---------------------------------------------------------------------- */
static void h1_process_input(conn_t *c)
{
    h1_conn_t *h = c->proto;
    const config_t *cfg = c->w->srv->cfg;

    if (h->in_flight)
        return; /* pipelined bytes wait their turn */

    if (!h->stream) {
        h->stream = stream_new(c->w, h1_respond, h);
        h1_parser_reset(&h->parser);
        /* Time limit for the whole request head to arrive. */
        conn_set_timeout(c, cfg->header_timeout_ms);
    }

    h1_result_t r = h1_parse_request(&h->parser, &c->in, &h->stream->req);
    if (r == H1_NEED_MORE)
        return;
    if (r == H1_ERROR) {
        LOG_DEBUG("%s: bad request: %s", c->peer_ip, h->parser.error_reason);
        h1_send_error(h, h->parser.error_status, h->parser.error_reason);
        return;
    }

    /* Complete request. Hand it to the pipeline. */
    http_stream_t *s = h->stream;
    h->in_flight = true;
    h->head_request = http_request_method_is(&s->req, "HEAD");
    conn_clear_timeout(c);
    conn_want_read(c, false); /* back-pressure until the response is out */

    snprintf(s->req.client_ip, sizeof(s->req.client_ip), "%s", c->peer_ip);
    s->req.scheme = c->ssl ? "https" : "http";

    server_dispatch(c->w->srv, s);
}

/* ------------------------------------------------------------------------
 * conn_ops
 * ---------------------------------------------------------------------- */
static void h1_on_data(conn_t *c)
{
    h1_process_input(c);
}

static void h1_on_drained(conn_t *c)
{
    h1_conn_t *h = c->proto;
    if (h->close_after_write) {
        conn_close(c);
        return;
    }
    if (h->in_flight)
        return;
    /* Keep-alive: wait for the next request, or process one already read. */
    conn_want_read(c, true);
    conn_set_timeout(c, c->w->srv->cfg->idle_timeout_ms);
    if (!buf_empty(&c->in))
        h1_process_input(c);
}

static void h1_on_timeout(conn_t *c)
{
    h1_conn_t *h = c->proto;
    if (h->in_flight)
        return; /* upstream has its own timeout */
    if (h->stream && h1_parser_head_done(&h->parser) == false && !buf_empty(&c->in)) {
        /* Partial head received: tell the client it was too slow. */
        h1_send_error(h, 408, "request header timeout");
        return;
    }
    LOG_DEBUG("%s: idle timeout", c->peer_ip);
    conn_close(c);
}

static void h1_on_close(conn_t *c)
{
    h1_conn_t *h = c->proto;
    if (!h)
        return;
    if (h->stream) {
        if (h->in_flight)
            stream_abort(h->stream); /* cancels pending upstream work */
        else
            stream_free(h->stream);
        h->stream = NULL;
    }
    free(h);
    c->proto = NULL;
}

static const conn_ops_t h1_ops = {
    .on_data = h1_on_data,
    .on_drained = h1_on_drained,
    .on_timeout = h1_on_timeout,
    .on_close = h1_on_close,
};

void h1_conn_attach(conn_t *c)
{
    const config_t *cfg = c->w->srv->cfg;
    h1_conn_t *h = xcalloc(1, sizeof(*h));
    h->c = c;
    h1_parser_init(&h->parser, H1_MODE_REQUEST, cfg->max_header_size, cfg->max_body_size);
    conn_set_ops(c, &h1_ops, h);
    conn_set_timeout(c, cfg->header_timeout_ms);
    if (!c->ssl)
        conn_start(c);
    else {
        /* TLS already registered the fd; just make sure we are reading and
         * process any application data that arrived with the handshake. */
        conn_want_read(c, true);
        if (!buf_empty(&c->in))
            h1_process_input(c);
    }
}
