/*
 * pool.h - per-worker pool of idle keep-alive connections to upstreams.
 *
 * Opening a TCP connection per proxied request costs a round trip and
 * leaves TIME_WAIT sockets behind. Instead, when an upstream response has
 * been fully read on a keep-alive connection, the socket is parked here and
 * reused by the next request to the same server.
 *
 * The pool belongs to one worker and is only touched from that worker's
 * thread, so there is no locking. While parked, each socket stays
 * registered for readability: if the server closes it (idle timeout on
 * their side) we notice immediately and drop it instead of handing a dead
 * socket to a request.
 */
#ifndef APROXY_PROXY_POOL_H
#define APROXY_PROXY_POOL_H

#include <stddef.h>

#include "core/event_loop.h"
#include "lb/lb.h"

typedef struct upstream_pool upstream_pool_t;

upstream_pool_t *pool_new(event_loop_t *loop);
void pool_free(upstream_pool_t *p);

/* Take an idle connection to `s`; returns the fd or -1 if none. */
int pool_take(upstream_pool_t *p, lb_server_t *s);

/* Park `fd` (a healthy keep-alive connection to `s`). If the pool already
 * holds `max_per_server` connections for that server the fd is closed. */
void pool_put(upstream_pool_t *p, lb_server_t *s, int fd, size_t max_per_server);

size_t pool_size(const upstream_pool_t *p);

#endif /* APROXY_PROXY_POOL_H */
