/*
 * lb.c - load balancing strategies (see lb.h).
 */
#include "lb/lb.h"

#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/log.h"
#include "core/util.h"

#define RING_POINTS_PER_WEIGHT 160

/* ------------------------------------------------------------------------
 * Construction
 * ---------------------------------------------------------------------- */
static int resolve(const config_server_t *cfg, struct sockaddr_storage *out, socklen_t *outlen)
{
    char port[8];
    snprintf(port, sizeof(port), "%u", cfg->port);
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM};
    struct addrinfo *res;
    int rc = getaddrinfo(cfg->host, port, &hints, &res);
    if (rc != 0) {
        LOG_ERROR("cannot resolve upstream %s:%u: %s", cfg->host, cfg->port, gai_strerror(rc));
        return -1;
    }
    /* Prefer IPv4 for predictability on dual-stack hosts. */
    struct addrinfo *pick = res;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next)
        if (ai->ai_family == AF_INET) {
            pick = ai;
            break;
        }
    memcpy(out, pick->ai_addr, pick->ai_addrlen);
    *outlen = pick->ai_addrlen;
    freeaddrinfo(res);
    return 0;
}

static int ring_cmp(const void *a, const void *b)
{
    const struct ring_point *x = a, *y = b;
    return x->hash < y->hash ? -1 : x->hash > y->hash;
}

/* Build the ketama ring: each server gets weight*160 pseudo-random points
 * so the key space is split proportionally to weight. */
static void build_ring(lb_group_t *g)
{
    size_t total = 0;
    for (size_t i = 0; i < g->n; i++)
        total += (size_t)g->servers[i].cfg->weight * RING_POINTS_PER_WEIGHT;
    g->ring = xmalloc(total * sizeof(*g->ring));
    g->ring_len = 0;
    for (size_t i = 0; i < g->n; i++) {
        size_t points = (size_t)g->servers[i].cfg->weight * RING_POINTS_PER_WEIGHT;
        for (size_t p = 0; p < points; p++) {
            char key[128];
            int n = snprintf(key, sizeof(key), "%s#%zu", g->servers[i].label, p);
            g->ring[g->ring_len].hash = hash_mix64(hash_fnv1a(key, (size_t)n));
            g->ring[g->ring_len].server = (uint32_t)i;
            g->ring_len++;
        }
    }
    qsort(g->ring, g->ring_len, sizeof(*g->ring), ring_cmp);
}

lb_group_t *lb_group_new(const config_upstream_t *cfg)
{
    lb_group_t *g = xcalloc(1, sizeof(*g));
    g->cfg = cfg;
    g->strategy = cfg->strategy;
    g->n = cfg->nservers;
    g->servers = xcalloc(g->n, sizeof(lb_server_t));
    pthread_mutex_init(&g->lock, NULL);
    atomic_init(&g->rr_next, 0);

    for (size_t i = 0; i < g->n; i++) {
        lb_server_t *s = &g->servers[i];
        s->cfg = &cfg->servers[i];
        snprintf(s->label, sizeof(s->label), "%s:%u", s->cfg->host, s->cfg->port);
        if (resolve(s->cfg, &s->addr, &s->addrlen) < 0) {
            lb_group_free(g);
            return NULL;
        }
        atomic_init(&s->active, 0);
        atomic_init(&s->fails, 0);
        atomic_init(&s->down_until_ms, 0);
        atomic_init(&s->health_ok, true);
        atomic_init(&s->total_requests, 0);
        atomic_init(&s->total_failures, 0);
    }
    if (g->strategy == LB_IP_HASH)
        build_ring(g);
    return g;
}

void lb_group_free(lb_group_t *g)
{
    if (!g)
        return;
    pthread_mutex_destroy(&g->lock);
    free(g->ring);
    free(g->servers);
    free(g);
}

/* ------------------------------------------------------------------------
 * Availability and health bookkeeping
 * ---------------------------------------------------------------------- */
bool lb_server_available(const lb_server_t *s, uint64_t now_ms)
{
    if (!atomic_load_explicit(&s->health_ok, memory_order_relaxed))
        return false;
    uint64_t down_until = atomic_load_explicit(&s->down_until_ms, memory_order_relaxed);
    return down_until == 0 || now_ms >= down_until;
}

void lb_server_begin(lb_server_t *s)
{
    atomic_fetch_add_explicit(&s->active, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&s->total_requests, 1, memory_order_relaxed);
}

void lb_server_end(lb_server_t *s)
{
    atomic_fetch_sub_explicit(&s->active, 1, memory_order_relaxed);
}

void lb_server_success(lb_server_t *s)
{
    atomic_store_explicit(&s->fails, 0, memory_order_relaxed);
    /* A success after fail_timeout expired brings the server fully back. */
    atomic_store_explicit(&s->down_until_ms, 0, memory_order_relaxed);
}

