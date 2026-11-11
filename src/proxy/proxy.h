/*
 * proxy.h - the reverse proxy step of the request pipeline.
 *
 * Given a stream whose route resolved to an upstream group, pick a server,
 * rewrite the request for forwarding, send it, rewrite the response and
 * deliver it back to the frontend. Caching wraps around this module: the
 * server consults the cache before calling proxy_forward() and stores the
 * result afterwards.
 */
#ifndef APROXY_PROXY_PROXY_H
#define APROXY_PROXY_PROXY_H

#include "config/config.h"
#include "http/stream.h"
#include "lb/lb.h"

/* Forward the request; ends with stream_finish() or an error response.
 * `on_response` (optional) is invoked with the rewritten upstream response
 * before it is delivered, so the cache can store a copy. */
typedef void (*proxy_response_hook)(http_stream_t *s, void *arg);

void proxy_forward(http_stream_t *s, const config_route_t *route, lb_group_t *group,
                   proxy_response_hook hook, void *hook_arg);

/* Rewrite helpers, exposed for tests. */
void proxy_prepare_request(http_request_t *out, const http_request_t *in);
void proxy_prepare_response(http_response_t *resp);

#endif /* APROXY_PROXY_PROXY_H */
