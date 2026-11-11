/*
 * pool.c - idle upstream connection pool (see pool.h).
 */
#include "proxy/pool.h"

#include <stdlib.h>
#include <unistd.h>

#include "core/log.h"
#include "core/util.h"

typedef struct pooled {
    int fd;
    lb_server_t *server;
    upstream_pool_t *pool;
    struct pooled *prev, *next;
} pooled_t;

struct upstream_pool {
    event_loop_t *loop;
    pooled_t *head;
    size_t n;
};

upstream_pool_t *pool_new(event_loop_t *loop)
{
    upstream_pool_t *p = xcalloc(1, sizeof(*p));
    p->loop = loop;
    return p;
}

static void unlink_entry(upstream_pool_t *p, pooled_t *e)
{
    if (e->prev)
        e->prev->next = e->next;
    else
        p->head = e->next;
    if (e->next)
        e->next->prev = e->prev;
    p->n--;
}

static void drop_entry(upstream_pool_t *p, pooled_t *e)
{
    unlink_entry(p, e);
    ev_del(p->loop, e->fd);
    close(e->fd);
    free(e);
}

void pool_free(upstream_pool_t *p)
{
    if (!p)
        return;
    while (p->head)
        drop_entry(p, p->head);
    free(p);
}

/* Any readiness on a parked socket means the server closed it or sent
 * something unexpected; either way the connection is no longer reusable. */
static void on_idle_event(event_loop_t *loop, int fd, int events, void *arg)
{
    pooled_t *e = arg;
    LOG_DEBUG("pooled connection to %s closed by peer", e->server->label);
    drop_entry(e->pool, e);
}

int pool_take(upstream_pool_t *p, lb_server_t *s)
{
    for (pooled_t *e = p->head; e; e = e->next) {
        if (e->server != s)
            continue;
        int fd = e->fd;
        unlink_entry(p, e);
        ev_del(p->loop, fd);
        free(e);
        return fd;
    }
    return -1;
}

void pool_put(upstream_pool_t *p, lb_server_t *s, int fd, size_t max_per_server)
{
    size_t have = 0;
    for (pooled_t *e = p->head; e; e = e->next)
        have += e->server == s;
    if (have >= max_per_server) {
        close(fd);
        return;
    }
    pooled_t *e = xcalloc(1, sizeof(*e));
    e->fd = fd;
    e->server = s;
    e->pool = p;
    e->next = p->head;
    if (p->head)
        p->head->prev = e;
    p->head = e;
    p->n++;
    ev_add(p->loop, fd, EV_READ, on_idle_event, e);
}

size_t pool_size(const upstream_pool_t *p)
{
    return p->n;
}
