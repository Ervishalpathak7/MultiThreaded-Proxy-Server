/*
 * worker.c - worker thread body and the accept loop (see worker.h).
 */
#include "net/worker.h"

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "core/log.h"
#include "core/util.h"
#include "net/conn.h"
#include "net/listener.h"
#include "server.h"

/* Accept at most this many connections per readiness event so one worker
 * does not grab the whole backlog while the others sit idle. */
#define ACCEPT_BATCH 32

static void on_accept(event_loop_t *loop, int fd, int events, void *arg)
{
    worker_t *w = arg;
    const listener_t *l = NULL;
    for (size_t i = 0; i < w->srv->nlisteners; i++)
        if (w->srv->listeners[i].fd == fd)
            l = &w->srv->listeners[i];
    if (!l)
        return;

    for (int i = 0; i < ACCEPT_BATCH; i++) {
        struct sockaddr_storage ss;
        socklen_t sl = sizeof(ss);
        int cfd = accept(fd, (struct sockaddr *)&ss, &sl);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ECONNABORTED)
                return; /* another worker won the race, or the backlog is empty */
            if (errno == EINTR)
                continue;
            if (errno == EMFILE || errno == ENFILE) {
                LOG_WARN("accept: out of file descriptors");
                return;
            }
            LOG_ERROR("accept: %s", strerror(errno));
            return;
        }
        if (w->draining) {
            close(cfd);
            continue;
        }
        conn_t *c = conn_new(w, cfd, l, (struct sockaddr *)&ss, sl);
        LOG_DEBUG("worker %d accepted %s:%u on port %u", w->id, c->peer_ip, c->peer_port, l->port);
        server_on_accept(w->srv, c);
    }
}

static void *worker_main(void *arg)
{
    worker_t *w = arg;
    LOG_DEBUG("worker %d started", w->id);

    /* Every worker watches every listener. */
    for (size_t i = 0; i < w->srv->nlisteners; i++)
        ev_add(w->loop, w->srv->listeners[i].fd, EV_READ, on_accept, w);

    server_worker_init(w->srv, w);
    ev_run(w->loop);

    /* Loop stopped: tear down whatever is still connected. */
    while (w->conns)
        conn_close(w->conns);
    server_worker_cleanup(w->srv, w);

    LOG_DEBUG("worker %d exiting", w->id);
    return NULL;
}

int worker_start(worker_t *w, server_t *srv, int id)
{
    memset(w, 0, sizeof(*w));
    w->id = id;
    w->srv = srv;
    w->loop = ev_new();
    if (!w->loop)
        return -1;
    atomic_init(&w->running, true);

    /* Signals are handled by the main thread via sigwait; block them here
     * so a worker never gets interrupted by SIGINT/SIGTERM. */
    sigset_t set, old;
    sigfillset(&set);
    pthread_sigmask(SIG_BLOCK, &set, &old);
    int rc = pthread_create(&w->thread, NULL, worker_main, w);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    if (rc != 0) {
        LOG_ERROR("pthread_create: %s", strerror(rc));
        ev_free(w->loop);
        w->loop = NULL;
        return -1;
    }
    return 0;
}

void worker_stop(worker_t *w)
{
    if (w->loop)
        ev_stop(w->loop);
}

void worker_join(worker_t *w)
{
    if (!w->loop)
        return;
    pthread_join(w->thread, NULL);
    ev_free(w->loop);
    w->loop = NULL;
}

void worker_add_conn(worker_t *w, conn_t *c)
{
    c->prev = NULL;
    c->next = w->conns;
    if (w->conns)
        w->conns->prev = c;
    w->conns = c;
    w->nconns++;
}

void worker_remove_conn(worker_t *w, conn_t *c)
{
    if (c->prev)
        c->prev->next = c->next;
    else
        w->conns = c->next;
    if (c->next)
        c->next->prev = c->prev;
    c->prev = c->next = NULL;
    w->nconns--;
}
