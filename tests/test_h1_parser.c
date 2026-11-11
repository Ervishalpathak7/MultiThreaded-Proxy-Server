#include "http/h1_parser.h"

#include "test.h"

#define MAX_HEAD 8192
#define MAX_BODY (1024 * 1024)

static const char *SIMPLE = "GET /index.html?x=1 HTTP/1.1\r\n"
                            "Host: example.com\r\n"
                            "User-Agent: test\r\n"
                            "Accept:   text/html  \r\n"
                            "\r\n";

TEST(whole_request_in_one_buffer)
{
    h1_parser_t p;
    h1_parser_init(&p, H1_MODE_REQUEST, MAX_HEAD, MAX_BODY);
    http_request_t req;
    http_request_init(&req);
    buf_t in;
    buf_init(&in);
    buf_append_str(&in, SIMPLE);

    ASSERT_EQ(h1_parse_request(&p, &in, &req), H1_COMPLETE);
    ASSERT_SLICE_EQ(req.method, req.method_len, "GET");
    ASSERT_SLICE_EQ(req.target, req.target_len, "/index.html?x=1");
    ASSERT_EQ(http_request_path_len(&req), 11);
    ASSERT_EQ(req.version_minor, 1);
    ASSERT(req.keep_alive);
    ASSERT_EQ(headers_count(&req.headers), 3);
    const char *v;
    size_t vl;
    ASSERT(headers_get(&req.headers, "HOST", &v, &vl));
    ASSERT_SLICE_EQ(v, vl, "example.com");
    ASSERT(headers_get(&req.headers, "accept", &v, &vl));
    ASSERT_SLICE_EQ(v, vl, "text/html"); /* whitespace trimmed */
    ASSERT(buf_empty(&in));               /* everything consumed */

    buf_free(&in);
    http_request_free(&req);
}

TEST(request_split_byte_by_byte)
{
    const char *msg = "POST /submit HTTP/1.1\r\nHost: h\r\nContent-Length: 5\r\n\r\nhello";
    h1_parser_t p;
    h1_parser_init(&p, H1_MODE_REQUEST, MAX_HEAD, MAX_BODY);
    http_request_t req;
    http_request_init(&req);
    buf_t in;
    buf_init(&in);

    size_t len = strlen(msg);
    h1_result_t r = H1_NEED_MORE;
    for (size_t i = 0; i < len; i++) {
        buf_append(&in, msg + i, 1);
        r = h1_parse_request(&p, &in, &req);
        if (i < len - 1)
            ASSERT_EQ(r, H1_NEED_MORE);
    }
    ASSERT_EQ(r, H1_COMPLETE);
    ASSERT_SLICE_EQ(req.method, req.method_len, "POST");
    ASSERT_SLICE_EQ(buf_rptr(&req.body), buf_rlen(&req.body), "hello");

    buf_free(&in);
    http_request_free(&req);
}

TEST(pipelined_requests_leave_remainder)
{
    h1_parser_t p;
    h1_parser_init(&p, H1_MODE_REQUEST, MAX_HEAD, MAX_BODY);
    http_request_t req;
    http_request_init(&req);
    buf_t in;
    buf_init(&in);
    buf_append_str(&in, "GET /a HTTP/1.1\r\nHost: h\r\n\r\nGET /b HTTP/1.1\r\nHost: h\r\n\r\n");

    ASSERT_EQ(h1_parse_request(&p, &in, &req), H1_COMPLETE);
    ASSERT_SLICE_EQ(req.target, req.target_len, "/a");
    ASSERT_EQ(buf_rlen(&in), 28); /* second request still buffered */

    h1_parser_reset(&p);
    http_request_reset(&req);
    ASSERT_EQ(h1_parse_request(&p, &in, &req), H1_COMPLETE);
    ASSERT_SLICE_EQ(req.target, req.target_len, "/b");
    ASSERT(buf_empty(&in));

    buf_free(&in);
    http_request_free(&req);
}

