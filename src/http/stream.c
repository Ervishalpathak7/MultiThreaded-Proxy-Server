/*
 * stream.c - request/response exchange lifecycle (see stream.h).
 */
#include "http/stream.h"

#include <stdlib.h>
#include <string.h>

#include "core/event_loop.h"
#include "core/log.h"
#include "core/util.h"
#include "net/worker.h"
#include "server.h"

http_stream_t *stream_new(worker_t *w, stream_respond_fn respond, void *frontend)
{
    http_stream_t *s = xcalloc(1, sizeof(*s));
    s->worker = w;
    s->respond = respond;
    s->frontend = frontend;
    s->stream_id = -1;
    http_request_init(&s->req);
    http_response_init(&s->resp);
    s->start_ms = ev_now(w->loop);
    atomic_fetch_add_explicit(&w->srv->stats.requests, 1, memory_order_relaxed);
    return s;
}

void stream_free(http_stream_t *s)
{
    if (!s)
        return;
    http_request_free(&s->req);
    http_response_free(&s->resp);
    free(s->cache_key);
    free(s);
}

void stream_abort(http_stream_t *s)
{
    if (!s)
        return;
    s->aborted = true;
    if (s->cancel) {
        /* The pipeline releases anything it attached under s->pipeline. */
        s->cancel(s);
        s->cancel = NULL;
    }
    stream_free(s);
}

void stream_finish(http_stream_t *s)
{
    if (s->aborted)
        return;
    s->cancel = NULL;
    s->pipeline = NULL;

    /* Access log: one line per request, like nginx's combined format but
     * shorter. Emitted at INFO so it can be silenced with log_level warn. */
    uint64_t took = ev_now(s->worker->loop) - s->start_ms;
    LOG_INFO("%s \"%.*s %.*s\" %d %zuB %llums%s", s->req.client_ip, (int)s->req.method_len,
             s->req.method, (int)s->req.target_len, s->req.target ? s->req.target : "",
             s->resp.status, buf_rlen(&s->resp.body), (unsigned long long)took,
             s->cache_hit ? " cache=HIT" : "");

    atomic_fetch_add_explicit(&s->worker->srv->stats.bytes_out, buf_rlen(&s->resp.body),
                              memory_order_relaxed);
    s->respond(s);
}

void stream_respond_error(http_stream_t *s, int status, const char *message)
{
    http_response_set_error(&s->resp, status, message);
    stream_finish(s);
}
