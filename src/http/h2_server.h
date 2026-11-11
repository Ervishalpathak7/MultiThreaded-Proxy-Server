/*
 * h2_server.h - HTTP/2 frontend built on nghttp2.
 *
 * nghttp2 handles framing, HPACK, flow control and stream multiplexing;
 * this module adapts its callbacks to the conn_t transport and the
 * http_stream_t pipeline. Each HTTP/2 stream becomes one http_stream_t,
 * so many requests can be in flight concurrently on one connection.
 *
 * Only reachable over TLS with ALPN "h2" (no h2c upgrade).
 */
#ifndef APROXY_HTTP_H2_SERVER_H
#define APROXY_HTTP_H2_SERVER_H

#include "net/conn.h"

void h2_conn_attach(conn_t *c);

#endif /* APROXY_HTTP_H2_SERVER_H */
