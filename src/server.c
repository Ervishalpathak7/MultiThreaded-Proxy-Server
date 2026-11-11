/*
 * server.c - start-up, shutdown and the request pipeline (see server.h).
 */
#include "server.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "core/log.h"
#include "core/util.h"
#include "http/h1_server.h"
#include "http/stream.h"
#include "net/conn.h"
#include "proxy/pool.h"
#include "proxy/proxy.h"

/* ------------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------- */
server_t *server_new(config_t *cfg)
{
    server_t *srv = xcalloc(1, sizeof(*srv));
    srv->cfg = cfg;
    srv->started_at = wall_s();
    atomic_init(&srv->stopping, false);

    srv->listeners = xcalloc(cfg->nlistens ? cfg->nlistens : 1, sizeof(listener_t));
    for (size_t i = 0; i < cfg->nlistens; i++) {
        if (listener_open(&srv->listeners[i], &cfg->listens[i]) < 0) {
            server_free(srv);
            return NULL;
        }
        srv->nlisteners++;
    }

    /* Resolve upstream addresses now so a typo fails at start-up, not on
     * the first request. */
    srv->groups = xcalloc(cfg->nupstreams ? cfg->nupstreams : 1, sizeof(*srv->groups));
    srv->health = xcalloc(cfg->nupstreams ? cfg->nupstreams : 1, sizeof(*srv->health));
    for (size_t i = 0; i < cfg->nupstreams; i++) {
        srv->groups[i] = lb_group_new(&cfg->upstreams[i]);
        if (!srv->groups[i]) {
            server_free(srv);
            return NULL;
        }
        srv->ngroups++;
    }

    srv->nworkers = cfg->workers > 0 ? cfg->workers : (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (srv->nworkers < 1)
        srv->nworkers = 1;
    srv->workers = xcalloc((size_t)srv->nworkers, sizeof(worker_t));
    return srv;
}

void server_free(server_t *srv)
{
    if (!srv)
        return;
    for (size_t i = 0; i < srv->nlisteners; i++)
        listener_close(&srv->listeners[i]);
    free(srv->listeners);
    for (size_t i = 0; i < srv->ngroups; i++)
        lb_group_free(srv->groups[i]);
    free(srv->groups);
    free(srv->health);
    free(srv->workers);
    config_free(srv->cfg);
    free(srv);
}

static void noop_handler(int sig)
{
    (void)sig;
}

int server_run(server_t *srv)
{
    /* Signal plan: block the termination signals in every thread (workers
     * inherit the mask) and have the main thread wait for them with
     * sigwait(). No async-signal-safety headaches. */
    signal(SIGPIPE, SIG_IGN);
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGHUP);
    /* A signal whose disposition is SIG_IGN (inherited e.g. when started in
     * the background by a shell) is discarded before sigwait() can see it,
     * so give each one a real, if empty, handler. */
    struct sigaction sa = {.sa_handler = noop_handler};
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    for (int i = 0; i < srv->nworkers; i++) {
        if (worker_start(&srv->workers[i], srv, i) < 0) {
            LOG_ERROR("failed to start worker %d", i);
            srv->nworkers = i;
            break;
        }
    }
    LOG_INFO("aproxy %s ready: %d worker(s), %zu listener(s)", APROXY_VERSION, srv->nworkers,
             srv->nlisteners);

    int sig = 0;
    for (;;) {
        if (sigwait(&set, &sig) != 0)
            continue;
        if (sig == SIGHUP) {
            LOG_INFO("SIGHUP received (reload not supported; ignoring)");
            continue;
        }
        break;
    }
    LOG_INFO("signal %d received, shutting down", sig);
    atomic_store(&srv->stopping, true);

    for (int i = 0; i < srv->nworkers; i++)
        worker_stop(&srv->workers[i]);
    for (int i = 0; i < srv->nworkers; i++)
        worker_join(&srv->workers[i]);
    LOG_INFO("all workers stopped");
    return 0;
}

/* ------------------------------------------------------------------------
 * Hooks
 * ---------------------------------------------------------------------- */
void server_on_accept(server_t *srv, conn_t *c)
{
    /* Plaintext listeners speak HTTP/1.1 directly. */
    h1_conn_attach(c);
}

void server_worker_init(server_t *srv, worker_t *w)
{
    w->pool = pool_new(w->loop);
    /* Health checks run on exactly one loop; the results are shared
     * through atomics in lb_server_t. */
    if (w->id == 0)
        for (size_t i = 0; i < srv->ngroups; i++)
            srv->health[i] = health_checker_start(srv->groups[i], w->loop);
}

void server_worker_cleanup(server_t *srv, worker_t *w)
{
    if (w->id == 0)
        for (size_t i = 0; i < srv->ngroups; i++) {
            health_checker_stop(srv->health[i]);
            srv->health[i] = NULL;
        }
    pool_free(w->pool);
    w->pool = NULL;
}

/* ------------------------------------------------------------------------
 * Request pipeline
 * ---------------------------------------------------------------------- */
void server_stats_json(server_t *srv, buf_t *out)
{
#define LOADU(f) ((unsigned long long)atomic_load_explicit(&srv->stats.f, memory_order_relaxed))
    buf_printf(out,
               "{\n"
               "  \"version\": \"%s\",\n"
               "  \"uptime_s\": %llu,\n"
               "  \"workers\": %d,\n"
               "  \"connections\": {\"active\": %llu, \"total\": %llu},\n"
               "  \"requests\": %llu,\n"
               "  \"cache\": {\"hits\": %llu, \"misses\": %llu},\n"
               "  \"upstream\": {\"requests\": %llu, \"errors\": %llu},\n"
               "  \"bytes\": {\"in\": %llu, \"out\": %llu},\n"
               "  \"upstreams\": {\n",
               APROXY_VERSION, (unsigned long long)(wall_s() - srv->started_at), srv->nworkers,
               LOADU(active_conns), LOADU(total_conns), LOADU(requests), LOADU(cache_hits),
               LOADU(cache_misses), LOADU(upstream_requests), LOADU(upstream_errors),
               LOADU(bytes_in), LOADU(bytes_out));
#undef LOADU
    uint64_t now = now_ms();
    for (size_t i = 0; i < srv->ngroups; i++) {
        lb_group_stats_json(srv->groups[i], out, now);
        buf_append_str(out, i + 1 < srv->ngroups ? ",\n" : "\n");
    }
    buf_append_str(out, "  }\n}\n");
}

void server_dispatch(server_t *srv, http_stream_t *s)
{
    http_request_t *req = &s->req;
    size_t plen = http_request_path_len(req);

    /* Built-in status endpoint. */
    if (plen == 9 && memcmp(req->target, "/__status", 9) == 0) {
        s->resp.status = 200;
        headers_add_str(&s->resp.headers, "Content-Type", "application/json");
        headers_add_str(&s->resp.headers, "Cache-Control", "no-store");
        server_stats_json(srv, &s->resp.body);
        stream_finish(s);
        return;
    }

    config_route_t *route = config_match_route(srv->cfg, req->target, plen);
    if (!route) {
        stream_respond_error(s, 404, "no route matches this path");
        return;
    }

    lb_group_t *group = NULL;
    for (size_t i = 0; i < srv->ngroups; i++)
        if (strcmp(srv->groups[i]->cfg->name, route->upstream) == 0)
            group = srv->groups[i];
    if (!group) { /* cannot happen: config validation checks the reference */
        stream_respond_error(s, 500, "route points to unknown upstream");
        return;
    }

    proxy_forward(s, route, group, NULL, NULL);
}
