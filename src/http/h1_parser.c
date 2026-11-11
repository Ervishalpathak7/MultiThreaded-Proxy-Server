/*
 * h1_parser.c - incremental HTTP/1.x parser (see h1_parser.h).
 *
 * Strategy: the head (start line + headers) is parsed only once the
 * terminating blank line has arrived, which keeps the header parser a
 * simple linear scan over a complete byte range. Incrementality comes from
 * remembering how far we already scanned for the terminator, so a head
 * split across N reads costs O(total) and not O(N * total). The body is
 * then streamed into the message buffer piece by piece.
 */
#include "http/h1_parser.h"

#include <stdio.h>
#include <string.h>

#include "core/util.h"

enum {
    ST_HEAD = 0,
    ST_BODY,
    ST_CHUNK_SIZE,
    ST_CHUNK_DATA,
    ST_CHUNK_DATA_CRLF,
    ST_TRAILERS,
    ST_DONE,
    ST_ERROR,
};

void h1_parser_init(h1_parser_t *p, h1_mode_t mode, size_t max_header_size, size_t max_body_size)
{
    memset(p, 0, sizeof(*p));
    p->mode = mode;
    p->max_header_size = max_header_size;
    p->max_body_size = max_body_size;
}

void h1_parser_reset(h1_parser_t *p)
{
    h1_parser_init(p, p->mode, p->max_header_size, p->max_body_size);
}

bool h1_parser_head_done(const h1_parser_t *p)
{
    return p->state != ST_HEAD && p->state != ST_ERROR;
}

static h1_result_t fail(h1_parser_t *p, int status, const char *reason)
{
    p->state = ST_ERROR;
    p->error_status = status;
    p->error_reason = reason;
    return H1_ERROR;
}

/* RFC 9110 token characters, used for method and header names. */
static bool is_tchar(unsigned char c)
{
    if (c >= 'a' && c <= 'z')
        return true;
    if (c >= 'A' && c <= 'Z')
        return true;
    if (c >= '0' && c <= '9')
        return true;
    return strchr("!#$%&'*+-.^_`|~", c) != NULL && c != '\0';
}

/* Find the end of the head starting at scan_pos. Returns the length of the
 * head including the terminator, or 0 if not yet complete. Accepts both
 * CRLF CRLF and bare LF LF (lenient, like most servers). */
static size_t find_head_end(h1_parser_t *p, const char *data, size_t len)
{
    size_t i = p->scan_pos;
    for (; i < len; i++) {
        if (data[i] != '\n')
            continue;
        if (i >= 3 && data[i - 1] == '\r' && data[i - 2] == '\n' && data[i - 3] == '\r')
            return i + 1;
        if (i >= 1 && data[i - 1] == '\n')
            return i + 1;
    }
    /* Resume a few bytes back in case the terminator straddles two reads. */
    p->scan_pos = len > 3 ? len - 3 : 0;
    return 0;
}

/* Strip one line ending from [start, end). */
static size_t line_end(const char *s, size_t len)
{
    if (len && s[len - 1] == '\n')
        len--;
    if (len && s[len - 1] == '\r')
        len--;
    return len;
}

/* Parse "HTTP/x.y"; returns false if malformed. */
static bool parse_version(const char *s, size_t len, int *major, int *minor)
{
    if (len != 8 || memcmp(s, "HTTP/", 5) != 0 || s[6] != '.')
        return false;
    if (s[5] < '0' || s[5] > '9' || s[7] < '0' || s[7] > '9')
        return false;
    *major = s[5] - '0';
    *minor = s[7] - '0';
    return true;
}

