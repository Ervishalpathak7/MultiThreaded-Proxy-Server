/*
 * h3_server.c - HTTP/3 frontend (see h3_server.h).
 *
 * Connection lifetime:
 *
 *   Initial packet ──▶ h3_conn_new ──▶ handshake ──▶ requests ...
 *        │                                              │
 *        │      error / idle / shutdown                 ▼
 *        └──────────────────────────────▶ closing (we sent CONNECTION_CLOSE)
 *                                         draining (peer sent it)
 *                                              │ 3 × PTO
 *                                              ▼
 *                                          h3_conn_free
 *
 * Every packet read and every timer expiry ends with h3_conn_flush(), which
 * pulls HTTP/3 stream data out of nghttp3, lets ngtcp2 pack it into QUIC
 * packets, and re-arms the connection timer from ngtcp2's next expiry.
 */
#include "http/h3_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <nghttp3/nghttp3.h>
#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_ossl.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>

#include "config/config.h"
#include "core/event_loop.h"
#include "core/log.h"
#include "core/util.h"
#include "http/stream.h"
#include "net/listener.h"
#include "net/worker.h"
#include "tls/tls.h"

#define H3_SCIDLEN 16            /* length of the connection IDs we issue */
#define H3_MAX_STREAMS_BIDI 100  /* concurrent requests per connection */
#define H3_CID_BUCKETS 1024      /* connection table size (power of two) */
#define H3_SECRET_LEN 32         /* for stateless reset tokens */
#define H3_BUF_SIZE 65536        /* one datagram, in or out */
#define H3_RECV_BATCH 64         /* datagrams read per readiness event */
#define H3_FLUSH_MAX_PKTS 32     /* packets written per flush */

static const unsigned char alpn_h3[] = "\x02h3";

typedef struct h3_endpoint h3_endpoint_t;
typedef struct h3_conn h3_conn_t;

typedef struct h3_stream {
    h3_conn_t *hc;
    http_stream_t *s;
    int64_t id;
    bool dispatched; /* handed to the pipeline (or answered directly) */
    bool responded;  /* response submitted to nghttp3 */
    bool body_sent;  /* data reader handed out the body */
    bool bad;        /* malformed request head */
    bool too_large;  /* body exceeded max_body_size */
    struct h3_stream *next;
} h3_stream_t;

/* Connection table entry: one per connection ID currently routed to hc. */
typedef struct cid_entry {
    ngtcp2_cid cid;
    h3_conn_t *hc;
    struct cid_entry *next;      /* hash chain */
    struct cid_entry *conn_next; /* all ids of one connection */
} cid_entry_t;

struct h3_conn {
    h3_endpoint_t *ep;
    ngtcp2_conn *conn;
    nghttp3_conn *h3; /* created once the 1-RTT keys are installed */
    SSL *ssl;
    ngtcp2_crypto_ossl_ctx *ossl;
    ngtcp2_crypto_conn_ref conn_ref;
    ngtcp2_path_storage path;
    ngtcp2_ccerr last_error;
    timer_t_ timer;
    char peer_ip[64];

    cid_entry_t *cids;
    h3_stream_t *streams;
    size_t nstreams;

    /* Close handling. */
    bool closing;  /* we sent CONNECTION_CLOSE; close_pkt is re-sent on input */
    bool draining; /* peer sent CONNECTION_CLOSE; stay silent */
    uint8_t *close_pkt;
    size_t close_pkt_len;
    bool in_callback; /* inside ngtcp2: defer flushing */

    h3_conn_t *prev, *next;
};

struct h3_endpoint {
    server_t *srv;
    worker_t *w;
    int fd;
    SSL_CTX *ssl_ctx;
    struct sockaddr_storage local;
    socklen_t local_len;
    uint8_t secret[H3_SECRET_LEN];
    cid_entry_t *buckets[H3_CID_BUCKETS];
    h3_conn_t *conns;
    size_t nconns;
    uint8_t *rbuf;
    uint8_t *wbuf;
};

static void h3_conn_flush(h3_conn_t *hc);
static void h3_conn_close(h3_conn_t *hc);
static void h3_conn_free(h3_conn_t *hc);
static void h3_respond(http_stream_t *s);

/* ------------------------------------------------------------------------
 * Small helpers
 * ---------------------------------------------------------------------- */
static ngtcp2_tstamp ts_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (ngtcp2_tstamp)t.tv_sec * NGTCP2_SECONDS + (ngtcp2_tstamp)t.tv_nsec;
}

static void format_ip(char *dst, size_t cap, const struct sockaddr *sa, socklen_t len)
{
    if (getnameinfo(sa, len, dst, (socklen_t)cap, NULL, 0, NI_NUMERICHOST) != 0)
        snprintf(dst, cap, "?");
}

static void h3_sendto(h3_endpoint_t *ep, const ngtcp2_path *path, const uint8_t *data, size_t len)
{
    ssize_t n;
    do {
        n = sendto(ep->fd, data, len, 0, (const struct sockaddr *)path->remote.addr,
                   path->remote.addrlen);
    } while (n < 0 && errno == EINTR);
    /* EAGAIN means the socket buffer is full. Dropping the datagram is the
     * correct response: QUIC treats it like network loss and retransmits. */
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
        LOG_DEBUG("h3: sendto: %s", strerror(errno));
}

/* Only the first error is kept: it is the one that caused the close. */
static void set_liberr(h3_conn_t *hc, int liberr)
{
    if (hc->last_error.type == NGTCP2_CCERR_TYPE_TRANSPORT && hc->last_error.error_code == 0)
        ngtcp2_ccerr_set_liberr(&hc->last_error, liberr, NULL, 0);
}

static void set_app_err(h3_conn_t *hc, int h3_liberr)
{
    if (hc->last_error.type == NGTCP2_CCERR_TYPE_TRANSPORT && hc->last_error.error_code == 0)
        ngtcp2_ccerr_set_application_error(
            &hc->last_error, nghttp3_err_infer_quic_app_error_code(h3_liberr), NULL, 0);
}

