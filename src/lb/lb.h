/*
 * lb.h - load balancing across the servers of an upstream group.
 *
 * One lb_group_t exists per `upstream { }` block. It is shared by all
 * workers, so its mutable state is either atomic (connection counts,
 * failure counters, health flags) or guarded by a tiny mutex (the smooth
 * weighted round robin weights, which must be updated together).
 *
 * Strategies:
 *   round_robin  - each request goes to the next server
 *   weighted     - nginx's smooth weighted round robin: a server with
 *                  weight 3 gets 3 of every 4 requests, spread out rather
 *                  than bunched (A A A B becomes A B A A)
 *   least_conn   - the server with the fewest in-flight requests, weight
 *                  adjusted
 *   ip_hash      - consistent hashing (ketama ring, 160 points per server)
 *                  on the client address, so a client sticks to one server
 *                  and only 1/N of clients move when a server is added
 *
 * Health: a server is skipped while it is marked down. Passive detection
 * marks it down after `max_fails` consecutive failures for `fail_timeout`;
 * active checks (health.c) flip `health_ok`.
 */
#ifndef APROXY_LB_LB_H
#define APROXY_LB_LB_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/socket.h>

#include "config/config.h"
#include "core/buffer.h"

typedef struct lb_server {
    const config_server_t *cfg;
    struct sockaddr_storage addr;
    socklen_t addrlen;
    char label[80]; /* "host:port" for logs and stats */

    atomic_int active;                   /* in-flight requests */
    atomic_int fails;                    /* consecutive failures */
    atomic_uint_fast64_t down_until_ms;  /* passive: 0 = up */
    atomic_bool health_ok;               /* active health check result */
    atomic_uint_fast64_t total_requests;
    atomic_uint_fast64_t total_failures;

    int current_weight; /* smooth WRR state, guarded by group->lock */
} lb_server_t;

typedef struct lb_group {
    const config_upstream_t *cfg;
    lb_server_t *servers;
    size_t n;
    lb_strategy_t strategy;

    atomic_uint rr_next;
    pthread_mutex_t lock;

    /* consistent hash ring: sorted by hash */
    struct ring_point {
        uint64_t hash;
        uint32_t server;
    } *ring;
    size_t ring_len;
} lb_group_t;

/* Resolve every server address and build the group. NULL on failure. */
lb_group_t *lb_group_new(const config_upstream_t *cfg);
void lb_group_free(lb_group_t *g);

/* Choose a server for a request. `key` is the client address (used by
 * ip_hash). Returns NULL when every server is down. */
lb_server_t *lb_pick(lb_group_t *g, const char *key, size_t keylen, uint64_t now_ms);

bool lb_server_available(const lb_server_t *s, uint64_t now_ms);

/* Bookkeeping hooks called by the upstream client. */
void lb_server_begin(lb_server_t *s);
void lb_server_end(lb_server_t *s);
void lb_server_success(lb_server_t *s);
void lb_server_failure(lb_server_t *s, uint64_t now_ms);

void lb_group_stats_json(lb_group_t *g, buf_t *out, uint64_t now_ms);

#endif /* APROXY_LB_LB_H */
