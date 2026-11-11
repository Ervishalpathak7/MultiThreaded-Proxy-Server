/*
 * server.h - process-wide state and the request pipeline entry point.
 */
#ifndef APROXY_SERVER_H
#define APROXY_SERVER_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "config/config.h"
#include "core/buffer.h"
#include "lb/health.h"
#include "lb/lb.h"
#include "net/listener.h"
#include "net/worker.h"

typedef struct http_stream http_stream_t;
typedef struct conn conn_t;

/* Counters shared by all workers. Relaxed atomics: they are statistics,
 * not synchronization. */
typedef struct server_stats {
    atomic_uint_fast64_t total_conns;
    atomic_uint_fast64_t active_conns;
    atomic_uint_fast64_t requests;
    atomic_uint_fast64_t cache_hits;
    atomic_uint_fast64_t cache_misses;
    atomic_uint_fast64_t upstream_requests;
    atomic_uint_fast64_t upstream_errors;
    atomic_uint_fast64_t bytes_in;
    atomic_uint_fast64_t bytes_out;
} server_stats_t;

typedef struct server {
    config_t *cfg;

    listener_t *listeners;
    size_t nlisteners;

    worker_t *workers;
    int nworkers;

    /* One load balancing group per cfg->upstreams[i], same index. */
    lb_group_t **groups;
    size_t ngroups;
    /* Active health checkers (run on worker 0's loop), one per group. */
    health_checker_t **health;

    struct cache *cache; /* shared response cache */

    server_stats_t stats;
    uint64_t started_at; /* wall clock seconds */
    atomic_bool stopping;
} server_t;

/* Create the server: opens listening sockets, builds shared state.
 * Takes ownership of cfg. */
server_t *server_new(config_t *cfg);
/* Start workers and block until SIGINT/SIGTERM; then shut down cleanly. */
int server_run(server_t *srv);
void server_free(server_t *srv);

/* Hooks used by the networking layer. */
void server_on_accept(server_t *srv, conn_t *c);
void server_worker_init(server_t *srv, worker_t *w);
void server_worker_cleanup(server_t *srv, worker_t *w);

/* Entry point of the request pipeline. Always ends with a call to the
 * stream's respond callback (or nothing at all if the stream is aborted). */
void server_dispatch(server_t *srv, http_stream_t *s);

/* Render the /__status JSON document. */
void server_stats_json(server_t *srv, buf_t *out);

#endif /* APROXY_SERVER_H */
