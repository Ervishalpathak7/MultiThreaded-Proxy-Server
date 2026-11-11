/*
 * h1_parser.h - incremental HTTP/1.x message parser.
 *
 * The parser is fed from a buf_t and consumes bytes as it goes, so it can
 * stop anywhere (mid-header, mid-chunk) and resume when the socket delivers
 * more data. It handles both directions:
 *
 *   - request mode:  used by the HTTP/1.1 frontend
 *   - response mode: used by the upstream client
 *
 * Body framing follows RFC 9112 §6: Transfer-Encoding: chunked wins over
 * Content-Length, and a message carrying both is rejected to prevent
 * request smuggling. Oversized heads/bodies are rejected with the proper
 * status code (413 / 414 / 431) so the caller can answer meaningfully.
 */
#ifndef APROXY_HTTP_H1_PARSER_H
#define APROXY_HTTP_H1_PARSER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/buffer.h"
#include "http/request.h"

typedef enum {
    H1_NEED_MORE = 0, /* incomplete; feed more bytes */
    H1_COMPLETE,      /* a full message is available */
    H1_ERROR,         /* malformed; see error_status */
} h1_result_t;

typedef enum {
    H1_MODE_REQUEST,
    H1_MODE_RESPONSE,
} h1_mode_t;

typedef enum {
    H1_BODY_NONE,
    H1_BODY_CONTENT_LENGTH,
    H1_BODY_CHUNKED,
    H1_BODY_UNTIL_CLOSE, /* responses with no framing: body ends at EOF */
} h1_body_kind_t;

typedef struct h1_parser {
    h1_mode_t mode;
    size_t max_header_size;
    size_t max_body_size;

    /* internal state */
    int state;
    size_t scan_pos; /* where to resume searching for the end of head */
    h1_body_kind_t body_kind;
    uint64_t content_length;
    uint64_t body_received;
    uint64_t chunk_remaining;
    bool head_response; /* response to a HEAD: no body regardless of headers */

    int error_status; /* HTTP status that describes the parse error */
    const char *error_reason;
} h1_parser_t;

void h1_parser_init(h1_parser_t *p, h1_mode_t mode, size_t max_header_size, size_t max_body_size);
/* Prepare for the next message on the same connection. */
void h1_parser_reset(h1_parser_t *p);

/* Request mode: parse from `in` into `req`. Consumes exactly the bytes of
 * one message so pipelined requests remain in the buffer. */
h1_result_t h1_parse_request(h1_parser_t *p, buf_t *in, http_request_t *req);

/* Response mode. `head_request` must be true when the request was HEAD. */
h1_result_t h1_parse_response(h1_parser_t *p, buf_t *in, http_response_t *resp, bool head_request);

/* Response mode only: the peer closed the connection. Completes a
 * H1_BODY_UNTIL_CLOSE body, otherwise reports a truncated message. */
h1_result_t h1_parse_eof(h1_parser_t *p);

/* True once the head has been parsed (body may still be pending). */
bool h1_parser_head_done(const h1_parser_t *p);

/* Serialize helpers used by both frontend and upstream client. */
void h1_write_request_head(buf_t *out, const http_request_t *req);
void h1_write_response_head(buf_t *out, const http_response_t *resp, size_t body_len,
                            bool keep_alive);

#endif /* APROXY_HTTP_H1_PARSER_H */