/* Parse header lines in [s, s+len) into `h`. Returns 0 or an HTTP error. */
static int parse_header_lines(h1_parser_t *p, const char *s, size_t len, http_headers_t *h)
{
    size_t pos = 0;
    while (pos < len) {
        const char *nl = memchr(s + pos, '\n', len - pos);
        size_t ll = nl ? (size_t)(nl - (s + pos)) + 1 : len - pos;
        const char *line = s + pos;
        size_t n = line_end(line, ll);
        pos += ll;
        if (n == 0)
            break; /* blank line: end of headers */

        /* Obsolete line folding (leading whitespace) is a smuggling vector. */
        if (line[0] == ' ' || line[0] == '\t')
            return 400;

        const char *colon = memchr(line, ':', n);
        if (!colon || colon == line)
            return 400;
        size_t nlen = (size_t)(colon - line);
        for (size_t i = 0; i < nlen; i++)
            if (!is_tchar((unsigned char)line[i]))
                return 400; /* includes "Header : v" which is forbidden */

        /* Trim optional whitespace around the value. */
        const char *v = colon + 1;
        size_t vlen = n - nlen - 1;
        while (vlen && (*v == ' ' || *v == '\t')) {
            v++;
            vlen--;
        }
        while (vlen && (v[vlen - 1] == ' ' || v[vlen - 1] == '\t'))
            vlen--;
        for (size_t i = 0; i < vlen; i++) {
            unsigned char c = (unsigned char)v[i];
            if (c < 0x20 && c != '\t')
                return 400; /* control characters in a field value */
        }
        headers_add(h, line, nlen, v, vlen);
    }
    return 0;
}

/* Decide how the body is framed once the head has been parsed. Shared by
 * request and response mode; `status` is 0 for requests. */
static h1_result_t setup_body(h1_parser_t *p, const http_headers_t *h, int status)
{
    const char *te, *cl;
    size_t telen, cllen;
    bool has_te = headers_get(h, "transfer-encoding", &te, &telen);
    bool has_cl = headers_get(h, "content-length", &cl, &cllen);

    if (has_te && has_cl) {
        /* RFC 9112 §6.3: a message with both is very likely an attempt at
         * request smuggling. Reject instead of guessing. */
        return fail(p, 400, "both Transfer-Encoding and Content-Length");
    }

    if (p->mode == H1_MODE_RESPONSE) {
        /* Messages that never carry a body regardless of headers. */
        if (p->head_response || status / 100 == 1 || status == 204 || status == 304) {
            p->body_kind = H1_BODY_NONE;
            p->state = ST_DONE;
            return H1_COMPLETE;
        }
    }

    if (has_te) {
        /* Only "chunked" as the final coding is supported; anything else
         * means we cannot find the message boundary. */
        if (!header_value_has_token(te, telen, "chunked") ||
            !str_ieq_lit(te + telen - 7 > te ? te + telen - 7 : te,
                         telen >= 7 ? 7 : telen, "chunked"))
            return fail(p, p->mode == H1_MODE_REQUEST ? 501 : 502,
                        "unsupported Transfer-Encoding");
        p->body_kind = H1_BODY_CHUNKED;
        p->state = ST_CHUNK_SIZE;
        return H1_NEED_MORE;
    }

    if (has_cl) {
        uint64_t v;
        /* Reject duplicate differing Content-Length values too. */
        if (!parse_u64(cl, cllen, &v))
            return fail(p, 400, "invalid Content-Length");
        for (size_t i = 0; i < h->n; i++) {
            size_t nl, vl;
            const char *nm = headers_name(h, i, &nl);
            if (str_ieq_lit(nm, nl, "content-length")) {
                const char *vv = headers_value(h, i, &vl);
                uint64_t v2;
                if (!parse_u64(vv, vl, &v2) || v2 != v)
                    return fail(p, 400, "conflicting Content-Length");
            }
        }
        if (v > p->max_body_size)
            return fail(p, 413, "body too large");
        p->content_length = v;
        p->body_kind = H1_BODY_CONTENT_LENGTH;
        if (v == 0) {
            p->state = ST_DONE;
            return H1_COMPLETE;
        }
        p->state = ST_BODY;
        return H1_NEED_MORE;
    }

    if (p->mode == H1_MODE_REQUEST) {
        /* Requests without framing have no body. */
        p->body_kind = H1_BODY_NONE;
        p->state = ST_DONE;
        return H1_COMPLETE;
    }
    /* Responses without framing run until the connection closes. */
    p->body_kind = H1_BODY_UNTIL_CLOSE;
    p->state = ST_BODY;
    return H1_NEED_MORE;
}

/* ------------------------------------------------------------------------
 * Body state machine, shared by both modes.
 * ---------------------------------------------------------------------- */
