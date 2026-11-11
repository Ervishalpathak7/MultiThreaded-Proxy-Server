/*
 * tls.h - TLS termination with OpenSSL.
 *
 * One SSL_CTX is built per configured certificate. The default one is used
 * for a connection until the ClientHello's SNI names a host that has its
 * own certificate, at which point the SSL object is switched to that
 * context. ALPN offers "h2" (when built with nghttp2) and "http/1.1"; the
 * negotiated protocol decides which frontend is attached once the
 * handshake completes.
 *
 * Handshakes are fully non-blocking: tls_conn_handshake() is driven by
 * the connection's readiness events and sets the read/write interest the
 * handshake needs next.
 */
#ifndef APROXY_TLS_TLS_H
#define APROXY_TLS_TLS_H

#include <openssl/ssl.h>
#include <stddef.h>
#include <sys/types.h>

#include "config/config.h"
#include "net/conn.h"

typedef struct tls_ctx tls_ctx_t;

/* Load every certificate from the config. NULL on failure (logged). */
tls_ctx_t *tls_ctx_new(const config_t *cfg);
void tls_ctx_free(tls_ctx_t *t);

/* Install certificates, SNI and ALPN selection into an SSL_CTX created by
 * someone else (the QUIC frontend needs its own context). `alpn` is a
 * wire-format protocol list. */
int tls_ctx_configure(tls_ctx_t *t, SSL_CTX *ctx, const unsigned char *alpn, size_t alpn_len);
/* Pin the ALPN preference of one SSL object, overriding its context's.
 * `alpn` must outlive the SSL object. */
void tls_ssl_set_alpn(SSL *ssl, const unsigned char *alpn, size_t alpn_len);

/* Begin a TLS server handshake on a freshly accepted connection. On
 * completion the matching HTTP frontend is attached automatically. */
void tls_conn_start(conn_t *c, tls_ctx_t *t);
/* Advance the handshake; called by conn.c on readiness events. */
void tls_conn_handshake(conn_t *c);
void tls_conn_free(conn_t *c);

/* Application data I/O with read(2)/write(2) semantics (-1/EAGAIN when the
 * TLS engine needs the socket to become ready again, 0 on close_notify). */
ssize_t tls_conn_read(conn_t *c, void *buf, size_t len);
ssize_t tls_conn_write(conn_t *c, const void *buf, size_t len);

#endif /* APROXY_TLS_TLS_H */
