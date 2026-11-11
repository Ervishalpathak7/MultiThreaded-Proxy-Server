/*
 * policy.c - caching rules (see policy.h).
 */
#include "cache/policy.h"

#include <stdio.h>
#include <string.h>

#include "core/util.h"

/* Scan a Cache-Control value for a directive; if it has "=N", parse N. */
static bool cc_has(const char *val, size_t vlen, const char *directive, uint64_t *num)
{
    size_t dlen = strlen(directive);
    size_t i = 0;
    while (i < vlen) {
        while (i < vlen && (val[i] == ',' || val[i] == ' ' || val[i] == '\t'))
            i++;
        size_t start = i;
        while (i < vlen && val[i] != ',')
            i++;
        size_t end = i;
        while (end > start && (val[end - 1] == ' ' || val[end - 1] == '\t'))
            end--;
        const char *eq = memchr(val + start, '=', end - start);
        size_t namelen = eq ? (size_t)(eq - (val + start)) : end - start;
        if (str_ieq(val + start, namelen, directive, dlen)) {
            if (num) {
                if (!eq)
                    return false;
                const char *v = eq + 1;
                size_t vl = (size_t)(val + end - v);
                if (vl && v[0] == '"') { /* quoted number, rare but legal */
                    v++;
                    vl -= vl >= 2 ? 2 : vl;
                }
                return parse_u64(v, vl, num);
            }
            return true;
        }
    }
    return false;
}

bool cache_policy_request_cacheable(const http_request_t *req)
{
    if (!http_request_method_is(req, "GET") && !http_request_method_is(req, "HEAD"))
        return false;
    const char *v;
    size_t vl;
    if (headers_get(&req->headers, "authorization", &v, &vl))
        return false;
    if (headers_get(&req->headers, "cache-control", &v, &vl) &&
        (cc_has(v, vl, "no-store", NULL) || cc_has(v, vl, "no-cache", NULL)))
        return false;
    if (headers_get(&req->headers, "pragma", &v, &vl) && cc_has(v, vl, "no-cache", NULL))
        return false;
    return true;
}

bool cache_policy_response_cacheable(const http_response_t *resp, uint64_t default_ttl_ms,
                                     uint64_t *ttl_ms)
{
    switch (resp->status) {
    case 200:
    case 203:
    case 204:
    case 206: /* ranges are not understood; be conservative */
        if (resp->status == 206)
            return false;
        break;
    case 301:
    case 308:
    case 404:
    case 405:
    case 410:
    case 414:
    case 501:
        break;
    default:
        return false;
    }

    const char *v;
    size_t vl;
    if (headers_get(&resp->headers, "set-cookie", &v, &vl))
        return false;
    if (headers_get(&resp->headers, "vary", &v, &vl) && vl == 1 && v[0] == '*')
        return false;

    *ttl_ms = default_ttl_ms;
    if (headers_get(&resp->headers, "cache-control", &v, &vl)) {
        if (cc_has(v, vl, "no-store", NULL) || cc_has(v, vl, "private", NULL) ||
            cc_has(v, vl, "no-cache", NULL))
            return false;
        uint64_t secs;
        if (cc_has(v, vl, "s-maxage", &secs) || cc_has(v, vl, "max-age", &secs)) {
            if (secs == 0)
                return false;
            *ttl_ms = secs * 1000;
        }
    }
    return *ttl_ms > 0;
}

void cache_policy_build_key(const http_request_t *req, buf_t *key)
{
    buf_append_str(key, req->scheme ? req->scheme : "http");
    buf_append_str(key, "://");
    const char *host;
    size_t hl;
    if (headers_get(&req->headers, "host", &host, &hl) ||
        headers_get(&req->headers, ":authority", &host, &hl))
        buf_append(key, host, hl);
    buf_append(key, req->target, req->target_len);
}

void cache_policy_fill_response(http_response_t *resp, const cache_entry_t *e, uint64_t now_s)
{
    http_response_reset(resp);
    resp->status = e->status;
    bool has_age = false;
    for (size_t i = 0; i < headers_count(&e->headers); i++) {
        size_t nl, vl;
        const char *n = headers_name(&e->headers, i, &nl);
        const char *v = headers_value(&e->headers, i, &vl);
        if (str_ieq_lit(n, nl, "age")) {
            has_age = true;
            continue; /* recomputed below */
        }
        if (str_ieq_lit(n, nl, "x-cache"))
            continue;
        headers_add(&resp->headers, n, nl, v, vl);
    }
    (void)has_age;
    char age[24];
    snprintf(age, sizeof(age), "%llu",
             (unsigned long long)(now_s >= e->stored_at_s ? now_s - e->stored_at_s : 0));
    headers_add_str(&resp->headers, "Age", age);
    headers_add_str(&resp->headers, "X-Cache", "HIT");
    buf_append(&resp->body, buf_rptr(&e->body), buf_rlen(&e->body));
}