/* ------------------------------------------------------------------------
 * Connection table: connection ID -> connection
 * ---------------------------------------------------------------------- */
static size_t cid_bucket(const uint8_t *data, size_t len)
{
    return hash_fnv1a(data, len) & (H3_CID_BUCKETS - 1);
}

static h3_conn_t *cid_lookup(h3_endpoint_t *ep, const uint8_t *data, size_t len)
{
    for (cid_entry_t *e = ep->buckets[cid_bucket(data, len)]; e; e = e->next)
        if (e->cid.datalen == len && memcmp(e->cid.data, data, len) == 0)
            return e->hc;
    return NULL;
}

static void cid_register(h3_conn_t *hc, const ngtcp2_cid *cid)
{
    cid_entry_t *e = xcalloc(1, sizeof(*e));
    e->cid = *cid;
    e->hc = hc;
    cid_entry_t **b = &hc->ep->buckets[cid_bucket(cid->data, cid->datalen)];
    e->next = *b;
    *b = e;
    e->conn_next = hc->cids;
    hc->cids = e;
}

static void cid_unregister(h3_conn_t *hc, const ngtcp2_cid *cid)
{
    cid_entry_t **b = &hc->ep->buckets[cid_bucket(cid->data, cid->datalen)];
    for (cid_entry_t **pp = b; *pp; pp = &(*pp)->next) {
        cid_entry_t *e = *pp;
        if (e->hc != hc || e->cid.datalen != cid->datalen ||
            memcmp(e->cid.data, cid->data, cid->datalen) != 0)
            continue;
        *pp = e->next;
        for (cid_entry_t **cp = &hc->cids; *cp; cp = &(*cp)->conn_next)
            if (*cp == e) {
                *cp = e->conn_next;
                break;
            }
        free(e);
        return;
    }
}

static void cid_unregister_all(h3_conn_t *hc)
{
    while (hc->cids)
        cid_unregister(hc, &hc->cids->cid);
}

/* ------------------------------------------------------------------------
 * Streams
 * ---------------------------------------------------------------------- */
static h3_stream_t *h3_stream_new(h3_conn_t *hc, int64_t id)
{
    h3_stream_t *hs = xcalloc(1, sizeof(*hs));
    hs->hc = hc;
    hs->id = id;
    hs->s = stream_new(hc->ep->w, h3_respond, hs);
    hs->s->stream_id = id;
    hs->s->req.version_major = 3;
    hs->s->req.version_minor = 0;
    hs->s->req.scheme = "https";
    snprintf(hs->s->req.client_ip, sizeof(hs->s->req.client_ip), "%s", hc->peer_ip);
    hs->next = hc->streams;
    hc->streams = hs;
    hc->nstreams++;
    return hs;
}

static void h3_stream_release(h3_stream_t *hs)
{
    h3_conn_t *hc = hs->hc;
    for (h3_stream_t **pp = &hc->streams; *pp; pp = &(*pp)->next)
        if (*pp == hs) {
            *pp = hs->next;
            break;
        }
    hc->nstreams--;
    if (hs->dispatched && !hs->responded)
        stream_abort(hs->s); /* cancels upstream work */
    else
        stream_free(hs->s);
    free(hs);
}

static void h3_dispatch(h3_stream_t *hs)
{
    http_request_t *req = &hs->s->req;
    hs->dispatched = true;
    if (hs->bad || req->method_len == 0 || !req->target) {
        stream_respond_error(hs->s, 400, "malformed request");
        return;
    }
    if (hs->too_large) {
        stream_respond_error(hs->s, 413, "request body too large");
        return;
    }
    server_dispatch(hs->hc->ep->srv, hs->s);
}

/* ------------------------------------------------------------------------
 * Response path
 * ---------------------------------------------------------------------- */
static nghttp3_ssize read_data_cb(nghttp3_conn *conn, int64_t stream_id, nghttp3_vec *vec,
                                  size_t veccnt, uint32_t *pflags, void *conn_user_data,
                                  void *stream_user_data)
{
    h3_stream_t *hs = stream_user_data;
    *pflags |= NGHTTP3_DATA_FLAG_EOF;
    if (hs->body_sent)
        return 0;
    hs->body_sent = true;
    /* The whole body in one vector; it stays valid until the stream closes,
     * which nghttp3 only reports once every byte has been acknowledged. */
    vec[0].base = (uint8_t *)buf_rptr(&hs->s->resp.body);
    vec[0].len = buf_rlen(&hs->s->resp.body);
    return 1;
}

static void h3_respond(http_stream_t *s)
{
    h3_stream_t *hs = s->frontend;
    h3_conn_t *hc = hs->hc;
    http_response_t *resp = &s->resp;
    bool head = http_request_method_is(&s->req, "HEAD");

    buf_t scratch;
    buf_init(&scratch);
    http_field_t *f;
    size_t n = http_response_fields_binary(resp, buf_rlen(&resp->body), &scratch, &f);
    nghttp3_nv *nva = xcalloc(n, sizeof(*nva));
    for (size_t i = 0; i < n; i++)
        nva[i] = (nghttp3_nv){(uint8_t *)f[i].name, (uint8_t *)f[i].val, f[i].nlen, f[i].vlen,
                              NGHTTP3_NV_FLAG_NONE};

    nghttp3_data_reader dr = {.read_data = read_data_cb};
    bool has_body = !head && buf_rlen(&resp->body) > 0;
    int rv = nghttp3_conn_submit_response(hc->h3, hs->id, nva, n, has_body ? &dr : NULL);
    free(nva);
    free(f);
    buf_free(&scratch);
    if (rv != 0) {
        LOG_WARN("h3: submit_response failed: %s", nghttp3_strerror(rv));
        ngtcp2_conn_shutdown_stream(hc->conn, 0, hs->id, NGHTTP3_H3_INTERNAL_ERROR);
    }
    hs->responded = true;
    h3_conn_flush(hc); /* no-op while inside an ngtcp2 callback */
}