static h1_result_t parse_body(h1_parser_t *p, buf_t *in, buf_t *body)
{
    for (;;) {
        const char *data = buf_rptr(in);
        size_t avail = buf_rlen(in);

        switch (p->state) {
        case ST_BODY: {
            if (p->body_kind == H1_BODY_UNTIL_CLOSE) {
                if (p->body_received + avail > p->max_body_size)
                    return fail(p, 413, "body too large");
                buf_append(body, data, avail);
                buf_consume(in, avail);
                p->body_received += avail;
                return H1_NEED_MORE;
            }
            uint64_t remaining = p->content_length - p->body_received;
            size_t take = (size_t)MIN((uint64_t)avail, remaining);
            buf_append(body, data, take);
            buf_consume(in, take);
            p->body_received += take;
            if (p->body_received == p->content_length) {
                p->state = ST_DONE;
                return H1_COMPLETE;
            }
            return H1_NEED_MORE;
        }

        case ST_CHUNK_SIZE: {
            const char *nl = memchr(data, '\n', avail);
            if (!nl) {
                if (avail > 1024)
                    return fail(p, 400, "chunk size line too long");
                return H1_NEED_MORE;
            }
            size_t ll = (size_t)(nl - data) + 1;
            size_t n = line_end(data, ll);
            /* Ignore chunk extensions (";name=value"). */
            const char *semi = memchr(data, ';', n);
            size_t hexlen = semi ? (size_t)(semi - data) : n;
            if (hexlen == 0 || hexlen > 16)
                return fail(p, 400, "bad chunk size");
            uint64_t size = 0;
            for (size_t i = 0; i < hexlen; i++) {
                char c = data[i];
                int d;
                if (c >= '0' && c <= '9')
                    d = c - '0';
                else if (c >= 'a' && c <= 'f')
                    d = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F')
                    d = c - 'A' + 10;
                else
                    return fail(p, 400, "bad chunk size");
                size = size * 16 + (uint64_t)d;
            }
            buf_consume(in, ll);
            if (p->body_received + size > p->max_body_size)
                return fail(p, 413, "body too large");
            p->chunk_remaining = size;
            p->state = size == 0 ? ST_TRAILERS : ST_CHUNK_DATA;
            break;
        }

        case ST_CHUNK_DATA: {
            if (avail == 0)
                return H1_NEED_MORE;
            size_t take = (size_t)MIN((uint64_t)avail, p->chunk_remaining);
            buf_append(body, data, take);
            buf_consume(in, take);
            p->body_received += take;
            p->chunk_remaining -= take;
            if (p->chunk_remaining == 0)
                p->state = ST_CHUNK_DATA_CRLF;
            break;
        }

        case ST_CHUNK_DATA_CRLF: {
            /* Each chunk's data is followed by CRLF. */
            if (avail == 0)
                return H1_NEED_MORE;
            if (data[0] == '\r') {
                if (avail < 2)
                    return H1_NEED_MORE;
                if (data[1] != '\n')
                    return fail(p, 400, "bad chunk terminator");
                buf_consume(in, 2);
            } else if (data[0] == '\n') {
                buf_consume(in, 1);
            } else {
                return fail(p, 400, "bad chunk terminator");
            }
            p->state = ST_CHUNK_SIZE;
            break;
        }

        case ST_TRAILERS: {
            /* Trailer section: header lines terminated by a blank line.
             * We discard trailers (they are rarely used by origin servers). */
            const char *nl = memchr(data, '\n', avail);
            if (!nl) {
                if (avail > p->max_header_size)
                    return fail(p, 431, "trailers too large");
                return H1_NEED_MORE;
            }
            size_t ll = (size_t)(nl - data) + 1;
            size_t n = line_end(data, ll);
            buf_consume(in, ll);
            if (n == 0) {
                p->state = ST_DONE;
                return H1_COMPLETE;
            }
            break;
        }

        case ST_DONE:
            return H1_COMPLETE;
        default:
            return H1_ERROR;
        }
    }
}

/* ------------------------------------------------------------------------
 * Request mode
 * ---------------------------------------------------------------------- */