void lb_server_failure(lb_server_t *s, uint64_t now_ms)
{
    atomic_fetch_add_explicit(&s->total_failures, 1, memory_order_relaxed);
    int fails = atomic_fetch_add_explicit(&s->fails, 1, memory_order_relaxed) + 1;
    if (s->cfg->max_fails > 0 && fails >= s->cfg->max_fails) {
        atomic_store_explicit(&s->down_until_ms, now_ms + s->cfg->fail_timeout_ms,
                              memory_order_relaxed);
        atomic_store_explicit(&s->fails, 0, memory_order_relaxed);
        LOG_WARN("upstream %s marked down for %llums after %d failures", s->label,
                 (unsigned long long)s->cfg->fail_timeout_ms, fails);
    }
}

/* ------------------------------------------------------------------------
 * Strategies
 * ---------------------------------------------------------------------- */
static lb_server_t *pick_round_robin(lb_group_t *g, uint64_t now)
{
    unsigned start = atomic_fetch_add_explicit(&g->rr_next, 1, memory_order_relaxed);
    for (size_t i = 0; i < g->n; i++) {
        lb_server_t *s = &g->servers[(start + i) % g->n];
        if (lb_server_available(s, now))
            return s;
    }
    return NULL;
}

/* Smooth weighted round robin (the nginx algorithm):
 *   every server: current += weight; total += weight
 *   pick the max current, then picked.current -= total */
static lb_server_t *pick_weighted(lb_group_t *g, uint64_t now)
{
    lb_server_t *best = NULL;
    int total = 0;
    pthread_mutex_lock(&g->lock);
    for (size_t i = 0; i < g->n; i++) {
        lb_server_t *s = &g->servers[i];
        if (!lb_server_available(s, now))
            continue;
        s->current_weight += s->cfg->weight;
        total += s->cfg->weight;
        if (!best || s->current_weight > best->current_weight)
            best = s;
    }
    if (best)
        best->current_weight -= total;
    pthread_mutex_unlock(&g->lock);
    return best;
}

/* Fewest active requests, scaled by weight: compare active_a/weight_a with
 * active_b/weight_b using cross multiplication to stay in integers. */
static lb_server_t *pick_least_conn(lb_group_t *g, uint64_t now)
{
    lb_server_t *best = NULL;
    int best_active = 0;
    /* Start at a rotating offset so ties do not always go to server 0. */
    unsigned start = atomic_fetch_add_explicit(&g->rr_next, 1, memory_order_relaxed);
    for (size_t i = 0; i < g->n; i++) {
        lb_server_t *s = &g->servers[(start + i) % g->n];
        if (!lb_server_available(s, now))
            continue;
        int active = atomic_load_explicit(&s->active, memory_order_relaxed);
        if (!best || (long)active * best->cfg->weight < (long)best_active * s->cfg->weight) {
            best = s;
            best_active = active;
        }
    }
    return best;
}

static lb_server_t *pick_ip_hash(lb_group_t *g, const char *key, size_t keylen, uint64_t now)
{
    if (g->ring_len == 0)
        return NULL;
    uint64_t h = hash_mix64(hash_fnv1a(key, keylen));
    /* Binary search for the first ring point >= h (wrapping around). */
    size_t lo = 0, hi = g->ring_len;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (g->ring[mid].hash < h)
            lo = mid + 1;
        else
            hi = mid;
    }
    /* Walk clockwise until we find an available server. */
    for (size_t i = 0; i < g->ring_len; i++) {
        lb_server_t *s = &g->servers[g->ring[(lo + i) % g->ring_len].server];
        if (lb_server_available(s, now))
            return s;
    }
    return NULL;
}

lb_server_t *lb_pick(lb_group_t *g, const char *key, size_t keylen, uint64_t now_ms)
{
    switch (g->strategy) {
    case LB_ROUND_ROBIN:
        return pick_round_robin(g, now_ms);
    case LB_WEIGHTED:
        return pick_weighted(g, now_ms);
    case LB_LEAST_CONN:
        return pick_least_conn(g, now_ms);
    case LB_IP_HASH:
        return pick_ip_hash(g, key, keylen, now_ms);
    }
    return NULL;
}

/* ------------------------------------------------------------------------
 * Stats
 * ---------------------------------------------------------------------- */
void lb_group_stats_json(lb_group_t *g, buf_t *out, uint64_t now_ms)
{
    buf_printf(out, "    \"%s\": {\"strategy\": \"%s\", \"servers\": [\n", g->cfg->name,
               lb_strategy_name(g->strategy));
    for (size_t i = 0; i < g->n; i++) {
        lb_server_t *s = &g->servers[i];
        buf_printf(out,
                   "      {\"addr\": \"%s\", \"weight\": %d, \"up\": %s, \"active\": %d, "
                   "\"requests\": %llu, \"failures\": %llu}%s\n",
                   s->label, s->cfg->weight, lb_server_available(s, now_ms) ? "true" : "false",
                   atomic_load_explicit(&s->active, memory_order_relaxed),
                   (unsigned long long)atomic_load_explicit(&s->total_requests, memory_order_relaxed),
                   (unsigned long long)atomic_load_explicit(&s->total_failures, memory_order_relaxed),
                   i + 1 < g->n ? "," : "");
    }
    buf_append_str(out, "    ]}");
}