/* ------------------------------------------------------------------------
 * nghttp3 callbacks (HTTP layer)
 * ---------------------------------------------------------------------- */
static int h3_begin_headers_cb(nghttp3_conn *conn, int64_t stream_id, void *conn_user_data,
                               void *stream_user_data)
{
    h3_conn_t *hc = conn_user_data;
    h3_stream_t *hs = h3_stream_new(hc, stream_id);
    nghttp3_conn_set_stream_user_data(conn, stream_id, hs);
    return 0;
}

static int h3_recv_header_cb(nghttp3_conn *conn, int64_t stream_id, int32_t token,
                             nghttp3_rcbuf *name, nghttp3_rcbuf *value, uint8_t flags,
                             void *conn_user_data, void *stream_user_data)
{
    h3_stream_t *hs = stream_user_data;
    if (!hs)
        return 0;
    http_request_t *req = &hs->s->req;
    nghttp3_vec nm = nghttp3_rcbuf_get_buf(name), vl = nghttp3_rcbuf_get_buf(value);
    const char *n = (const char *)nm.base, *v = (const char *)vl.base;

    if (nm.len > 0 && n[0] == ':') {
        if (str_ieq_lit(n, nm.len, ":method")) {
            if (vl.len >= HTTP_METHOD_MAX) {
                hs->bad = true;
                return 0;
            }
            memcpy(req->method, v, vl.len);
            req->method_len = vl.len;
        } else if (str_ieq_lit(n, nm.len, ":path")) {
            http_request_set_target(req, v, vl.len);
        } else if (str_ieq_lit(n, nm.len, ":authority")) {
            /* Upstreams speak HTTP/1.1, which wants Host. */
            headers_remove(&req->headers, "host");
            headers_add(&req->headers, "Host", 4, v, vl.len);
        } else if (str_ieq_lit(n, nm.len, ":scheme")) {
            req->scheme = vl.len == 5 && memcmp(v, "https", 5) == 0 ? "https" : "http";
        }
        return 0;
    }
    const char *hv;
    size_t hvl;
    if (str_ieq_lit(n, nm.len, "host") && headers_get(&req->headers, "Host", &hv, &hvl))
        return 0; /* :authority already produced Host */
    /* nghttp3 enforces max_field_section_size itself. */
    headers_add(&req->headers, n, nm.len, v, vl.len);
    return 0;
}

static int h3_recv_data_cb(nghttp3_conn *conn, int64_t stream_id, const uint8_t *data,
                           size_t datalen, void *conn_user_data, void *stream_user_data)
{
    h3_conn_t *hc = conn_user_data;
    h3_stream_t *hs = stream_user_data;
    /* Body bytes are not counted in nghttp3's return value; consume them
     * here so the client's flow-control window keeps opening. */
    ngtcp2_conn_extend_max_stream_offset(hc->conn, stream_id, datalen);
    ngtcp2_conn_extend_max_offset(hc->conn, datalen);
    if (!hs || hs->too_large)
        return 0;
    if (buf_rlen(&hs->s->req.body) + datalen > hc->ep->srv->cfg->max_body_size) {
        /* Answer 413 right away; the rest of the upload is discarded. */
        hs->too_large = true;
        buf_reset(&hs->s->req.body);
        if (!hs->dispatched)
            h3_dispatch(hs);
        return 0;
    }
    buf_append(&hs->s->req.body, data, datalen);
    return 0;
}

static int h3_deferred_consume_cb(nghttp3_conn *conn, int64_t stream_id, size_t consumed,
                                  void *conn_user_data, void *stream_user_data)
{
    h3_conn_t *hc = conn_user_data;
    ngtcp2_conn_extend_max_stream_offset(hc->conn, stream_id, consumed);
    ngtcp2_conn_extend_max_offset(hc->conn, consumed);
    return 0;
}

static int h3_end_stream_cb(nghttp3_conn *conn, int64_t stream_id, void *conn_user_data,
                            void *stream_user_data)
{
    h3_stream_t *hs = stream_user_data;
    if (hs && !hs->dispatched)
        h3_dispatch(hs);
    return 0;
}

static int h3_stream_close_cb(nghttp3_conn *conn, int64_t stream_id, uint64_t app_error_code,
                              void *conn_user_data, void *stream_user_data)
{
    h3_stream_t *hs = stream_user_data;
    if (hs)
        h3_stream_release(hs);
    return 0;
}

static int h3_stop_sending_cb(nghttp3_conn *conn, int64_t stream_id, uint64_t app_error_code,
                              void *conn_user_data, void *stream_user_data)
{
    h3_conn_t *hc = conn_user_data;
    if (ngtcp2_conn_shutdown_stream_read(hc->conn, 0, stream_id, app_error_code) != 0)
        return NGHTTP3_ERR_CALLBACK_FAILURE;
    return 0;
}

static int h3_reset_stream_cb(nghttp3_conn *conn, int64_t stream_id, uint64_t app_error_code,
                              void *conn_user_data, void *stream_user_data)
{
    h3_conn_t *hc = conn_user_data;
    if (ngtcp2_conn_shutdown_stream_write(hc->conn, 0, stream_id, app_error_code) != 0)
        return NGHTTP3_ERR_CALLBACK_FAILURE;
    return 0;
}

/* Called once 1-RTT keys exist: create the HTTP/3 session and open the
 * three unidirectional streams it needs (control + QPACK encoder/decoder). */
