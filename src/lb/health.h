/*
 * health.h - active upstream health checks.
 *
 * Every `interval`, each server of a group gets a tiny HTTP/1.1 GET to the
 * configured path. 2xx/3xx within the timeout means healthy; anything else
 * (connection refused, timeout, 5xx) flips lb_server_t.health_ok to false
 * and the load balancer stops routing to it until a later check passes.
 *
 * The checker is driven entirely by one worker's event loop with
 * non-blocking sockets, so it never stalls request processing.
 */
#ifndef APROXY_LB_HEALTH_H
#define APROXY_LB_HEALTH_H

#include "core/event_loop.h"
#include "lb/lb.h"

typedef struct health_checker health_checker_t;

/* Start periodic checks for `g` on `loop`. Returns NULL if the group has
 * health checks disabled. */
health_checker_t *health_checker_start(lb_group_t *g, event_loop_t *loop);
/* Stop and free. Must be called on the loop's thread. */
void health_checker_stop(health_checker_t *hc);

#endif /* APROXY_LB_HEALTH_H */
