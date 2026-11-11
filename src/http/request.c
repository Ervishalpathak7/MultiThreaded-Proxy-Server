/*
 * request.c - header arena and request/response helpers (see request.h).
 */
#include "http/request.h"

#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "core/util.h"

/* ------------------------------------------------------------------------
 * Headers
 * ---------------------------------------------------------------------- */
void headers_init(http_headers_t *h)
{
    buf_init(&h->arena);
    h->items = NULL;
    h->n = h->cap = 0;
}

void headers_free(http_headers_t *h)
{
    buf_free(&h->arena);
    free(h->items);
    headers_init(h);
}

void headers_reset(http_headers_t *h)
{
    buf_reset(&h->arena);
    h->n = 0;
}

void headers_add(http_headers_t *h, const char *name, size_t nlen, const char *val, size_t vlen)
{
    if (h->n == h->cap) {
        h->cap = h->cap ? h->cap * 2 : 32;
        h->items = xrealloc(h->items, h->cap * sizeof(*h->items));
    }
    hdr_ref_t *ref = &h->items[h->n++];
    ref->name_off = (uint32_t)h->arena.len;
    ref->name_len = (uint32_t)nlen;
    buf_append(&h->arena, name, nlen);
    ref->val_off = (uint32_t)h->arena.len;
    ref->val_len = (uint32_t)vlen;
    buf_append(&h->arena, val, vlen);
}

void headers_add_str(http_headers_t *h, const char *name, const char *val)
{
    headers_add(h, name, strlen(name), val, strlen(val));
}

void headers_set_str(http_headers_t *h, const char *name, const char *val)
{
    headers_remove(h, name);
    headers_add_str(h, name, val);
}

const char *headers_name(const http_headers_t *h, size_t i, size_t *len)
{
    *len = h->items[i].name_len;
    return h->arena.data + h->items[i].name_off;
}

const char *headers_value(const http_headers_t *h, size_t i, size_t *len)
{
    *len = h->items[i].val_len;
    return h->arena.data + h->items[i].val_off;
}

bool headers_get(const http_headers_t *h, const char *name, const char **val, size_t *vlen)
{
    size_t nlen = strlen(name);
    for (size_t i = 0; i < h->n; i++) {
        const hdr_ref_t *r = &h->items[i];
        if (str_ieq(h->arena.data + r->name_off, r->name_len, name, nlen)) {
            *val = h->arena.data + r->val_off;
            *vlen = r->val_len;
            return true;
        }
    }
    return false;
}

size_t headers_remove(http_headers_t *h, const char *name)
{
    size_t nlen = strlen(name), removed = 0, w = 0;
    for (size_t i = 0; i < h->n; i++) {
        const hdr_ref_t *r = &h->items[i];
        if (str_ieq(h->arena.data + r->name_off, r->name_len, name, nlen)) {
            removed++;
            continue; /* arena bytes are left behind; reclaimed on reset */
        }
        h->items[w++] = h->items[i];
    }
    h->n = w;
    return removed;
}

bool header_value_has_token(const char *val, size_t vlen, const char *token)
{
    size_t tlen = strlen(token);
    size_t i = 0;
    while (i < vlen) {
        /* skip separators */
        while (i < vlen && (val[i] == ',' || val[i] == ' ' || val[i] == '\t'))
            i++;
        size_t start = i;
        while (i < vlen && val[i] != ',')
            i++;
        size_t end = i;
        while (end > start && (val[end - 1] == ' ' || val[end - 1] == '\t'))
            end--;
        if (str_ieq(val + start, end - start, token, tlen))
            return true;
    }
    return false;
}

/* ------------------------------------------------------------------------
 * Request
 * ---------------------------------------------------------------------- */
void http_request_init(http_request_t *r)
{
    memset(r, 0, sizeof(*r));
    headers_init(&r->headers);
    buf_init(&r->body);
    r->scheme = "http";
}

void http_request_free(http_request_t *r)
{
    free(r->target);
    headers_free(&r->headers);
    buf_free(&r->body);
    memset(r, 0, sizeof(*r));
}

void http_request_reset(http_request_t *r)
{
    r->method_len = 0;
    free(r->target);
    r->target = NULL;
    r->target_len = 0;
    r->version_major = r->version_minor = 0;
    headers_reset(&r->headers);
    buf_reset(&r->body);
    r->keep_alive = false;
}

void http_request_set_target(http_request_t *r, const char *target, size_t len)
{
    free(r->target);
    r->target = xstrndup(target, len);
    r->target_len = len;
}

bool http_request_method_is(const http_request_t *r, const char *method)
{
    size_t n = strlen(method);
    return r->method_len == n && memcmp(r->method, method, n) == 0;
}