static int h3_setup_http(h3_conn_t *hc)
{
    const config_t *cfg = hc->ep->srv->cfg;
    nghttp3_callbacks cbs = {
        .begin_headers = h3_begin_headers_cb,
        .recv_header = h3_recv_header_cb,
        .recv_data = h3_recv_data_cb,
        .deferred_consume = h3_deferred_consume_cb,
        .end_stream = h3_end_stream_cb,
        .stream_close = h3_stream_close_cb,
        .stop_sending = h3_stop_sending_cb,
        .reset_stream = h3_reset_stream_cb,
    };
    nghttp3_settings st;
    nghttp3_settings_default(&st);
    st.max_field_section_size = cfg->max_header_size;
    st.qpack_max_dtable_capacity = 4096;
    st.qpack_blocked_streams = 100;

    int rv = nghttp3_conn_server_new(&hc->h3, &cbs, &st, NULL, hc);
    if (rv != 0) {
        LOG_ERROR("nghttp3_conn_server_new: %s", nghttp3_strerror(rv));
        return -1;
    }
    const ngtcp2_transport_params *tp = ngtcp2_conn_get_local_transport_params(hc->conn);
    nghttp3_conn_set_max_client_streams_bidi(hc->h3, tp->initial_max_streams_bidi);

    int64_t ctrl, qenc, qdec;
    if (ngtcp2_conn_open_uni_stream(hc->conn, &ctrl, NULL) != 0 ||
        ngtcp2_conn_open_uni_stream(hc->conn, &qenc, NULL) != 0 ||
        ngtcp2_conn_open_uni_stream(hc->conn, &qdec, NULL) != 0)
        return -1;
    if (nghttp3_conn_bind_control_stream(hc->h3, ctrl) != 0 ||
        nghttp3_conn_bind_qpack_streams(hc->h3, qenc, qdec) != 0)
        return -1;
    return 0;
}

/* ------------------------------------------------------------------------
 * ngtcp2 callbacks (transport layer)
 * ---------------------------------------------------------------------- */
static ngtcp2_conn *get_conn_cb(ngtcp2_crypto_conn_ref *ref)
{
    return ((h3_conn_t *)ref->user_data)->conn;
}

static void rand_cb(uint8_t *dest, size_t destlen, const ngtcp2_rand_ctx *rand_ctx)
{
    RAND_bytes(dest, (int)destlen);
}

static int get_new_connection_id_cb(ngtcp2_conn *conn, ngtcp2_cid *cid, uint8_t *token,
                                    size_t cidlen, void *user_data)
{
    h3_conn_t *hc = user_data;
    if (RAND_bytes(cid->data, (int)cidlen) != 1)
        return NGTCP2_ERR_CALLBACK_FAILURE;
    cid->datalen = cidlen;
    if (ngtcp2_crypto_generate_stateless_reset_token(token, hc->ep->secret, H3_SECRET_LEN, cid) !=
        0)
        return NGTCP2_ERR_CALLBACK_FAILURE;
    cid_register(hc, cid);
    return 0;
}

static int remove_connection_id_cb(ngtcp2_conn *conn, const ngtcp2_cid *cid, void *user_data)
{
    cid_unregister(user_data, cid);
    return 0;
}

static int recv_tx_key_cb(ngtcp2_conn *conn, ngtcp2_encryption_level level, void *user_data)
{
    h3_conn_t *hc = user_data;
    if (level != NGTCP2_ENCRYPTION_LEVEL_1RTT || hc->h3)
        return 0;
    return h3_setup_http(hc) == 0 ? 0 : NGTCP2_ERR_CALLBACK_FAILURE;
}

