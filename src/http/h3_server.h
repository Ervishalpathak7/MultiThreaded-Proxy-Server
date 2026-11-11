/*
 * h3_server.h - HTTP/3 frontend over QUIC, built on ngtcp2 + nghttp3.
 *
 * QUIC runs over UDP, so there is no accept(): every connection's packets
 * arrive on the same socket and are demultiplexed by connection ID. The
 * endpoint therefore lives on exactly one worker's event loop (worker 0),
 * which owns the socket, the connection table and every QUIC connection.
 * Requests still flow through the shared http_stream_t pipeline, one
 * stream per HTTP/3 request stream, exactly like HTTP/2.
 *
 * Layering:
 *
 *   UDP socket ──▶ ngtcp2 (packets, loss recovery, flow control, TLS 1.3
 *                  via OpenSSL's QUIC TLS API)
 *              ──▶ nghttp3 (HTTP/3 framing, QPACK)
 *              ──▶ http_stream_t ──▶ server_dispatch()
 *
 * TCP responses carry an Alt-Svc header pointing here (see stream.c).
 */
#ifndef APROXY_HTTP_H3_SERVER_H
#define APROXY_HTTP_H3_SERVER_H

#include "net/worker.h"
#include "server.h"

typedef struct h3_endpoint h3_endpoint_t;

/* Open the UDP socket from cfg->http3_addr:http3_port on this worker's
 * loop. NULL on failure (logged). */
h3_endpoint_t *h3_endpoint_start(server_t *srv, worker_t *w);
/* Send CONNECTION_CLOSE to every client, close the socket, free state. */
void h3_endpoint_stop(h3_endpoint_t *ep);

#endif /* APROXY_HTTP_H3_SERVER_H */
