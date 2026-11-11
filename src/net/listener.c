/*
 * listener.c - socket setup (see listener.h).
 */
#include "net/listener.h"

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "core/log.h"
#include "core/util.h"

#define LISTEN_BACKLOG 1024

/* Resolve addr:port and create a bound socket of the given type. */
static int open_bound_socket(const char *addr, uint16_t port, int socktype)
{
    char portstr[8];
    snprintf(portstr, sizeof(portstr), "%u", port);
    struct addrinfo hints = {
        .ai_family = AF_UNSPEC,
        .ai_socktype = socktype,
        .ai_flags = AI_PASSIVE | AI_NUMERICSERV,
    };
    struct addrinfo *res;
    int rc = getaddrinfo(addr, portstr, &hints, &res);
    if (rc != 0) {
        LOG_ERROR("cannot resolve %s:%u: %s", addr, port, gai_strerror(rc));
        return -1;
    }

    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
        int one = 1;
        /* Allow immediate restart while old connections sit in TIME_WAIT. */
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (ai->ai_family == AF_INET6)
            setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof(one));
        if (bind(fd, ai->ai_addr, ai->ai_addrlen) == 0)
            break;
        LOG_ERROR("bind %s:%u: %s", addr, port, strerror(errno));
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0)
        return -1;
    fd_set_nonblocking(fd);
    fd_set_cloexec(fd);
    return fd;
}

int listener_open(listener_t *l, const config_listen_t *cfg)
{
    memset(l, 0, sizeof(*l));
    l->fd = -1;
    l->cfg = cfg;
    l->ssl = cfg->ssl;
    l->port = cfg->port;

    int fd = open_bound_socket(cfg->addr, cfg->port, SOCK_STREAM);
    if (fd < 0)
        return -1;
    if (listen(fd, LISTEN_BACKLOG) < 0) {
        LOG_ERROR("listen %s:%u: %s", cfg->addr, cfg->port, strerror(errno));
        close(fd);
        return -1;
    }
    l->fd = fd;
    LOG_INFO("listening on %s:%u%s", cfg->addr, cfg->port, cfg->ssl ? " (tls)" : "");
    return 0;
}

void listener_close(listener_t *l)
{
    if (l->fd >= 0)
        close(l->fd);
    l->fd = -1;
}

int udp_socket_open(const char *addr, uint16_t port)
{
    int fd = open_bound_socket(addr, port, SOCK_DGRAM);
    if (fd < 0)
        return -1;
    /* Large buffers: QUIC bursts many datagrams per RTT. */
    int sz = 4 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &sz, sizeof(sz));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sz, sizeof(sz));
    return fd;
}