static int handshake_completed_cb(ngtcp2_conn *conn, void *user_data)
{
    h3_conn_t *hc = user_data;
    const unsigned char *alpn = NULL;
    unsigned int alpnlen = 0;
    SSL_get0_alpn_selected(hc->ssl, &alpn, &alpnlen);
    if (alpnlen != 2 || memcmp(alpn, "h3", 2) != 0) {
        LOG_DEBUG("%s: QUIC client did not negotiate h3", hc->peer_ip);
        /* TLS alert no_application_protocol (120), as a QUIC crypto error. */
        ngtcp2_ccerr_set_transport_error(&hc->last_error, NGTCP2_CRYPTO_ERROR | 120, NULL, 0);
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    LOG_DEBUG("%s: QUIC handshake complete (%s)", hc->peer_ip, SSL_get_cipher_name(hc->ssl));
    return 0;
}

static int recv_stream_data_cb(ngtcp2_conn *conn, uint32_t flags, int64_t stream_id,
                               uint64_t offset, const uint8_t *data, size_t datalen,
                               void *user_data, void *stream_user_data)
{
    h3_conn_t *hc = user_data;
    if (!hc->h3)
        return 0;
    nghttp3_ssize n = nghttp3_conn_read_stream(hc->h3, stream_id, data, datalen,
                                               (flags & NGTCP2_STREAM_DATA_FLAG_FIN) != 0);
    if (n < 0) {
        LOG_DEBUG("%s: h3: %s", hc->peer_ip, nghttp3_strerror((int)n));
        set_app_err(hc, (int)n);
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    ngtcp2_conn_extend_max_stream_offset(conn, stream_id, (uint64_t)n);
    ngtcp2_conn_extend_max_offset(conn, (uint64_t)n);
    return 0;
}

static int acked_stream_data_offset_cb(ngtcp2_conn *conn, int64_t stream_id, uint64_t offset,
                                       uint64_t datalen, void *user_data,
                                       void *stream_user_data)
{
    h3_conn_t *hc = user_data;
    if (!hc->h3)
        return 0;
    int rv = nghttp3_conn_add_ack_offset(hc->h3, stream_id, datalen);
    if (rv != 0 && rv != NGHTTP3_ERR_STREAM_NOT_FOUND) {
        set_app_err(hc, rv);
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    return 0;
}

static int stream_close_cb(ngtcp2_conn *conn, uint32_t flags, int64_t stream_id,
                           uint64_t app_error_code, void *user_data, void *stream_user_data)
{
    h3_conn_t *hc = user_data;
    if (!hc->h3)
        return 0;
    if (!(flags & NGTCP2_STREAM_CLOSE_FLAG_APP_ERROR_CODE_SET))
        app_error_code = NGHTTP3_H3_NO_ERROR;
    int rv = nghttp3_conn_close_stream(hc->h3, stream_id, app_error_code);
    if (rv != 0 && rv != NGHTTP3_ERR_STREAM_NOT_FOUND) {
        set_app_err(hc, rv);
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    return 0;
}

static int stream_reset_cb(ngtcp2_conn *conn, int64_t stream_id, uint64_t final_size,
                           uint64_t app_error_code, void *user_data, void *stream_user_data)
{
    h3_conn_t *hc = user_data;
    if (!hc->h3)
        return 0;
    int rv = nghttp3_conn_shutdown_stream_read(hc->h3, stream_id);
    if (rv != 0 && rv != NGHTTP3_ERR_STREAM_NOT_FOUND) {
        set_app_err(hc, rv);
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    return 0;
}

static int stream_stop_sending_cb(ngtcp2_conn *conn, int64_t stream_id, uint64_t app_error_code,
                                  void *user_data, void *stream_user_data)
{
    return stream_reset_cb(conn, stream_id, 0, app_error_code, user_data, stream_user_data);
}

static int extend_max_remote_streams_bidi_cb(ngtcp2_conn *conn, uint64_t max_streams,
                                             void *user_data)
{
    h3_conn_t *hc = user_data;
    if (hc->h3)
        nghttp3_conn_set_max_client_streams_bidi(hc->h3, max_streams);
    return 0;
}

static int extend_max_stream_data_cb(ngtcp2_conn *conn, int64_t stream_id, uint64_t max_data,
                                     void *user_data, void *stream_user_data)
{
    h3_conn_t *hc = user_data;
    if (!hc->h3)
        return 0;
    int rv = nghttp3_conn_unblock_stream(hc->h3, stream_id);
    if (rv != 0 && rv != NGHTTP3_ERR_STREAM_NOT_FOUND) {
        set_app_err(hc, rv);
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    return 0;
}

static const ngtcp2_callbacks h3_ngtcp2_callbacks = {
    /* Crypto plumbing supplied by libngtcp2_crypto_ossl. */
    .recv_client_initial = ngtcp2_crypto_recv_client_initial_cb,
    .recv_crypto_data = ngtcp2_crypto_recv_crypto_data_cb,
    .encrypt = ngtcp2_crypto_encrypt_cb,
    .decrypt = ngtcp2_crypto_decrypt_cb,
    .hp_mask = ngtcp2_crypto_hp_mask_cb,
    .update_key = ngtcp2_crypto_update_key_cb,
    .delete_crypto_aead_ctx = ngtcp2_crypto_delete_crypto_aead_ctx_cb,
    .delete_crypto_cipher_ctx = ngtcp2_crypto_delete_crypto_cipher_ctx_cb,
    .get_path_challenge_data = ngtcp2_crypto_get_path_challenge_data_cb,
    .version_negotiation = ngtcp2_crypto_version_negotiation_cb,
    /* Ours. */
    .handshake_completed = handshake_completed_cb,
    .recv_stream_data = recv_stream_data_cb,
    .acked_stream_data_offset = acked_stream_data_offset_cb,
    .stream_close = stream_close_cb,
    .stream_reset = stream_reset_cb,
    .stream_stop_sending = stream_stop_sending_cb,
    .extend_max_remote_streams_bidi = extend_max_remote_streams_bidi_cb,
    .extend_max_stream_data = extend_max_stream_data_cb,
    .rand = rand_cb,
    .get_new_connection_id = get_new_connection_id_cb,
    .remove_connection_id = remove_connection_id_cb,
    .recv_tx_key = recv_tx_key_cb,
};

/* ------------------------------------------------------------------------
 * Connection lifecycle
 * ---------------------------------------------------------------------- */
static void h3_on_timer(timer_t_ *t);

static h3_conn_t *h3_conn_new(h3_endpoint_t *ep, const ngtcp2_pkt_hd *hd,
                              const struct sockaddr *peer, socklen_t peerlen)
{
    const config_t *cfg = ep->srv->cfg;
    h3_conn_t *hc = xcalloc(1, sizeof(*hc));
    hc->ep = ep;
    timer_init(&hc->timer, h3_on_timer, hc);
    ngtcp2_ccerr_default(&hc->last_error);
    format_ip(hc->peer_ip, sizeof(hc->peer_ip), peer, peerlen);
    ngtcp2_path_storage_init(&hc->path, (ngtcp2_sockaddr *)&ep->local, ep->local_len,
                             (ngtcp2_sockaddr *)peer, peerlen, NULL);

    ngtcp2_settings st;
    ngtcp2_settings_default(&st);
    st.initial_ts = ts_now();
    st.handshake_timeout = cfg->header_timeout_ms * NGTCP2_MILLISECONDS;
    st.max_window = 6 << 20; /* auto-tuned connection / stream windows */
    st.max_stream_window = 4 << 20;

    ngtcp2_transport_params tp;
    ngtcp2_transport_params_default(&tp);
    tp.initial_max_stream_data_bidi_local = 256 << 10;
    tp.initial_max_stream_data_bidi_remote = 256 << 10;
    tp.initial_max_stream_data_uni = 256 << 10;
    tp.initial_max_data = 1 << 20;
    tp.initial_max_streams_bidi = H3_MAX_STREAMS_BIDI;
    tp.initial_max_streams_uni = 3;
    tp.max_idle_timeout = cfg->idle_timeout_ms * NGTCP2_MILLISECONDS;
    tp.active_connection_id_limit = 7;
    tp.original_dcid = hd->dcid;
    tp.original_dcid_present = 1;

    ngtcp2_cid scid;
    RAND_bytes(scid.data, H3_SCIDLEN);
    scid.datalen = H3_SCIDLEN;
    ngtcp2_crypto_generate_stateless_reset_token(tp.stateless_reset_token, ep->secret,
                                                 H3_SECRET_LEN, &scid);
    tp.stateless_reset_token_present = 1;

    int rv = ngtcp2_conn_server_new(&hc->conn, &hd->scid, &scid, &hc->path.path, hd->version,
                                    &h3_ngtcp2_callbacks, &st, &tp, NULL, hc);
    if (rv != 0) {
        LOG_WARN("ngtcp2_conn_server_new: %s", ngtcp2_strerror(rv));
        free(hc);
        return NULL;
    }

    /* TLS 1.3 through OpenSSL's QUIC TLS API; ngtcp2 drives the handshake. */
    hc->ssl = SSL_new(ep->ssl_ctx);
    hc->conn_ref.get_conn = get_conn_cb;
    hc->conn_ref.user_data = hc;
    SSL_set_app_data(hc->ssl, &hc->conn_ref);
    SSL_set_accept_state(hc->ssl);
    tls_ssl_set_alpn(hc->ssl, alpn_h3, sizeof(alpn_h3) - 1);
    if (ngtcp2_crypto_ossl_ctx_new(&hc->ossl, hc->ssl) != 0 ||
        ngtcp2_crypto_ossl_configure_server_session(hc->ssl) != 0) {
        LOG_WARN("h3: cannot configure TLS for QUIC");
        ngtcp2_conn_del(hc->conn);
        SSL_set_app_data(hc->ssl, NULL);
        SSL_free(hc->ssl);
        ngtcp2_crypto_ossl_ctx_del(hc->ossl);
        free(hc);
        return NULL;
    }
    ngtcp2_conn_set_tls_native_handle(hc->conn, hc->ossl);

    /* Route both the client's original DCID (used by its Initial packets)
     * and the SCID we chose to this connection. */
    cid_register(hc, &hd->dcid);
    cid_register(hc, &scid);

    hc->next = ep->conns;
    if (ep->conns)
        ep->conns->prev = hc;
    ep->conns = hc;
    ep->nconns++;
    atomic_fetch_add_explicit(&ep->srv->stats.total_conns, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&ep->srv->stats.active_conns, 1, memory_order_relaxed);
    LOG_DEBUG("%s: new QUIC connection", hc->peer_ip);
    return hc;
}

static void h3_conn_free(h3_conn_t *hc)
{
    h3_endpoint_t *ep = hc->ep;
    timer_cancel(ev_timers(ep->w->loop), &hc->timer);
    while (hc->streams)
        h3_stream_release(hc->streams);
    cid_unregister_all(hc);

    if (hc->prev)
        hc->prev->next = hc->next;
    else
        ep->conns = hc->next;
    if (hc->next)
        hc->next->prev = hc->prev;
    ep->nconns--;
    atomic_fetch_sub_explicit(&ep->srv->stats.active_conns, 1, memory_order_relaxed);

    nghttp3_conn_del(hc->h3);
    ngtcp2_conn_del(hc->conn);
    if (hc->ssl) {
        SSL_set_app_data(hc->ssl, NULL);
        SSL_free(hc->ssl);
    }
    ngtcp2_crypto_ossl_ctx_del(hc->ossl);
    free(hc->close_pkt);
    free(hc);
}

static void h3_conn_arm_timer(h3_conn_t *hc)
{
    ngtcp2_tstamp expiry = ngtcp2_conn_get_expiry(hc->conn);
    if (expiry == UINT64_MAX) {
        timer_cancel(ev_timers(hc->ep->w->loop), &hc->timer);
        return;
    }
    ngtcp2_tstamp now = ts_now();
    uint64_t delay_ms = expiry <= now ? 0 : (expiry - now + NGTCP2_MILLISECONDS - 1) /
                                               NGTCP2_MILLISECONDS;
    ev_timer_after(hc->ep->w->loop, &hc->timer, delay_ms);
}

/* Build and send CONNECTION_CLOSE for hc->last_error. Returns false if
 * nothing could be written (e.g. no keys yet); the caller then just drops
 * the connection. */
static bool h3_conn_send_close(h3_conn_t *hc)
{
    if (!hc->close_pkt)
        hc->close_pkt = xmalloc(NGTCP2_MAX_PMTUD_UDP_PAYLOAD_SIZE);
    ngtcp2_path_storage ps;
    ngtcp2_path_storage_zero(&ps);
    ngtcp2_pkt_info pi;
    ngtcp2_ssize n = ngtcp2_conn_write_connection_close(hc->conn, &ps.path, &pi, hc->close_pkt,
                                                        NGTCP2_MAX_PMTUD_UDP_PAYLOAD_SIZE,
                                                        &hc->last_error, ts_now());
    if (n <= 0)
        return false;
    hc->close_pkt_len = (size_t)n;
    h3_sendto(hc->ep, &ps.path, hc->close_pkt, hc->close_pkt_len);
    return true;
}

/* Enter the closing period: streams are aborted now, the connection object
 * lingers for 3 PTO so the peer's late packets get the CONNECTION_CLOSE
 * again instead of a stateless reset. */
static void h3_conn_close(h3_conn_t *hc)
{
    if (hc->closing || hc->draining)
        return;
    while (hc->streams)
        h3_stream_release(hc->streams);
    if (!h3_conn_send_close(hc)) {
        h3_conn_free(hc);
        return;
    }
    hc->closing = true;
    uint64_t pto_ms = ngtcp2_conn_get_pto(hc->conn) / NGTCP2_MILLISECONDS;
    ev_timer_after(hc->ep->w->loop, &hc->timer, 3 * pto_ms + 1);
}

static void h3_conn_start_draining(h3_conn_t *hc)
{
    hc->draining = true;
    while (hc->streams)
        h3_stream_release(hc->streams);
    uint64_t pto_ms = ngtcp2_conn_get_pto(hc->conn) / NGTCP2_MILLISECONDS;
    ev_timer_after(hc->ep->w->loop, &hc->timer, 3 * pto_ms + 1);
}

/* Pull stream data from nghttp3, let ngtcp2 pack and protect it, send. */
static void h3_conn_flush(h3_conn_t *hc)
{
    if (hc->in_callback || hc->closing || hc->draining)
        return;
    h3_endpoint_t *ep = hc->ep;
    ngtcp2_tstamp ts = ts_now();
    ngtcp2_path_storage ps;
    ngtcp2_path_storage_zero(&ps);
    ngtcp2_pkt_info pi;
    size_t max_pkt = ngtcp2_conn_get_max_tx_udp_payload_size(hc->conn);

    for (int sent = 0; sent < H3_FLUSH_MAX_PKTS;) {
        int64_t stream_id = -1;
        int fin = 0;
        nghttp3_vec vec[16];
        nghttp3_ssize veccnt = 0;
        if (hc->h3) {
            veccnt = nghttp3_conn_writev_stream(hc->h3, &stream_id, &fin, vec, ARRAY_LEN(vec));
            if (veccnt < 0) {
                LOG_DEBUG("%s: h3 write: %s", hc->peer_ip, nghttp3_strerror((int)veccnt));
                set_app_err(hc, (int)veccnt);
                h3_conn_close(hc);
                return;
            }
        }
        uint32_t flags = NGTCP2_WRITE_STREAM_FLAG_MORE | (fin ? NGTCP2_WRITE_STREAM_FLAG_FIN : 0);
        ngtcp2_ssize ndatalen = -1;
        ngtcp2_ssize n = ngtcp2_conn_writev_stream(hc->conn, &ps.path, &pi, ep->wbuf, max_pkt,
                                                   &ndatalen, flags, stream_id,
                                                   (const ngtcp2_vec *)vec, (size_t)veccnt, ts);
        if (n < 0) {
            switch (n) {
            case NGTCP2_ERR_STREAM_DATA_BLOCKED:
                nghttp3_conn_block_stream(hc->h3, stream_id);
                continue;
            case NGTCP2_ERR_STREAM_SHUT_WR:
                nghttp3_conn_shutdown_stream_write(hc->h3, stream_id);
                continue;
            case NGTCP2_ERR_WRITE_MORE:
                /* Packet not full yet: record what was packed, add more. */
                nghttp3_conn_add_write_offset(hc->h3, stream_id, (size_t)ndatalen);
                continue;
            default:
                LOG_DEBUG("%s: quic write: %s", hc->peer_ip, ngtcp2_strerror((int)n));
                set_liberr(hc, (int)n);
                h3_conn_close(hc);
                return;
            }
        }
        if (ndatalen > 0)
            nghttp3_conn_add_write_offset(hc->h3, stream_id, (size_t)ndatalen);
        if (n == 0)
            break; /* nothing more to send, or congestion / pacing limited */
        h3_sendto(ep, &ps.path, ep->wbuf, (size_t)n);
        sent++;
    }
    ngtcp2_conn_update_pkt_tx_time(hc->conn, ts);
    h3_conn_arm_timer(hc);
}

static void h3_on_timer(timer_t_ *t)
{
    h3_conn_t *hc = t->arg;
    if (hc->closing || hc->draining) {
        LOG_DEBUG("%s: QUIC connection closed", hc->peer_ip);
        h3_conn_free(hc);
        return;
    }
    hc->in_callback = true;
    int rv = ngtcp2_conn_handle_expiry(hc->conn, ts_now());
    hc->in_callback = false;
    if (rv != 0) {
        if (rv == NGTCP2_ERR_IDLE_CLOSE) {
            LOG_DEBUG("%s: QUIC idle timeout", hc->peer_ip);
            h3_conn_free(hc); /* silently, per RFC 9000 §10.1 */
            return;
        }
        LOG_DEBUG("%s: quic expiry: %s", hc->peer_ip, ngtcp2_strerror(rv));
        set_liberr(hc, rv);
        h3_conn_close(hc);
        return;
    }
    h3_conn_flush(hc);
}

/* ------------------------------------------------------------------------
 * Datagram input
 * ---------------------------------------------------------------------- */
static void h3_send_version_negotiation(h3_endpoint_t *ep, const ngtcp2_version_cid *vc,
                                        const struct sockaddr *peer, socklen_t peerlen)
{
    uint8_t buf[256], rnd;
    RAND_bytes(&rnd, 1);
    uint32_t versions[] = {NGTCP2_PROTO_VER_V1};
    /* Our DCID is the client's SCID and vice versa. */
    ngtcp2_ssize n = ngtcp2_pkt_write_version_negotiation(buf, sizeof(buf), rnd, vc->scid,
                                                          vc->scidlen, vc->dcid, vc->dcidlen,
                                                          versions, ARRAY_LEN(versions));
    if (n > 0)
        sendto(ep->fd, buf, (size_t)n, 0, peer, peerlen);
}

static void h3_handle_datagram(h3_endpoint_t *ep, const uint8_t *data, size_t len,
                               const struct sockaddr *peer, socklen_t peerlen)
{
    ngtcp2_version_cid vc;
    int rv = ngtcp2_pkt_decode_version_cid(&vc, data, len, H3_SCIDLEN);
    if (rv == NGTCP2_ERR_VERSION_NEGOTIATION) {
        h3_send_version_negotiation(ep, &vc, peer, peerlen);
        return;
    }
    if (rv != 0)
        return; /* not QUIC */

    h3_conn_t *hc = cid_lookup(ep, vc.dcid, vc.dcidlen);
    if (!hc) {
        ngtcp2_pkt_hd hd;
        if (ngtcp2_accept(&hd, data, len) != 0)
            return; /* stray packet for a connection we no longer know */
        if (ep->w->draining)
            return;
        hc = h3_conn_new(ep, &hd, peer, peerlen);
        if (!hc)
            return;
    }
    if (hc->closing) {
        /* Peer has not seen our CONNECTION_CLOSE yet: repeat it. */
        ngtcp2_path path;
        ngtcp2_addr_init(&path.local, (ngtcp2_sockaddr *)&ep->local, ep->local_len);
        ngtcp2_addr_init(&path.remote, (ngtcp2_sockaddr *)peer, peerlen);
        h3_sendto(ep, &path, hc->close_pkt, hc->close_pkt_len);
        return;
    }
    if (hc->draining)
        return;

    ngtcp2_path path;
    ngtcp2_addr_init(&path.local, (ngtcp2_sockaddr *)&ep->local, ep->local_len);
    ngtcp2_addr_init(&path.remote, (ngtcp2_sockaddr *)peer, peerlen);
    ngtcp2_pkt_info pi = {0};

    hc->in_callback = true;
    rv = ngtcp2_conn_read_pkt(hc->conn, &path, &pi, data, len, ts_now());
    hc->in_callback = false;
    if (rv != 0) {
        switch (rv) {
        case NGTCP2_ERR_DRAINING:
            LOG_DEBUG("%s: peer closed QUIC connection", hc->peer_ip);
            h3_conn_start_draining(hc);
            return;
        case NGTCP2_ERR_RETRY: /* we never ask for address validation */
        case NGTCP2_ERR_DROP_CONN:
            h3_conn_free(hc);
            return;
        case NGTCP2_ERR_CRYPTO:
            if (hc->last_error.error_code == 0)
                ngtcp2_ccerr_set_transport_error(
                    &hc->last_error, NGTCP2_CRYPTO_ERROR | ngtcp2_conn_get_tls_alert(hc->conn),
                    NULL, 0);
            break;
        default:
            set_liberr(hc, rv);
            break;
        }
        LOG_DEBUG("%s: quic read: %s", hc->peer_ip, ngtcp2_strerror(rv));
        h3_conn_close(hc);
        return;
    }
    h3_conn_flush(hc);
}

static void h3_on_readable(event_loop_t *loop, int fd, int events, void *arg)
{
    h3_endpoint_t *ep = arg;
    for (int i = 0; i < H3_RECV_BATCH; i++) {
        struct sockaddr_storage peer;
        socklen_t peerlen = sizeof(peer);
        ssize_t n = recvfrom(fd, ep->rbuf, H3_BUF_SIZE, 0, (struct sockaddr *)&peer, &peerlen);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                LOG_DEBUG("h3: recvfrom: %s", strerror(errno));
            return;
        }
        if (n == 0)
            continue;
        atomic_fetch_add_explicit(&ep->srv->stats.bytes_in, (uint64_t)n, memory_order_relaxed);
        h3_handle_datagram(ep, ep->rbuf, (size_t)n, (struct sockaddr *)&peer, peerlen);
    }
}

/* ------------------------------------------------------------------------
 * Endpoint
 * ---------------------------------------------------------------------- */
h3_endpoint_t *h3_endpoint_start(server_t *srv, worker_t *w)
{
    static bool crypto_ready;
    if (!crypto_ready) {
        ngtcp2_crypto_ossl_init();
        crypto_ready = true;
    }
    const config_t *cfg = srv->cfg;
    h3_endpoint_t *ep = xcalloc(1, sizeof(*ep));
    ep->srv = srv;
    ep->w = w;
    ep->fd = -1;
    RAND_bytes(ep->secret, sizeof(ep->secret));

    ep->fd = udp_socket_open(cfg->http3_addr, cfg->http3_port);
    if (ep->fd < 0)
        goto fail;
    ep->local_len = sizeof(ep->local);
    getsockname(ep->fd, (struct sockaddr *)&ep->local, &ep->local_len);

    /* Same certificates and SNI as the TCP listeners; QUIC mandates TLS 1.3. */
    ep->ssl_ctx = SSL_CTX_new(TLS_server_method());
    if (!ep->ssl_ctx ||
        tls_ctx_configure(srv->tls, ep->ssl_ctx, alpn_h3, sizeof(alpn_h3) - 1) < 0)
        goto fail;
    SSL_CTX_set_min_proto_version(ep->ssl_ctx, TLS1_3_VERSION);

    ep->rbuf = xmalloc(H3_BUF_SIZE);
    ep->wbuf = xmalloc(H3_BUF_SIZE);
    if (ev_add(w->loop, ep->fd, EV_READ, h3_on_readable, ep) < 0) {
        LOG_ERROR("h3: ev_add: %s", strerror(errno));
        goto fail;
    }
    LOG_INFO("listening on %s:%u (udp, http/3)", cfg->http3_addr, cfg->http3_port);
    return ep;

fail:
    if (ep->ssl_ctx) {
        free(SSL_CTX_get_ex_data(ep->ssl_ctx, 1)); /* ALPN pref from tls_ctx_configure */
        SSL_CTX_free(ep->ssl_ctx);
    }
    if (ep->fd >= 0)
        close(ep->fd);
    free(ep->rbuf);
    free(ep->wbuf);
    free(ep);
    return NULL;
}

void h3_endpoint_stop(h3_endpoint_t *ep)
{
    if (!ep)
        return;
    while (ep->conns) {
        h3_conn_t *hc = ep->conns;
        if (!hc->closing && !hc->draining) {
            ngtcp2_ccerr_set_application_error(&hc->last_error, NGHTTP3_H3_NO_ERROR, NULL, 0);
            h3_conn_send_close(hc); /* best effort, no closing period */
        }
        h3_conn_free(hc);
    }
    ev_del(ep->w->loop, ep->fd);
    close(ep->fd);
    free(SSL_CTX_get_ex_data(ep->ssl_ctx, 1));
    SSL_CTX_free(ep->ssl_ctx);
    free(ep->rbuf);
    free(ep->wbuf);
    free(ep);
}
