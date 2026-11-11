/*
 * listener.h - listening sockets.
 *
 * TCP listeners are opened once by the main thread and shared by every
 * worker: each worker registers the same fd in its own event loop and calls
 * accept() when it becomes readable. Workers that lose the race simply get
 * EAGAIN. This keeps the design portable (no SO_REUSEPORT semantics to
 * worry about across kernels) and is how nginx operates by default.
 */
#ifndef APROXY_NET_LISTENER_H
#define APROXY_NET_LISTENER_H

#include <stdbool.h>
#include <stdint.h>

#include "config/config.h"

typedef struct listener {
    int fd;
    bool ssl;
    uint16_t port;
    const config_listen_t *cfg;
} listener_t;

/* Bind + listen on cfg->addr:cfg->port. Returns 0 or -1 (logged). */
int listener_open(listener_t *l, const config_listen_t *cfg);
void listener_close(listener_t *l);

/* Open a bound, non-blocking UDP socket (used by the QUIC frontend). */
int udp_socket_open(const char *addr, uint16_t port);

#endif /* APROXY_NET_LISTENER_H */