TEST(chunked_body)
{
    const char *msg = "POST /up HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n"
                      "4\r\nWiki\r\n"
                      "6;ext=1\r\npedia \r\n"
                      "D\r\nin\r\n\r\nchunks.\r\n"
                      "0\r\n"
                      "X-Trailer: yes\r\n"
                      "\r\n"
                      "GET /next HTTP/1.1\r\n";
    h1_parser_t p;
    h1_parser_init(&p, H1_MODE_REQUEST, MAX_HEAD, MAX_BODY);
    http_request_t req;
    http_request_init(&req);
    buf_t in;
    buf_init(&in);

    /* Feed in awkward 7-byte pieces to exercise every resume point. */
    size_t len = strlen(msg);
    h1_result_t r = H1_NEED_MORE;
    for (size_t i = 0; i < len && r == H1_NEED_MORE; i += 7) {
        buf_append(&in, msg + i, i + 7 <= len ? 7 : len - i);
        r = h1_parse_request(&p, &in, &req);
    }
    ASSERT_EQ(r, H1_COMPLETE);
    ASSERT_SLICE_EQ(buf_rptr(&req.body), buf_rlen(&req.body), "Wikipedia in\r\n\r\nchunks.");

    buf_free(&in);
    http_request_free(&req);
}

static int parse_error_status(const char *msg)
{
    h1_parser_t p;
    h1_parser_init(&p, H1_MODE_REQUEST, MAX_HEAD, MAX_BODY);
    http_request_t req;
    http_request_init(&req);
    buf_t in;
    buf_init(&in);
    buf_append_str(&in, msg);
    h1_result_t r = h1_parse_request(&p, &in, &req);
    int status = r == H1_ERROR ? p.error_status : 0;
    buf_free(&in);
    http_request_free(&req);
    return status;
}

TEST(malformed_requests_are_rejected)
{
    ASSERT_EQ(parse_error_status("GARBAGE\r\n\r\n"), 400);
    ASSERT_EQ(parse_error_status("GET / HTTP/9.9\r\nHost: h\r\n\r\n"), 505);
    ASSERT_EQ(parse_error_status("GET / HTTP/1.1\r\n\r\n"), 400); /* missing Host */
    ASSERT_EQ(parse_error_status("GET / HTTP/1.1\r\nHost : h\r\n\r\n"), 400); /* space before colon */
    ASSERT_EQ(parse_error_status("GET / HTTP/1.1\r\nHost: h\r\n folded\r\n\r\n"), 400);
    ASSERT_EQ(parse_error_status("GET / HTTP/1.1\r\nHost: h\r\nContent-Length: abc\r\n\r\n"), 400);
    /* CL + TE together must be rejected (request smuggling). */
    ASSERT_EQ(parse_error_status("POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 3\r\n"
                                 "Transfer-Encoding: chunked\r\n\r\n0\r\n\r\n"),
              400);
    ASSERT_EQ(parse_error_status("POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: gzip\r\n\r\n"),
              501);
    ASSERT_EQ(parse_error_status("POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 1\r\n"
                                 "Content-Length: 2\r\n\r\n"),
              400);
    /* Bad chunk size. */
    ASSERT_EQ(parse_error_status("POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n"
                                 "zz\r\n"),
              400);
}

TEST(oversized_messages_are_rejected)
{
    h1_parser_t p;
    http_request_t req;
    buf_t in;

    /* Head larger than the limit -> 431 (even before the terminator). */
    h1_parser_init(&p, H1_MODE_REQUEST, 256, MAX_BODY);
    http_request_init(&req);
    buf_init(&in);
    buf_append_str(&in, "GET / HTTP/1.1\r\nHost: h\r\nX-Big: ");
    for (int i = 0; i < 300; i++)
        buf_append(&in, "a", 1);
    ASSERT_EQ(h1_parse_request(&p, &in, &req), H1_ERROR);
    ASSERT_EQ(p.error_status, 431);
    buf_free(&in);
    http_request_free(&req);

    /* Long URI -> 414 */
    h1_parser_init(&p, H1_MODE_REQUEST, 64 * 1024, MAX_BODY);
    http_request_init(&req);
    buf_init(&in);
    buf_append_str(&in, "GET /");
    for (int i = 0; i < 9000; i++)
        buf_append(&in, "a", 1);
    buf_append_str(&in, " HTTP/1.1\r\nHost: h\r\n\r\n");
    ASSERT_EQ(h1_parse_request(&p, &in, &req), H1_ERROR);
    ASSERT_EQ(p.error_status, 414);
    buf_free(&in);
    http_request_free(&req);

    /* Declared body larger than the limit -> 413 */
    h1_parser_init(&p, H1_MODE_REQUEST, MAX_HEAD, 100);
    http_request_init(&req);
    buf_init(&in);
    buf_append_str(&in, "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 101\r\n\r\n");
    ASSERT_EQ(h1_parse_request(&p, &in, &req), H1_ERROR);
    ASSERT_EQ(p.error_status, 413);
    buf_free(&in);
    http_request_free(&req);
}

