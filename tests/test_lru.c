#include "cache/lru.h"

#include <pthread.h>

#include "test.h"

static http_response_t make_resp(int status, const char *body)
{
    http_response_t r;
    http_response_init(&r);
    r.status = status;
    headers_add_str(&r.headers, "Content-Type", "text/plain");
    buf_append_str(&r.body, body);
    return r;
}

TEST(put_get_roundtrip)
{
    cache_t *c = cache_new(1 << 20, 4);
    http_response_t r = make_resp(200, "hello");
    ASSERT(cache_put(c, "k1", 2, &r, 1000, 0));
    cache_entry_t *e = cache_get(c, "k1", 2, 10);
    ASSERT(e != NULL);
    if (e) {
        ASSERT_EQ(e->status, 200);
        ASSERT_SLICE_EQ(buf_rptr(&e->body), buf_rlen(&e->body), "hello");
        const char *v;
        size_t vl;
        ASSERT(headers_get(&e->headers, "content-type", &v, &vl));
        cache_entry_unref(e);
    }
    ASSERT(cache_get(c, "nope", 4, 10) == NULL);
    ASSERT_EQ(atomic_load(&c->hits), 1);
    ASSERT_EQ(atomic_load(&c->misses), 1);
    ASSERT_EQ(cache_entries(c), 1);
    http_response_free(&r);
    cache_free(c);
}

TEST(entries_expire)
{
    cache_t *c = cache_new(1 << 20, 1);
    http_response_t r = make_resp(200, "x");
    cache_put(c, "k", 1, &r, 100, 1000);
    cache_entry_t *e = cache_get(c, "k", 1, 1099);
    ASSERT(e != NULL);
    if (e)
        cache_entry_unref(e);
    ASSERT(cache_get(c, "k", 1, 1100) == NULL); /* exactly at the deadline */
    ASSERT_EQ(cache_entries(c), 0);              /* removed on the way */
    http_response_free(&r);
    cache_free(c);
}

TEST(lru_eviction_by_bytes)
{
    /* One shard with room for 3 entries of ~1.2KB (body + struct overhead). */
    cache_t *c = cache_new(4000, 1);
    char body[1024];
    memset(body, 'a', sizeof(body) - 1);
    body[sizeof(body) - 1] = '\0';
    http_response_t r = make_resp(200, body);
    char key[8];
    for (int i = 0; i < 3; i++) {
        snprintf(key, sizeof(key), "k%d", i);
        ASSERT(cache_put(c, key, strlen(key), &r, 10000, 0));
    }
    ASSERT_EQ(cache_entries(c), 3);
    /* Touch k0 so k1 becomes the least recently used. */
    cache_entry_t *e = cache_get(c, "k0", 2, 1);
    ASSERT(e != NULL);
    if (e)
        cache_entry_unref(e);
    cache_put(c, "k3", 2, &r, 10000, 0);
    ASSERT(cache_get(c, "k1", 2, 1) == NULL); /* evicted */
    e = cache_get(c, "k0", 2, 1);
    ASSERT(e != NULL); /* survived because it was touched */
    if (e)
        cache_entry_unref(e);
    ASSERT(atomic_load(&c->evictions) >= 1);
    ASSERT(cache_bytes(c) <= 4000);

    /* An entry bigger than the whole budget is refused. */
    char huge[5000];
    memset(huge, 'b', sizeof(huge) - 1);
    huge[sizeof(huge) - 1] = '\0';
    http_response_t big = make_resp(200, huge);
    ASSERT(!cache_put(c, "big", 3, &big, 1000, 0));
    http_response_free(&big);
    http_response_free(&r);
    cache_free(c);
}

TEST(replace_and_delete)
{
    cache_t *c = cache_new(1 << 20, 2);
    http_response_t a = make_resp(200, "v1"), b = make_resp(404, "v2");
    cache_put(c, "k", 1, &a, 1000, 0);
    cache_put(c, "k", 1, &b, 1000, 0);
    ASSERT_EQ(cache_entries(c), 1);
    cache_entry_t *e = cache_get(c, "k", 1, 0);
    ASSERT(e && e->status == 404);
    if (e)
        cache_entry_unref(e);
    ASSERT(cache_delete(c, "k", 1));
    ASSERT(!cache_delete(c, "k", 1));
    ASSERT(cache_get(c, "k", 1, 0) == NULL);
    http_response_free(&a);
    http_response_free(&b);
    cache_free(c);
}

TEST(reader_keeps_entry_alive_after_eviction)
{
    cache_t *c = cache_new(1 << 20, 1);
    http_response_t r = make_resp(200, "still here");
    cache_put(c, "k", 1, &r, 1000, 0);
    cache_entry_t *e = cache_get(c, "k", 1, 0);
    ASSERT(e != NULL);
    cache_delete(c, "k", 1); /* shard drops its reference */
    /* Under ASan this would blow up if the entry had been freed. */
    ASSERT_SLICE_EQ(buf_rptr(&e->body), buf_rlen(&e->body), "still here");
    cache_entry_unref(e);
    http_response_free(&r);
    cache_free(c);
}

TEST(table_grows_with_many_keys)
{
    cache_t *c = cache_new(64 << 20, 4);
    http_response_t r = make_resp(200, "v");
    char key[32];
    for (int i = 0; i < 5000; i++) {
        snprintf(key, sizeof(key), "/path/%d", i);
        cache_put(c, key, strlen(key), &r, 100000, 0);
    }
    ASSERT_EQ(cache_entries(c), 5000);
    for (int i = 0; i < 5000; i += 97) {
        snprintf(key, sizeof(key), "/path/%d", i);
        cache_entry_t *e = cache_get(c, key, strlen(key), 0);
        ASSERT(e != NULL);
        if (e)
            cache_entry_unref(e);
    }
    http_response_free(&r);
    cache_free(c);
}

/* Hammer the cache from several threads to flush out locking mistakes
 * (run under TSan/ASan for best effect). */
static cache_t *shared;
static void *hammer(void *arg)
{
    int id = (int)(intptr_t)arg;
    http_response_t r = make_resp(200, "body");
    char key[32];
    for (int i = 0; i < 20000; i++) {
        snprintf(key, sizeof(key), "k%d", (i * 7 + id) % 500);
        if (i % 3 == 0)
            cache_put(shared, key, strlen(key), &r, 1000, 0);
        cache_entry_t *e = cache_get(shared, key, strlen(key), 0);
        if (e)
            cache_entry_unref(e);
        if (i % 11 == 0)
            cache_delete(shared, key, strlen(key));
    }
    http_response_free(&r);
    return NULL;
}

TEST(concurrent_access)
{
    shared = cache_new(200 * 1024, 8);
    pthread_t th[4];
    for (int i = 0; i < 4; i++)
        pthread_create(&th[i], NULL, hammer, (void *)(intptr_t)i);
    for (int i = 0; i < 4; i++)
        pthread_join(th[i], NULL);
    ASSERT(cache_bytes(shared) <= 200 * 1024);
    cache_free(shared);
}

int main(void)
{
    RUN(put_get_roundtrip);
    RUN(entries_expire);
    RUN(lru_eviction_by_bytes);
    RUN(replace_and_delete);
    RUN(reader_keeps_entry_alive_after_eviction);
    RUN(table_grows_with_many_keys);
    RUN(concurrent_access);
    return TEST_RESULT();
}