h1_result_t h1_parse_request(h1_parser_t *p, buf_t *in, http_request_t *req)
{
    if (p->state == ST_ERROR)
        return H1_ERROR;
    if (p->state == ST_DONE)
        return H1_COMPLETE;

    if (p->state == ST_HEAD) {
        const char *data = buf_rptr(in);
        size_t len = buf_rlen(in);
        size_t head_len = find_head_end(p, data, len);
        if (head_len == 0) {
            if (len > p->max_header_size)
                return fail(p, 431, "request head too large");
            return H1_NEED_MORE;
        }
        if (head_len > p->max_header_size)
            return fail(p, 431, "request head too large");

        /* --- request line: METHOD SP target SP HTTP/x.y --- */
        const char *nl = memchr(data, '\n', head_len);
        size_t ll = (size_t)(nl - data) + 1;
        size_t n = line_end(data, ll);
        const char *sp1 = memchr(data, ' ', n);
        if (!sp1)
            return fail(p, 400, "malformed request line");
        size_t mlen = (size_t)(sp1 - data);
        if (mlen == 0 || mlen >= HTTP_METHOD_MAX)
            return fail(p, 400, "bad method");
        for (size_t i = 0; i < mlen; i++)
            if (!is_tchar((unsigned char)data[i]))
                return fail(p, 400, "bad method");
        const char *target = sp1 + 1;
        const char *sp2 = memchr(target, ' ', n - mlen - 1);
        if (!sp2)
            return fail(p, 400, "malformed request line");
        size_t tlen = (size_t)(sp2 - target);
        if (tlen == 0)
            return fail(p, 400, "empty request target");
        if (tlen > 8192)
            return fail(p, 414, "URI too long");
        for (size_t i = 0; i < tlen; i++) {
            unsigned char c = (unsigned char)target[i];
            if (c <= 0x20 || c == 0x7f)
                return fail(p, 400, "bad character in target");
        }
        const char *ver = sp2 + 1;
        size_t vlen = (size_t)(data + n - ver);
        int major, minor;
        if (!parse_version(ver, vlen, &major, &minor))
            return fail(p, 400, "bad HTTP version");
        if (major != 1)
            return fail(p, 505, "unsupported HTTP version");

        memcpy(req->method, data, mlen);
        req->method_len = mlen;
        http_request_set_target(req, target, tlen);
        req->version_major = major;
        req->version_minor = minor;

        /* --- headers --- */
        int err = parse_header_lines(p, data + ll, head_len - ll, &req->headers);
        if (err)
            return fail(p, err, "malformed header");
        if (req->headers.n > 100)
            return fail(p, 431, "too many headers");

        /* HTTP/1.1 requires Host. */
        const char *host;
        size_t hostlen;
        if (minor >= 1 && !headers_get(&req->headers, "host", &host, &hostlen))
            return fail(p, 400, "missing Host header");

        /* Connection persistence: 1.1 defaults to keep-alive, 1.0 to close. */
        const char *conn;
        size_t connlen;
        bool has_conn = headers_get(&req->headers, "connection", &conn, &connlen);
        if (minor >= 1)
            req->keep_alive = !(has_conn && header_value_has_token(conn, connlen, "close"));
        else
            req->keep_alive = has_conn && header_value_has_token(conn, connlen, "keep-alive");

        buf_consume(in, head_len);
        h1_result_t r = setup_body(p, &req->headers, 0);
        if (r != H1_NEED_MORE)
            return r;
    }
    return parse_body(p, in, &req->body);
}

/* ------------------------------------------------------------------------
 * Response mode
 * ---------------------------------------------------------------------- */
