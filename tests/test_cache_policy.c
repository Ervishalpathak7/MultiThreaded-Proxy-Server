#include "cache/policy.h"

#include "test.h"

static http_request_t req_get(const char *target)
{
    http_request_t r;
    http_request_init(&r);
    memcpy(r.method, "GET", 3);
    r.method_len = 3;
    http_request_set_target(&r, target, strlen(target));
    headers_add_str(&r.headers, "Host", "example.com");
    return r;
}

static http_response_t resp_with(int status, const char *cc)
{
    http_response_t r;
    http_response_init(&r);
    r.status = status;
    if (cc)
        headers_add_str(&r.headers, "Cache-Control", cc);
    buf_append_str(&r.body, "body");
    return r;
}

TEST(request_rules)
{
    http_request_t r = req_get("/a");
    ASSERT(cache_policy_request_cacheable(&r));

    memcpy(r.method, "HEAD", 4);
    r.method_len = 4;
    ASSERT(cache_policy_request_cacheable(&r));

    memcpy(r.method, "POST", 4);
    ASSERT(!cache_policy_request_cacheable(&r));

    memcpy(r.method, "GET", 3);
    r.method_len = 3;
    headers_add_str(&r.headers, "Authorization", "Bearer x");
    ASSERT(!cache_policy_request_cacheable(&r));
    headers_remove(&r.headers, "authorization");
    ASSERT(cache_policy_request_cacheable(&r));

    headers_add_str(&r.headers, "Cache-Control", "max-age=0, no-store");
    ASSERT(!cache_policy_request_cacheable(&r));
    http_request_free(&r);
}

TEST(response_rules_and_ttl)
{
    uint64_t ttl = 0;
    http_response_t r;

    r = resp_with(200, NULL);
    ASSERT(cache_policy_response_cacheable(&r, 60000, &ttl));
    ASSERT_EQ(ttl, 60000); /* default */
    http_response_free(&r);

    r = resp_with(200, "public, max-age=30");
    ASSERT(cache_policy_response_cacheable(&r, 60000, &ttl));
    ASSERT_EQ(ttl, 30000);
    http_response_free(&r);

    r = resp_with(200, "max-age=30, s-maxage=120");
    ASSERT(cache_policy_response_cacheable(&r, 60000, &ttl));
    ASSERT_EQ(ttl, 120000); /* s-maxage wins for a shared cache */
    http_response_free(&r);

    r = resp_with(200, "no-store");
    ASSERT(!cache_policy_response_cacheable(&r, 60000, &ttl));
    http_response_free(&r);

    r = resp_with(200, "private, max-age=100");
    ASSERT(!cache_policy_response_cacheable(&r, 60000, &ttl));
    http_response_free(&r);

    r = resp_with(200, "max-age=0");
    ASSERT(!cache_policy_response_cacheable(&r, 60000, &ttl));
    http_response_free(&r);

    r = resp_with(500, NULL);
    ASSERT(!cache_policy_response_cacheable(&r, 60000, &ttl));
    http_response_free(&r);

    r = resp_with(404, NULL);
    ASSERT(cache_policy_response_cacheable(&r, 60000, &ttl));
    http_response_free(&r);

    r = resp_with(200, NULL);
    headers_add_str(&r.headers, "Set-Cookie", "sid=1");
    ASSERT(!cache_policy_response_cacheable(&r, 60000, &ttl));
    http_response_free(&r);

    r = resp_with(200, NULL);
    headers_add_str(&r.headers, "Vary", "*");
    ASSERT(!cache_policy_response_cacheable(&r, 60000, &ttl));
    http_response_free(&r);
}

TEST(key_includes_scheme_host_and_target)
{
    http_request_t r = req_get("/p?q=1");
    buf_t key;
    buf_init(&key);
    cache_policy_build_key(&r, &key);
    ASSERT_SLICE_EQ(buf_rptr(&key), buf_rlen(&key), "http://example.com/p?q=1");
    buf_reset(&key);
    r.scheme = "https";
    cache_policy_build_key(&r, &key);
    ASSERT_SLICE_EQ(buf_rptr(&key), buf_rlen(&key), "https://example.com/p?q=1");
    buf_free(&key);
    http_request_free(&r);
}

TEST(fill_response_adds_age_and_hit_marker)
{
    cache_t *c = cache_new(1 << 20, 1);
    http_response_t src = resp_with(200, "max-age=60");
    headers_add_str(&src.headers, "Age", "999"); /* stale upstream Age is replaced */
    cache_put(c, "k", 1, &src, 60000, 0);
    cache_entry_t *e = cache_get(c, "k", 1, 0);
    ASSERT(e != NULL);
    if (e) {
        http_response_t out;
        http_response_init(&out);
        cache_policy_fill_response(&out, e, e->stored_at_s + 5);
        const char *v;
        size_t vl;
        ASSERT(headers_get(&out.headers, "age", &v, &vl));
        ASSERT_SLICE_EQ(v, vl, "5");
        ASSERT(headers_get(&out.headers, "x-cache", &v, &vl));
        ASSERT_SLICE_EQ(v, vl, "HIT");
        ASSERT_SLICE_EQ(buf_rptr(&out.body), buf_rlen(&out.body), "body");
        ASSERT_EQ(headers_remove(&out.headers, "age"), 1); /* exactly one Age */
        http_response_free(&out);
        cache_entry_unref(e);
    }
    http_response_free(&src);
    cache_free(c);
}

int main(void)
{
    RUN(request_rules);
    RUN(response_rules_and_ttl);
    RUN(key_includes_scheme_host_and_target);
    RUN(fill_response_adds_age_and_hit_marker);
    return TEST_RESULT();
}
