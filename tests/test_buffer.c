#include "core/buffer.h"

#include "test.h"

TEST(append_and_read)
{
    buf_t b;
    buf_init(&b);
    ASSERT(buf_empty(&b));
    buf_append_str(&b, "hello ");
    buf_append(&b, "world", 5);
    ASSERT_EQ(buf_rlen(&b), 11);
    ASSERT_SLICE_EQ(buf_rptr(&b), buf_rlen(&b), "hello world");
    buf_free(&b);
}

TEST(consume_advances_cursor)
{
    buf_t b;
    buf_init(&b);
    buf_append_str(&b, "GET / HTTP/1.1\r\n");
    buf_consume(&b, 4);
    ASSERT_SLICE_EQ(buf_rptr(&b), buf_rlen(&b), "/ HTTP/1.1\r\n");
    buf_consume(&b, buf_rlen(&b));
    ASSERT(buf_empty(&b));
    /* Fully consumed buffers rewind to zero so the space is reused. */
    ASSERT_EQ(b.rpos, 0);
    ASSERT_EQ(b.len, 0);
    buf_free(&b);
}

TEST(grows_past_initial_capacity)
{
    buf_t b;
    buf_init(&b);
    char chunk[333];
    memset(chunk, 'x', sizeof(chunk));
    for (int i = 0; i < 100; i++)
        buf_append(&b, chunk, sizeof(chunk));
    ASSERT_EQ(buf_rlen(&b), 33300);
    ASSERT(b.cap >= 33300);
    for (size_t i = 0; i < buf_rlen(&b); i++)
        if (buf_rptr(&b)[i] != 'x') {
            TEST_FAIL("corrupted byte at %zu", i);
            break;
        }
    buf_free(&b);
}

TEST(printf_appends)
{
    buf_t b;
    buf_init(&b);
    int n = buf_printf(&b, "HTTP/1.1 %d %s\r\n", 200, "OK");
    ASSERT_EQ(n, 17);
    buf_printf(&b, "Content-Length: %zu\r\n", (size_t)42);
    ASSERT_SLICE_EQ(buf_rptr(&b), buf_rlen(&b), "HTTP/1.1 200 OK\r\nContent-Length: 42\r\n");
    buf_free(&b);
}

TEST(wptr_commit_roundtrip)
{
    buf_t b;
    buf_init(&b);
    buf_reserve(&b, 16);
    ASSERT(buf_wcap(&b) >= 16);
    memcpy(buf_wptr(&b), "abcdef", 6);
    buf_commit(&b, 6);
    ASSERT_SLICE_EQ(buf_rptr(&b), buf_rlen(&b), "abcdef");
    buf_free(&b);
}

TEST(compaction_reclaims_consumed_prefix)
{
    buf_t b;
    buf_init(&b);
    char chunk[2048];
    memset(chunk, 'a', sizeof(chunk));
    buf_append(&b, chunk, sizeof(chunk));
    buf_append_str(&b, "tail");
    size_t cap_before = b.cap;
    /* Consume most of it; the buffer should compact without growing. */
    buf_consume(&b, sizeof(chunk));
    ASSERT_SLICE_EQ(buf_rptr(&b), buf_rlen(&b), "tail");
    buf_append(&b, chunk, sizeof(chunk) - 100);
    ASSERT_EQ(b.cap, cap_before);
    buf_free(&b);
}

int main(void)
{
    RUN(append_and_read);
    RUN(consume_advances_cursor);
    RUN(grows_past_initial_capacity);
    RUN(printf_appends);
    RUN(wptr_commit_roundtrip);
    RUN(compaction_reclaims_consumed_prefix);
    return TEST_RESULT();
}
