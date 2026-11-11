/*
 * health.c - active health checks (see health.h).
 *
 * Per server, a probe is a small state machine:
 *   CONNECTING --writable--> SENDING --sent--> READING --status line--> done
 * Each probe has a deadline; if it fires first the server is unhealthy.
 */
#include "lb/health.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "core/log.h"
#include "core/util.h"

#define PROBE_TIMEOUT_MS 2000

typedef struct probe {
    health_checker_t *hc;
    lb_server_t *server;
    int fd;
    enum { P_IDLE, P_CONNECTING, P_SENDING, P_READING } state;
    char req[256];
    size_t req_len, req_sent;
    char resp[64];
    size_t resp_len;
    timer_t_ deadline;
} probe_t;

struct health_checker {
    lb_group_t *group;
    event_loop_t *loop;
    probe_t *probes;
    timer_t_ tick;
};

static void probe_finish(probe_t *p, bool ok, const char *why)
{
    health_checker_t *hc = p->hc;
    if (p->fd >= 0) {
        ev_del(hc->loop, p->fd);
        close(p->fd);
        p->fd = -1;
    }
    timer_cancel(ev_timers(hc->loop), &p->deadline);
    p->state = P_IDLE;

    bool was_ok = atomic_load_explicit(&p->server->health_ok, memory_order_relaxed);
    if (ok != was_ok) {
        if (ok)
            LOG_INFO("health: %s is UP", p->server->label);
        else
            LOG_WARN("health: %s is DOWN (%s)", p->server->label, why);
    }
    atomic_store_explicit(&p->server->health_ok, ok, memory_order_relaxed);
}

static void on_probe_io(event_loop_t *loop, int fd, int events, void *arg)
{
    probe_t *p = arg;

    if (events & EV_ERROR) {
        probe_finish(p, false, "socket error");
        return;
    }

    if (p->state == P_CONNECTING) {
        /* Connect completion is reported as writability; check SO_ERROR. */
        int err = 0;
        socklen_t len = sizeof(err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
        if (err != 0) {
            probe_finish(p, false, strerror(err));
            return;
        }
        p->state = P_SENDING;
    }

    if (p->state == P_SENDING) {
        while (p->req_sent < p->req_len) {
            ssize_t n = write(fd, p->req + p->req_sent, p->req_len - p->req_sent);
            if (n < 0) {
                if (errno == EAGAIN || errno == EINTR)
                    return;
                probe_finish(p, false, "write failed");
                return;
            }
            p->req_sent += (size_t)n;
        }
        p->state = P_READING;
        ev_mod(loop, fd, EV_READ);
        return;
    }

    if (p->state == P_READING) {
        ssize_t n = read(fd, p->resp + p->resp_len, sizeof(p->resp) - 1 - p->resp_len);
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR)
                return;
            probe_finish(p, false, "read failed");
            return;
        }
        if (n == 0) {
            probe_finish(p, false, "closed before status line");
            return;
        }
        p->resp_len += (size_t)n;
        p->resp[p->resp_len] = '\0';
        /* We only need "HTTP/1.x NNN". */
        if (p->resp_len >= 12) {
            int status = atoi(p->resp + 9);
            bool ok = status >= 200 && status < 400;
            char why[32];
            snprintf(why, sizeof(why), "status %d", status);
            probe_finish(p, ok, why);
        } else if (p->resp_len >= sizeof(p->resp) - 1) {
            probe_finish(p, false, "garbage response");
        }
    }
}

static void on_probe_deadline(timer_t_ *t)
{
    probe_finish(t->arg, false, "timeout");
}

static void probe_start(probe_t *p)
{
    if (p->state != P_IDLE)
        return; /* previous probe still running */
    lb_server_t *s = p->server;

    int fd = socket(s->addr.ss_family, SOCK_STREAM, 0);
    if (fd < 0)
        return;
    fd_set_nonblocking(fd);
    fd_set_cloexec(fd);
    p->fd = fd;
    p->req_sent = 0;
    p->resp_len = 0;
    p->req_len = (size_t)snprintf(p->req, sizeof(p->req),
                                  "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: aproxy-health\r\n"
                                  "Connection: close\r\n\r\n",
                                  p->hc->group->cfg->health_path, s->cfg->host);

    int rc = connect(fd, (struct sockaddr *)&s->addr, s->addrlen);
    if (rc < 0 && errno != EINPROGRESS) {
        probe_finish(p, false, strerror(errno));
        return;
    }
    p->state = P_CONNECTING;
    ev_add(p->hc->loop, fd, EV_WRITE, on_probe_io, p);
    ev_timer_after(p->hc->loop, &p->deadline, PROBE_TIMEOUT_MS);
}

static void on_tick(timer_t_ *t)
{
    health_checker_t *hc = t->arg;
    for (size_t i = 0; i < hc->group->n; i++)
        probe_start(&hc->probes[i]);
    ev_timer_after(hc->loop, &hc->tick, hc->group->cfg->health_interval_ms);
}

health_checker_t *health_checker_start(lb_group_t *g, event_loop_t *loop)
{
    if (!g->cfg->health_check)
        return NULL;
    health_checker_t *hc = xcalloc(1, sizeof(*hc));
    hc->group = g;
    hc->loop = loop;
    hc->probes = xcalloc(g->n, sizeof(probe_t));
    for (size_t i = 0; i < g->n; i++) {
        probe_t *p = &hc->probes[i];
        p->hc = hc;
        p->server = &g->servers[i];
        p->fd = -1;
        timer_init(&p->deadline, on_probe_deadline, p);
    }
    timer_init(&hc->tick, on_tick, hc);
    /* First round right away so a dead backend is noticed at start-up. */
    ev_timer_after(loop, &hc->tick, 0);
    LOG_INFO("health checks for upstream '%s' every %llums on %s", g->cfg->name,
             (unsigned long long)g->cfg->health_interval_ms, g->cfg->health_path);
    return hc;
}

void health_checker_stop(health_checker_t *hc)
{
    if (!hc)
        return;
    timer_cancel(ev_timers(hc->loop), &hc->tick);
    for (size_t i = 0; i < hc->group->n; i++) {
        probe_t *p = &hc->probes[i];
        timer_cancel(ev_timers(hc->loop), &p->deadline);
        if (p->fd >= 0) {
            ev_del(hc->loop, p->fd);
            close(p->fd);
        }
    }
    free(hc->probes);
    free(hc);
}