size_t http_request_path_len(const http_request_t *r)
{
    const char *q = r->target ? memchr(r->target, '?', r->target_len) : NULL;
    return q ? (size_t)(q - r->target) : r->target_len;
}

/* ------------------------------------------------------------------------
 * Response
 * ---------------------------------------------------------------------- */
void http_response_init(http_response_t *r)
{
    memset(r, 0, sizeof(*r));
    headers_init(&r->headers);
    buf_init(&r->body);
}

void http_response_free(http_response_t *r)
{
    headers_free(&r->headers);
    buf_free(&r->body);
    memset(r, 0, sizeof(*r));
}

void http_response_reset(http_response_t *r)
{
    r->status = 0;
    headers_reset(&r->headers);
    buf_reset(&r->body);
    r->keep_alive = false;
}

void http_response_set_error(http_response_t *r, int status, const char *message)
{
    http_response_reset(r);
    r->status = status;
    headers_add_str(&r->headers, "Content-Type", "text/plain; charset=utf-8");
    headers_add_str(&r->headers, "Cache-Control", "no-store");
    buf_printf(&r->body, "%d %s\n", status, message ? message : http_status_reason(status));
}

const char *http_status_reason(int status)
{
    switch (status) {
    case 100: return "Continue";
    case 101: return "Switching Protocols";
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 206: return "Partial Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 303: return "See Other";
    case 304: return "Not Modified";
    case 307: return "Temporary Redirect";
    case 308: return "Permanent Redirect";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 411: return "Length Required";
    case 413: return "Content Too Large";
    case 414: return "URI Too Long";
    case 415: return "Unsupported Media Type";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    case 505: return "HTTP Version Not Supported";
    default: return "Unknown";
    }
}

bool http_header_is_hop_by_hop(const char *name, size_t len)
{
    static const char *hop[] = {"connection",        "keep-alive", "proxy-authenticate",
                                "proxy-authorization", "te",        "trailer",
                                "transfer-encoding", "upgrade",    "proxy-connection"};
    for (size_t i = 0; i < ARRAY_LEN(hop); i++)
        if (str_ieq_lit(name, len, hop[i]))
            return true;
    return false;
}

/* ------------------------------------------------------------------------
 * Binary framing helpers
 * ---------------------------------------------------------------------- */
static bool connection_specific(const char *name, size_t len)
{
    return str_ieq_lit(name, len, "connection") || str_ieq_lit(name, len, "keep-alive") ||
           str_ieq_lit(name, len, "proxy-connection") ||
           str_ieq_lit(name, len, "transfer-encoding") || str_ieq_lit(name, len, "upgrade") ||
           str_ieq_lit(name, len, "content-length");
}

size_t http_response_fields_binary(const http_response_t *r, size_t body_len, buf_t *scratch,
                                   http_field_t **out)
{
    size_t nh = headers_count(&r->headers);
    http_field_t *f = xcalloc(nh + 2, sizeof(*f));
    /* Offsets rather than pointers while filling: scratch may reallocate. */
    size_t *off = xcalloc((nh + 2) * 2, sizeof(*off));
    size_t n = 0;

    off[0] = scratch->len;
    buf_append_str(scratch, ":status");
    off[1] = scratch->len;
    buf_printf(scratch, "%d", r->status);
    f[n].nlen = 7;
    f[n].vlen = scratch->len - off[1];
    n++;

    for (size_t i = 0; i < nh; i++) {
        size_t nl, vl;
        const char *nm = headers_name(&r->headers, i, &nl);
        const char *v = headers_value(&r->headers, i, &vl);
        if (connection_specific(nm, nl))
            continue;
        off[n * 2] = scratch->len;
        for (size_t k = 0; k < nl; k++) {
            char lc = (char)tolower((unsigned char)nm[k]);
            buf_append(scratch, &lc, 1);
        }
        off[n * 2 + 1] = scratch->len;
        buf_append(scratch, v, vl);
        f[n].nlen = nl;
        f[n].vlen = vl;
        n++;
    }

    off[n * 2] = scratch->len;
    buf_append_str(scratch, "content-length");
    off[n * 2 + 1] = scratch->len;
    buf_printf(scratch, "%zu", body_len);
    f[n].nlen = 14;
    f[n].vlen = scratch->len - off[n * 2 + 1];
    n++;

    for (size_t i = 0; i < n; i++) {
        f[i].name = scratch->data + off[i * 2];
        f[i].val = scratch->data + off[i * 2 + 1];
    }
    free(off);
    *out = f;
    return n;
}
