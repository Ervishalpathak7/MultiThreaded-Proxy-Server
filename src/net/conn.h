/*
 * conn.h - a client TCP connection owned by one worker.
 *
 * conn_t handles the transport: non-blocking reads into `in`, buffered
 * writes from `out`, timeouts, and (later) TLS. It knows nothing about HTTP;
 * a protocol handler plugs in through conn_ops_t:
 *
 *     on_data     new bytes were appended to c->in
 *     on_drained  everything in c->out has been written to the socket
 *     on_timeout  the connection timer fired
 *     on_close    the connection is going away; release protocol state
 *
 * Lifetime: connections are reference counted. The socket itself holds one
 * reference; anything that may outlive a close (an in-flight upstream
 * request, an event dispatch in progress) takes its own. conn_close() tears
 * the socket down immediately but memory is released only by the last
 * conn_unref().
 */
#ifndef APROXY_NET_CONN_H
#define APROXY_NET_CONN_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/socket.h>

#include "core/buffer.h"
#include "core/timer_heap.h"

typedef struct conn conn_t;
typedef struct worker worker_t;
typedef struct listener listener_t;
typedef struct ssl_st SSL;

typedef struct conn_ops {
    void (*on_data)(conn_t *c);
    void (*on_drained)(conn_t *c);
    void (*on_timeout)(conn_t *c);
    void (*on_close)(conn_t *c);
} conn_ops_t;

struct conn {
    int fd;
    worker_t *w;
    const listener_t *listener;
    char peer_ip[64];
    uint16_t peer_port;

    buf_t in;
    buf_t out;

    const conn_ops_t *ops;
    void *proto; /* protocol private state (h1_conn_t, h2_conn_t, ...) */

    timer_t_ timer;
    int interest; /* EV_READ / EV_WRITE currently registered */
    bool read_enabled;
    bool closed;
    int refs;

    /* TLS (NULL on plaintext listeners). Managed by tls.c. */
    SSL *ssl;
    bool tls_handshaking;
    bool tls_want_write; /* SSL_read asked for a writable socket */

    /* Intrusive list of the worker's live connections. */
    conn_t *prev, *next;
};

conn_t *conn_new(worker_t *w, int fd, const listener_t *l, const struct sockaddr *addr,
                 socklen_t addrlen);

/* Install the protocol handler and begin reading. */
void conn_set_ops(conn_t *c, const conn_ops_t *ops, void *proto);
void conn_start(conn_t *c);

void conn_ref(conn_t *c);
void conn_unref(conn_t *c);

/* Close the socket and notify the protocol. Idempotent. After this returns
 * the caller must not touch `c` unless it holds its own reference. */
void conn_close(conn_t *c);

/* Enable or disable read interest (back-pressure). */
void conn_want_read(conn_t *c, bool enable);

/* Write as much of c->out as the socket accepts; arms write interest for
 * the rest. Calls ops->on_drained (possibly synchronously) once empty. */
void conn_flush(conn_t *c);

/* Arm / disarm the single per-connection timer. */
void conn_set_timeout(conn_t *c, uint64_t ms);
void conn_clear_timeout(conn_t *c);

/* Raw transport I/O; -1 with errno EAGAIN when it would block, 0 on EOF.
 * Dispatches to TLS when c->ssl is set. */
ssize_t conn_read_raw(conn_t *c, void *buf, size_t len);
ssize_t conn_write_raw(conn_t *c, const void *buf, size_t len);

/* Internal: recompute the event interest mask from the current state. */
void conn_update_interest(conn_t *c);

#endif /* APROXY_NET_CONN_H */
