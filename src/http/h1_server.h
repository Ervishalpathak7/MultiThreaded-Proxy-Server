/*
 * h1_server.h - HTTP/1.1 frontend.
 *
 * Attaches to a conn_t and turns bytes into http_stream_t exchanges:
 * parse request -> dispatch -> serialize response -> keep-alive loop.
 * Requests on one connection are processed strictly one at a time
 * (pipelined requests stay in the input buffer until the previous response
 * is written), which is what RFC 9112 requires of response ordering anyway.
 */
#ifndef APROXY_HTTP_H1_SERVER_H
#define APROXY_HTTP_H1_SERVER_H

#include "net/conn.h"

/* Install the HTTP/1.1 handler on a connection that is ready for
 * application data (plaintext, or TLS with ALPN http/1.1). */
void h1_conn_attach(conn_t *c);

#endif /* APROXY_HTTP_H1_SERVER_H */