TEST(http10_connection_semantics)
{
    h1_parser_t p;
    http_request_t req;
    buf_t in;

    h1_parser_init(&p, H1_MODE_REQUEST, MAX_HEAD, MAX_BODY);
    http_request_init(&req);
    buf_init(&in);
    buf_append_str(&in, "GET / HTTP/1.0\r\n\r\n");
    ASSERT_EQ(h1_parse_request(&p, &in, &req), H1_COMPLETE);
    ASSERT(!req.keep_alive);
    buf_free(&in);
    http_request_free(&req);

    h1_parser_init(&p, H1_MODE_REQUEST, MAX_HEAD, MAX_BODY);
    http_request_init(&req);
    buf_init(&in);
    buf_append_str(&in, "GET / HTTP/1.0\r\nConnection: Keep-Alive\r\n\r\n");
    ASSERT_EQ(h1_parse_request(&p, &in, &req), H1_COMPLETE);
    ASSERT(req.keep_alive);
    buf_free(&in);
    http_request_free(&req);

    h1_parser_init(&p, H1_MODE_REQUEST, MAX_HEAD, MAX_BODY);
    http_request_init(&req);
    buf_init(&in);
    buf_append_str(&in, "GET / HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n");
    ASSERT_EQ(h1_parse_request(&p, &in, &req), H1_COMPLETE);
    ASSERT(!req.keep_alive);
    buf_free(&in);
    http_request_free(&req);
}

TEST(response_with_content_length)
{
    h1_parser_t p;
    h1_parser_init(&p, H1_MODE_RESPONSE, MAX_HEAD, MAX_BODY);
    http_response_t resp;
    http_response_init(&resp);
    buf_t in;
    buf_init(&in);
    buf_append_str(&in, "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\n"
                        "Content-Length: 9\r\n\r\nnot found");
    ASSERT_EQ(h1_parse_response(&p, &in, &resp, false), H1_COMPLETE);
    ASSERT_EQ(resp.status, 404);
    ASSERT(resp.keep_alive);
    ASSERT_SLICE_EQ(buf_rptr(&resp.body), buf_rlen(&resp.body), "not found");
    buf_free(&in);
    http_response_free(&resp);
}

TEST(response_until_close_and_head)
{
    h1_parser_t p;
    http_response_t resp;
    buf_t in;

    /* No framing headers: body runs until EOF. */
    h1_parser_init(&p, H1_MODE_RESPONSE, MAX_HEAD, MAX_BODY);
    http_response_init(&resp);
    buf_init(&in);
    buf_append_str(&in, "HTTP/1.0 200 OK\r\n\r\nstream");
    ASSERT_EQ(h1_parse_response(&p, &in, &resp, false), H1_NEED_MORE);
    buf_append_str(&in, "ing");
    ASSERT_EQ(h1_parse_response(&p, &in, &resp, false), H1_NEED_MORE);
    ASSERT_EQ(h1_parse_eof(&p), H1_COMPLETE);
    ASSERT_SLICE_EQ(buf_rptr(&resp.body), buf_rlen(&resp.body), "streaming");
    ASSERT(!resp.keep_alive);
    buf_free(&in);
    http_response_free(&resp);

    /* Response to HEAD: Content-Length present but no body follows. */
    h1_parser_init(&p, H1_MODE_RESPONSE, MAX_HEAD, MAX_BODY);
    http_response_init(&resp);
    buf_init(&in);
    buf_append_str(&in, "HTTP/1.1 200 OK\r\nContent-Length: 1000\r\n\r\n");
    ASSERT_EQ(h1_parse_response(&p, &in, &resp, true), H1_COMPLETE);
    ASSERT_EQ(buf_rlen(&resp.body), 0);
    buf_free(&in);
    http_response_free(&resp);

    /* 204 never has a body. */
    h1_parser_init(&p, H1_MODE_RESPONSE, MAX_HEAD, MAX_BODY);
    http_response_init(&resp);
    buf_init(&in);
    buf_append_str(&in, "HTTP/1.1 204 No Content\r\n\r\n");
    ASSERT_EQ(h1_parse_response(&p, &in, &resp, false), H1_COMPLETE);
    buf_free(&in);
    http_response_free(&resp);

    /* Truncated response -> error. */
    h1_parser_init(&p, H1_MODE_RESPONSE, MAX_HEAD, MAX_BODY);
    http_response_init(&resp);
    buf_init(&in);
    buf_append_str(&in, "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nabc");
    ASSERT_EQ(h1_parse_response(&p, &in, &resp, false), H1_NEED_MORE);
    ASSERT_EQ(h1_parse_eof(&p), H1_ERROR);
    buf_free(&in);
    http_response_free(&resp);
}

