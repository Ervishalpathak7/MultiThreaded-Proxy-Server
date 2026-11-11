/*
 * stream.h - one request/response exchange, independent of the transport.
 *
 * Every frontend (HTTP/1.1 connection, HTTP/2 stream, HTTP/3 stream) creates
 * an http_stream_t once a complete request has arrived and hands it to the
 * request pipeline (router -> cache -> load balancer -> upstream). The
 * pipeline is asynchronous; when it has a response it calls
 * `stream->respond(stream)`, which the frontend implements to serialize the
 * response in its own wire format and then release the stream.
 *
 *     frontend                      pipeline
 *     --------                      --------
 *     stream_new(...)
 *     fill stream->req
 *     server_dispatch(stream)  --->  look up cache / talk to upstream
 *                              <---  stream->respond(stream)
 *     write stream->resp
 *     stream_free(stream)
 *
 * If the client goes away first, the frontend calls stream_abort(): the
 * pipeline cancels pending upstream work and the respond callback is never
 * invoked.
 */
#ifndef APROXY_HTTP_STREAM_H
#define APROXY_HTTP_STREAM_H

#include <stdbool.h>
#include <stdint.h>

#include "http/request.h"

typedef struct http_stream http_stream_t;
typedef struct worker worker_t;

typedef void (*stream_respond_fn)(http_stream_t *s);
typedef void (*stream_cancel_fn)(http_stream_t *s);

struct http_stream {
    worker_t *worker; /* owning worker: event loop, upstream pool */
    http_request_t req;
    http_response_t resp;

    /* set by the frontend */
    stream_respond_fn respond;
    void *frontend;    /* frontend private pointer (conn, h2 session...) */
    int64_t stream_id; /* H2/H3 stream id, -1 for HTTP/1 */

    /* set by the pipeline while work is pending */
    stream_cancel_fn cancel;
    void *pipeline;

    bool aborted;
    bool cache_hit;
    uint64_t start_ms;
};

http_stream_t *stream_new(worker_t *w, stream_respond_fn respond, void *frontend);
void stream_free(http_stream_t *s);

/* Frontend lost the client: cancel pipeline work, free the stream. */
void stream_abort(http_stream_t *s);

/* Pipeline helpers: fill resp with an error page and deliver it. */
void stream_respond_error(http_stream_t *s, int status, const char *message);
/* Deliver the response currently in s->resp (adds Date / Via / Server). */
void stream_finish(http_stream_t *s);

#endif /* APROXY_HTTP_STREAM_H */
