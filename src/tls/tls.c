/*
 * tls.c - TLS termination (see tls.h).
 */
#include "tls/tls.h"

#include <errno.h>
#include <openssl/err.h>
#include <stdlib.h>
#include <string.h>

#include "core/log.h"
#include "core/util.h"
#include "http/h1_server.h"
#include "net/worker.h"
#include "server.h"
#if HAVE_HTTP2
#include "http/h2_server.h"
#endif

typedef struct tls_host {
    char *hostname; /* NULL = default */
    SSL_CTX *ctx;
} tls_host_t;

struct tls_ctx {
    tls_host_t *hosts;
    size_t nhosts;
    SSL_CTX *default_ctx;
};

/* ALPN list in wire format: length-prefixed names, most preferred first. */
#if HAVE_HTTP2
static const unsigned char alpn_tcp[] = "\x02h2\x08http/1.1";
#else
static const unsigned char alpn_tcp[] = "\x08http/1.1";
#endif

static void log_ssl_errors(const char *what)
{
    unsigned long e;
    while ((e = ERR_get_error()) != 0) {
        char buf[256];
        ERR_error_string_n(e, buf, sizeof(buf));
        LOG_ERROR("%s: %s", what, buf);
    }
}

/* ------------------------------------------------------------------------
 * Callbacks
 * ---------------------------------------------------------------------- */

/* SNI: switch to the context holding the certificate for the named host. */
static int servername_cb(SSL *ssl, int *al, void *arg)
{
    tls_ctx_t *t = arg;
    const char *name = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
    if (!name)
        return SSL_TLSEXT_ERR_OK;
    for (size_t i = 0; i < t->nhosts; i++) {
        if (t->hosts[i].hostname && strcasecmp(t->hosts[i].hostname, name) == 0) {
            SSL_set_SSL_CTX(ssl, t->hosts[i].ctx);
            return SSL_TLSEXT_ERR_OK;
        }
    }
    return SSL_TLSEXT_ERR_OK; /* unknown host: keep the default certificate */
}

typedef struct {
    const unsigned char *alpn;
    unsigned int alpn_len;
} alpn_pref_t;

static int alpn_select_cb(SSL *ssl, const unsigned char **out, unsigned char *outlen,
                          const unsigned char *in, unsigned int inlen, void *arg)
{
    const alpn_pref_t *pref = arg;
    /* Picks the first of *our* protocols that the client also offers. */
    if (SSL_select_next_proto((unsigned char **)out, outlen, pref->alpn, pref->alpn_len, in,
                              inlen) == OPENSSL_NPN_NEGOTIATED)
        return SSL_TLSEXT_ERR_OK;
    return SSL_TLSEXT_ERR_NOACK; /* no overlap: continue without ALPN */
}

/* ------------------------------------------------------------------------
 * Context setup
 * ---------------------------------------------------------------------- */
static int apply_cert(SSL_CTX *ctx, const config_cert_t *c)
{
    if (SSL_CTX_use_certificate_chain_file(ctx, c->cert_file) != 1) {
        log_ssl_errors(c->cert_file);
        return -1;
    }
    if (SSL_CTX_use_PrivateKey_file(ctx, c->key_file, SSL_FILETYPE_PEM) != 1) {
        log_ssl_errors(c->key_file);
        return -1;
    }
    if (SSL_CTX_check_private_key(ctx) != 1) {
        LOG_ERROR("%s does not match %s", c->key_file, c->cert_file);
        return -1;
    }
    return 0;
}

static void harden(SSL_CTX *ctx)
{
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_options(ctx, SSL_OP_NO_COMPRESSION | SSL_OP_CIPHER_SERVER_PREFERENCE);
    /* Session resumption: tickets are on by default; keep a small cache
     * for clients that do not support them. */
    SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_SERVER);
    SSL_CTX_sess_set_cache_size(ctx, 20480);
    SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER |
                              SSL_MODE_RELEASE_BUFFERS);
}

