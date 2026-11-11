/*
 * worker.h - one event-loop thread.
 *
 * The server starts N workers (N = CPU cores by default). Each one owns an
 * event loop, accepts connections from the shared listening sockets, and
 * drives those connections to completion without ever handing them to
 * another thread. State that is per worker (upstream connection pool,
 * live connection list) therefore needs no locking at all; only the cache
 * and a few counters are shared.
 */
#ifndef APROXY_NET_WORKER_H
#define APROXY_NET_WORKER_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

#include "core/event_loop.h"

typedef struct server server_t;
typedef struct conn conn_t;
typedef struct upstream_pool upstream_pool_t;

typedef struct worker {
    int id;
    pthread_t thread;
    event_loop_t *loop;
    server_t *srv;

    conn_t *conns; /* intrusive doubly linked list */
    size_t nconns;

    upstream_pool_t *pool; /* idle keep-alive connections to upstreams */

    atomic_bool running;
    bool draining; /* shutting down: stop accepting, finish in-flight work */
} worker_t;

int worker_start(worker_t *w, server_t *srv, int id);
/* Ask the loop to exit (thread safe) and wait for the thread. */
void worker_stop(worker_t *w);
void worker_join(worker_t *w);

/* Called by conn.c to keep the live list in sync. */
void worker_add_conn(worker_t *w, conn_t *c);
void worker_remove_conn(worker_t *w, conn_t *c);

#endif /* APROXY_NET_WORKER_H */
