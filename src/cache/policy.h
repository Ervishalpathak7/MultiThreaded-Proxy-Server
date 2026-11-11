/*
 * policy.h - HTTP caching rules (a practical subset of RFC 9111).
 *
 * Requests are cacheable when they are GET or HEAD, carry no Authorization,
 * and do not say Cache-Control: no-store. Responses are stored when the
 * status is one of the "cacheable by default" codes, there is no
 * no-store / private / max-age=0, no Set-Cookie and no `Vary: *`. The TTL
 * comes from s-maxage, then max-age, then the configured default.
 *
 * The cache key is scheme + host + request target; HEAD shares the GET
 * entry (the frontend omits the body), and only GET responses are stored.
 */
#ifndef APROXY_CACHE_POLICY_H
#define APROXY_CACHE_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#include "cache/lru.h"
#include "http/request.h"

bool cache_policy_request_cacheable(const http_request_t *req);

/* Returns true if the response may be stored; *ttl_ms is set from the
 * response's freshness directives or `default_ttl_ms`. */
bool cache_policy_response_cacheable(const http_response_t *resp, uint64_t default_ttl_ms,
                                     uint64_t *ttl_ms);

/* Append the cache key for `req` to `key`. */
void cache_policy_build_key(const http_request_t *req, buf_t *key);

/* Copy a cached entry into `resp`, adding Age and X-Cache: HIT. */
void cache_policy_fill_response(http_response_t *resp, const cache_entry_t *e, uint64_t now_s);

#endif /* APROXY_CACHE_POLICY_H */