int tls_ctx_configure(tls_ctx_t *t, SSL_CTX *ctx, const unsigned char *alpn, size_t alpn_len)
{
    harden(ctx);
    /* The default certificate lives in the caller's context; per-host
     * certificates are switched in by the SNI callback. */
    const config_cert_t *def = NULL;
    for (size_t i = 0; i < t->nhosts; i++)
        if (!t->hosts[i].hostname)
            def = SSL_CTX_get_app_data(t->hosts[i].ctx);
    if (!def)
        def = SSL_CTX_get_app_data(t->hosts[0].ctx);
    if (apply_cert(ctx, def) < 0)
        return -1;
    SSL_CTX_set_tlsext_servername_callback(ctx, servername_cb);
    SSL_CTX_set_tlsext_servername_arg(ctx, t);

    alpn_pref_t *pref = xmalloc(sizeof(*pref));
    pref->alpn = alpn;
    pref->alpn_len = (unsigned int)alpn_len;
    SSL_CTX_set_alpn_select_cb(ctx, alpn_select_cb, pref);
    SSL_CTX_set_ex_data(ctx, 1, pref); /* freed with the context by the owner */
    return 0;
}

tls_ctx_t *tls_ctx_new(const config_t *cfg)
{
    if (cfg->ncerts == 0) {
        LOG_ERROR("TLS requested but no certificates configured");
        return NULL;
    }
    tls_ctx_t *t = xcalloc(1, sizeof(*t));
    t->hosts = xcalloc(cfg->ncerts, sizeof(tls_host_t));

    for (size_t i = 0; i < cfg->ncerts; i++) {
        const config_cert_t *c = &cfg->certs[i];
        SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
        if (!ctx || apply_cert(ctx, c) < 0) {
            if (ctx)
                SSL_CTX_free(ctx);
            tls_ctx_free(t);
            return NULL;
        }
        harden(ctx);
        SSL_CTX_set_app_data(ctx, (void *)c);
        t->hosts[i].hostname = c->hostname ? xstrdup(c->hostname) : NULL;
        t->hosts[i].ctx = ctx;
        t->nhosts++;
        LOG_INFO("loaded certificate %s for %s", c->cert_file, c->hostname ? c->hostname : "(default)");
    }

    /* The listener context: default certificate + SNI + ALPN. */
    t->default_ctx = SSL_CTX_new(TLS_server_method());
    if (!t->default_ctx || tls_ctx_configure(t, t->default_ctx, alpn_tcp, sizeof(alpn_tcp) - 1) < 0) {
        tls_ctx_free(t);
        return NULL;
    }
    /* Per-host contexts need the ALPN callback too, since the SSL object
     * switches to them before ALPN is negotiated. */
    for (size_t i = 0; i < t->nhosts; i++) {
        alpn_pref_t *pref = xmalloc(sizeof(*pref));
        pref->alpn = alpn_tcp;
        pref->alpn_len = sizeof(alpn_tcp) - 1;
        SSL_CTX_set_alpn_select_cb(t->hosts[i].ctx, alpn_select_cb, pref);
        SSL_CTX_set_ex_data(t->hosts[i].ctx, 1, pref);
    }
    return t;
}

static void ctx_free(SSL_CTX *ctx)
{
    if (!ctx)
        return;
    free(SSL_CTX_get_ex_data(ctx, 1));
    SSL_CTX_free(ctx);
}

void tls_ctx_free(tls_ctx_t *t)
{
    if (!t)
        return;
    for (size_t i = 0; i < t->nhosts; i++) {
        free(t->hosts[i].hostname);
        ctx_free(t->hosts[i].ctx);
    }
    free(t->hosts);
    ctx_free(t->default_ctx);
    free(t);
}

/* ------------------------------------------------------------------------
 * Per-connection handshake
 * ---------------------------------------------------------------------- */
void tls_conn_start(conn_t *c, tls_ctx_t *t)
{
    c->ssl = SSL_new(t->default_ctx);
    if (!c->ssl) {
        log_ssl_errors("SSL_new");
        conn_close(c);
        return;
    }
    SSL_set_fd(c->ssl, c->fd);
    SSL_set_accept_state(c->ssl);
    c->tls_handshaking = true;
    /* Bound the handshake so idle TCP connections cannot pile up. */
    conn_set_timeout(c, c->w->srv->cfg->header_timeout_ms);
    conn_start(c); /* registers for EV_READ; on_io drives the handshake */
    tls_conn_handshake(c);
}