h1_result_t h1_parse_response(h1_parser_t *p, buf_t *in, http_response_t *resp, bool head_request)
{
    if (p->state == ST_ERROR)
        return H1_ERROR;
    if (p->state == ST_DONE)
        return H1_COMPLETE;

    if (p->state == ST_HEAD) {
        p->head_response = head_request;
        const char *data = buf_rptr(in);
        size_t len = buf_rlen(in);
        size_t head_len = find_head_end(p, data, len);
        if (head_len == 0) {
            if (len > p->max_header_size)
                return fail(p, 502, "response head too large");
            return H1_NEED_MORE;
        }
        if (head_len > p->max_header_size)
            return fail(p, 502, "response head too large");

        /* --- status line: HTTP/x.y SP 3DIGIT SP reason --- */
        const char *nl = memchr(data, '\n', head_len);
        size_t ll = (size_t)(nl - data) + 1;
        size_t n = line_end(data, ll);
        int major, minor;
        if (n < 12 || !parse_version(data, 8, &major, &minor) || data[8] != ' ')
            return fail(p, 502, "malformed status line");
        uint64_t status;
        if (!parse_u64(data + 9, 3, &status) || status < 100 || status > 999)
            return fail(p, 502, "bad status code");
        resp->status = (int)status;

        int err = parse_header_lines(p, data + ll, head_len - ll, &resp->headers);
        if (err)
            return fail(p, 502, "malformed response header");

        const char *conn;
        size_t connlen;
        bool has_conn = headers_get(&resp->headers, "connection", &conn, &connlen);
        if (minor >= 1)
            resp->keep_alive = !(has_conn && header_value_has_token(conn, connlen, "close"));
        else
            resp->keep_alive = has_conn && header_value_has_token(conn, connlen, "keep-alive");

        buf_consume(in, head_len);
        h1_result_t r = setup_body(p, &resp->headers, resp->status);
        if (r != H1_NEED_MORE)
            return r;
        /* A body that ends at EOF obviously cannot be followed by another
         * response on the same connection. */
        if (p->body_kind == H1_BODY_UNTIL_CLOSE)
            resp->keep_alive = false;
    }
    return parse_body(p, in, &resp->body);
}

h1_result_t h1_parse_eof(h1_parser_t *p)
{
    if (p->state == ST_BODY && p->body_kind == H1_BODY_UNTIL_CLOSE) {
        p->state = ST_DONE;
        return H1_COMPLETE;
    }
    if (p->state == ST_DONE)
        return H1_COMPLETE;
    return fail(p, 502, "connection closed mid-message");
}

/* ------------------------------------------------------------------------
 * Serialization
 * ---------------------------------------------------------------------- */
void h1_write_request_head(buf_t *out, const http_request_t *req)
{
    buf_append(out, req->method, req->method_len);
    buf_append(out, " ", 1);
    buf_append(out, req->target, req->target_len);
    buf_append_str(out, " HTTP/1.1\r\n");
    for (size_t i = 0; i < req->headers.n; i++) {
        size_t nl, vl;
        const char *nm = headers_name(&req->headers, i, &nl);
        const char *v = headers_value(&req->headers, i, &vl);
        buf_append(out, nm, nl);
        buf_append(out, ": ", 2);
        buf_append(out, v, vl);
        buf_append(out, "\r\n", 2);
    }
    buf_append(out, "\r\n", 2);
}

void h1_write_response_head(buf_t *out, const http_response_t *resp, size_t body_len,
                            bool keep_alive)
{
    buf_printf(out, "HTTP/1.1 %d %s\r\n", resp->status, http_status_reason(resp->status));
    bool has_date = false;
    for (size_t i = 0; i < resp->headers.n; i++) {
        size_t nl, vl;
        const char *nm = headers_name(&resp->headers, i, &nl);
        const char *v = headers_value(&resp->headers, i, &vl);
        /* Framing headers are regenerated below. */
        if (str_ieq_lit(nm, nl, "content-length") || str_ieq_lit(nm, nl, "transfer-encoding") ||
            str_ieq_lit(nm, nl, "connection"))
            continue;
        if (str_ieq_lit(nm, nl, "date"))
            has_date = true;
        buf_append(out, nm, nl);
        buf_append(out, ": ", 2);
        buf_append(out, v, vl);
        buf_append(out, "\r\n", 2);
    }
    if (!has_date) {
        char date[64];
        format_http_date(date, sizeof(date), wall_s());
        buf_printf(out, "Date: %s\r\n", date);
    }
    buf_printf(out, "Content-Length: %zu\r\n", body_len);
    buf_append_str(out, keep_alive ? "Connection: keep-alive\r\n" : "Connection: close\r\n");
    buf_append(out, "\r\n", 2);
}