TEST(serialization_roundtrip)
{
    http_response_t resp;
    http_response_init(&resp);
    resp.status = 200;
    headers_add_str(&resp.headers, "Content-Type", "text/html");
    headers_add_str(&resp.headers, "Transfer-Encoding", "chunked"); /* must be dropped */
    buf_append_str(&resp.body, "<b>hi</b>");

    buf_t out;
    buf_init(&out);
    h1_write_response_head(&out, &resp, buf_rlen(&resp.body), true);
    buf_append(&out, buf_rptr(&resp.body), buf_rlen(&resp.body));

    h1_parser_t p;
    h1_parser_init(&p, H1_MODE_RESPONSE, MAX_HEAD, MAX_BODY);
    http_response_t back;
    http_response_init(&back);
    ASSERT_EQ(h1_parse_response(&p, &out, &back, false), H1_COMPLETE);
    ASSERT_EQ(back.status, 200);
    const char *v;
    size_t vl;
    ASSERT(!headers_get(&back.headers, "transfer-encoding", &v, &vl));
    ASSERT(headers_get(&back.headers, "content-length", &v, &vl));
    ASSERT_SLICE_EQ(v, vl, "9");
    ASSERT(headers_get(&back.headers, "date", &v, &vl));
    ASSERT_SLICE_EQ(buf_rptr(&back.body), buf_rlen(&back.body), "<b>hi</b>");

    http_response_free(&back);
    http_response_free(&resp);
    buf_free(&out);
}

TEST(header_helpers)
{
    http_headers_t h;
    headers_init(&h);
    headers_add_str(&h, "Set-Cookie", "a=1");
    headers_add_str(&h, "Set-Cookie", "b=2");
    headers_add_str(&h, "Connection", "keep-alive, Upgrade");
    const char *v;
    size_t vl;
    ASSERT(headers_get(&h, "connection", &v, &vl));
    ASSERT(header_value_has_token(v, vl, "upgrade"));
    ASSERT(header_value_has_token(v, vl, "keep-alive"));
    ASSERT(!header_value_has_token(v, vl, "close"));
    ASSERT_EQ(headers_remove(&h, "set-cookie"), 2);
    ASSERT_EQ(headers_count(&h), 1);
    headers_set_str(&h, "Connection", "close");
    ASSERT_EQ(headers_count(&h), 1);
    ASSERT(headers_get(&h, "connection", &v, &vl));
    ASSERT_SLICE_EQ(v, vl, "close");
    ASSERT(http_header_is_hop_by_hop("Transfer-Encoding", 17));
    ASSERT(!http_header_is_hop_by_hop("Content-Type", 12));
    headers_free(&h);
}

int main(void)
{
    RUN(whole_request_in_one_buffer);
    RUN(request_split_byte_by_byte);
    RUN(pipelined_requests_leave_remainder);
    RUN(chunked_body);
    RUN(malformed_requests_are_rejected);
    RUN(oversized_messages_are_rejected);
    RUN(http10_connection_semantics);
    RUN(response_with_content_length);
    RUN(response_until_close_and_head);
    RUN(serialization_roundtrip);
    RUN(header_helpers);
    return TEST_RESULT();
}