static void attach_frontend(conn_t *c)
{
    const unsigned char *proto = NULL;
    unsigned int plen = 0;
    SSL_get0_alpn_selected(c->ssl, &proto, &plen);
    LOG_DEBUG("%s: TLS %s %s alpn=%.*s", c->peer_ip, SSL_get_version(c->ssl),
              SSL_get_cipher_name(c->ssl), (int)plen, proto ? (const char *)proto : "-");
#if HAVE_HTTP2
    if (plen == 2 && memcmp(proto, "h2", 2) == 0) {
        h2_conn_attach(c);
        return;
    }
#endif
    /* "http/1.1" or no ALPN at all: HTTP/1.1 is the default. */
    h1_conn_attach(c);
}

void tls_conn_handshake(conn_t *c)
{
    if (c->closed || !c->tls_handshaking)
        return;
    ERR_clear_error();
    int rc = SSL_do_handshake(c->ssl);
    if (rc == 1) {
        c->tls_handshaking = false;
        c->tls_want_write = false;
        conn_clear_timeout(c);
        conn_update_interest(c);
        attach_frontend(c);
        return;
    }
    int err = SSL_get_error(c->ssl, rc);
    switch (err) {
    case SSL_ERROR_WANT_READ:
        c->tls_want_write = false;
        conn_update_interest(c);
        return;
    case SSL_ERROR_WANT_WRITE:
        c->tls_want_write = true;
        conn_update_interest(c);
        return;
    default:
        if (err == SSL_ERROR_SYSCALL && errno == 0)
            LOG_DEBUG("%s: peer closed during handshake", c->peer_ip);
        else {
            unsigned long e = ERR_peek_error();
            char buf[256] = "unknown";
            if (e)
                ERR_error_string_n(e, buf, sizeof(buf));
            LOG_DEBUG("%s: TLS handshake failed: %s", c->peer_ip, buf);
            ERR_clear_error();
        }
        conn_close(c);
        return;
    }
}

void tls_conn_free(conn_t *c)
{
    if (!c->ssl)
        return;
    /* Best-effort close_notify; the socket is closed right after anyway. */
    if (!c->tls_handshaking)
        SSL_shutdown(c->ssl);
    SSL_free(c->ssl);
    c->ssl = NULL;
}

/* ------------------------------------------------------------------------
 * Application data
 * ---------------------------------------------------------------------- */
ssize_t tls_conn_read(conn_t *c, void *buf, size_t len)
{
    ERR_clear_error();
    size_t n = 0;
    if (SSL_read_ex(c->ssl, buf, len, &n) == 1)
        return (ssize_t)n;
    int err = SSL_get_error(c->ssl, 0);
    switch (err) {
    case SSL_ERROR_WANT_READ:
        errno = EAGAIN;
        return -1;
    case SSL_ERROR_WANT_WRITE:
        /* Renegotiation / key update needs to send first. */
        c->tls_want_write = true;
        conn_update_interest(c);
        errno = EAGAIN;
        return -1;
    case SSL_ERROR_ZERO_RETURN:
        return 0; /* clean close_notify */
    case SSL_ERROR_SYSCALL:
        if (errno == 0)
            return 0; /* unexpected EOF, treat like close */
        return -1;
    default:
        ERR_clear_error();
        errno = EIO;
        return -1;
    }
}

ssize_t tls_conn_write(conn_t *c, const void *buf, size_t len)
{
    ERR_clear_error();
    size_t n = 0;
    if (SSL_write_ex(c->ssl, buf, len, &n) == 1)
        return (ssize_t)n;
    int err = SSL_get_error(c->ssl, 0);
    if (err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ) {
        errno = EAGAIN;
        return -1;
    }
    if (err == SSL_ERROR_SYSCALL && errno != 0)
        return -1;
    ERR_clear_error();
    errno = EPIPE;
    return -1;
}
