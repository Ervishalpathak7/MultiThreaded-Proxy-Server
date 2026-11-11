/*
 * request.h - protocol-neutral HTTP request / response model.
 *
 * HTTP/1.1, HTTP/2 and HTTP/3 frontends all produce the same
 * `http_request_t` and consume the same `http_response_t`, so the router,
 * cache, load balancer and upstream client are written once.
 *
 * Headers are stored in a small arena: one growable byte buffer holds all
 * names and values back to back, and a parallel array of offsets indexes
 * into it. That is two allocations per message instead of two per header,
 * and the whole thing is freed in O(1).
 */
#ifndef APROXY_HTTP_REQUEST_H
#define APROXY_HTTP_REQUEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/buffer.h"

/* ------------------------------------------------------------------------
 * Header list
 * ---------------------------------------------------------------------- */
typedef struct {
    uint32_t name_off, name_len;
    uint32_t val_off, val_len;
} hdr_ref_t;

typedef struct http_headers {
    buf_t arena;
    hdr_ref_t *items;
    size_t n, cap;
} http_headers_t;

void headers_init(http_headers_t *h);
void headers_free(http_headers_t *h);
void headers_reset(http_headers_t *h);

void headers_add(http_headers_t *h, const char *name, size_t nlen, const char *val, size_t vlen);
void headers_add_str(http_headers_t *h, const char *name, const char *val);
/* Replace every existing header `name` with a single new value. */
void headers_set_str(http_headers_t *h, const char *name, const char *val);

static inline size_t headers_count(const http_headers_t *h) { return h->n; }
const char *headers_name(const http_headers_t *h, size_t i, size_t *len);
const char *headers_value(const http_headers_t *h, size_t i, size_t *len);

/* First header with a matching (case-insensitive) name. */
bool headers_get(const http_headers_t *h, const char *name, const char **val, size_t *vlen);
/* Remove every header with the given name; returns how many were removed. */
size_t headers_remove(http_headers_t *h, const char *name);
/* True if the comma separated header value contains `token`
 * (e.g. Connection: keep-alive, Upgrade). */
bool header_value_has_token(const char *val, size_t vlen, const char *token);

/* ------------------------------------------------------------------------
 * Request
 * ---------------------------------------------------------------------- */
#define HTTP_METHOD_MAX 16

typedef struct http_request {
    char method[HTTP_METHOD_MAX];
    size_t method_len;
    char *target; /* request target as sent: "/path?query" */
    size_t target_len;
    int version_major, version_minor; /* 1.0, 1.1, 2.0, 3.0 */
    http_headers_t headers;
    buf_t body;
    bool keep_alive; /* HTTP/1 connection semantics */

    /* Filled in by the frontend for logging / forwarding headers. */
    char client_ip[64];
    const char *scheme; /* "http" or "https" */
} http_request_t;

void http_request_init(http_request_t *r);
void http_request_free(http_request_t *r);
void http_request_reset(http_request_t *r);
void http_request_set_target(http_request_t *r, const char *target, size_t len);
bool http_request_method_is(const http_request_t *r, const char *method);
/* Length of the path portion of the target (up to '?' or end). */
size_t http_request_path_len(const http_request_t *r);

/* ------------------------------------------------------------------------
 * Response
 * ---------------------------------------------------------------------- */
typedef struct http_response {
    int status;
    http_headers_t headers;
    buf_t body;
    bool keep_alive; /* set by the HTTP/1 parser for upstream pooling */
} http_response_t;

void http_response_init(http_response_t *r);
void http_response_free(http_response_t *r);
void http_response_reset(http_response_t *r);

/* Fill `r` with a simple text/plain error page for `status`. */
void http_response_set_error(http_response_t *r, int status, const char *message);

const char *http_status_reason(int status);

/* Hop-by-hop headers must not be forwarded by a proxy (RFC 9110 §7.6.1). */
bool http_header_is_hop_by_hop(const char *name, size_t len);

#endif /* APROXY_HTTP_REQUEST_H */
