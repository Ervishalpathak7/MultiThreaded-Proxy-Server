/*
 * bench/origin.c - a minimal, fast HTTP/1.1 origin server for benchmarks.
 *
 * The demo backend (scripts/backend.py) is single-process Python and tops
 * out long before the proxy does, so proxied (cache-miss) numbers would only
 * measure Python. This server answers from a pre-built buffer with one thread
 * per connection, keep-alive and pipelining, so the proxy is the bottleneck.
 *
 *     cc -O2 -pthread bench/origin.c -o build/bench-origin
 *     ./build/bench-origin 9101 [name]
 *
 * Routes (any method; request bodies are read and discarded):
 *     /health       200 "ok", no-store
 *     /api/...      small body, Cache-Control: no-store   (always proxied)
 *     /bytes/N      N-byte body (N <= 4 MiB), cacheable
 *     anything else small body, Cache-Control: public, max-age=3600
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define MAX_BODY (4u << 20)
#define RBUF (64u << 10)

static const char *g_name = "origin";
static char *g_fill; /* MAX_BODY bytes of 'x' */

static int write_all(int fd, const char *p, size_t n)
{
    while (n) {
        ssize_t w = send(fd, p, n, 0);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

/* Case-insensitive search for a header value inside [h, end). */
static const char *find_header(const char *h, const char *end, const char *name, size_t *vlen)
{
    size_t nl = strlen(name);
    const char *p = memchr(h, '\n', (size_t)(end - h)); /* skip request line */
    while (p && p + 1 < end) {
        p++;
        if ((size_t)(end - p) > nl && strncasecmp(p, name, nl) == 0 && p[nl] == ':') {
            const char *v = p + nl + 1;
            while (v < end && (*v == ' ' || *v == '\t'))
                v++;
            const char *e = v;
            while (e < end && *e != '\r' && *e != '\n')
                e++;
            *vlen = (size_t)(e - v);
            return v;
        }
        p = memchr(p, '\n', (size_t)(end - p));
    }
    return NULL;
}

static int respond(int fd, const char *path, size_t plen, int is_head, int keep_alive)
{
    const char *cc = "public, max-age=3600";
    size_t blen = 0;
    char small[128];
    const char *body = small;

    if (plen == 7 && memcmp(path, "/health", 7) == 0) {
        body = "ok";
        blen = 2;
        cc = "no-store";
    } else if (plen >= 4 && memcmp(path, "/api", 4) == 0) {
        blen = (size_t)snprintf(small, sizeof small, "hello from %s (uncached)\n", g_name);
        cc = "no-store";
    } else if (plen > 7 && memcmp(path, "/bytes/", 7) == 0) {
        unsigned long n = strtoul(path + 7, NULL, 10);
        blen = n > MAX_BODY ? MAX_BODY : n;
        body = g_fill;
    } else {
        blen = (size_t)snprintf(small, sizeof small, "hello from %s\n", g_name);
    }

    char head[512];
    int hl = snprintf(head, sizeof head,
                      "HTTP/1.1 200 OK\r\n"
                      "Content-Type: text/plain\r\n"
                      "Content-Length: %zu\r\n"
                      "Cache-Control: %s\r\n"
                      "X-Backend: %s\r\n"
                      "%s"
                      "\r\n",
                      blen, cc, g_name, keep_alive ? "" : "Connection: close\r\n");
    if (write_all(fd, head, (size_t)hl) < 0)
        return -1;
    if (!is_head && blen && write_all(fd, body, blen) < 0)
        return -1;
    return 0;
}

static void *conn_main(void *arg)
{
    int fd = (int)(intptr_t)arg;
    char *buf = malloc(RBUF);
    size_t len = 0;
    if (!buf)
        goto out;

    for (;;) {
        /* Process every complete request already buffered (pipelining). */
        for (;;) {
            char *end = len >= 4 ? memmem(buf, len, "\r\n\r\n", 4) : NULL;
            if (!end)
                break;
            size_t hdr_len = (size_t)(end - buf) + 4;

            size_t vlen = 0, body_len = 0;
            const char *v = find_header(buf, end, "Content-Length", &vlen);
            if (v)
                body_len = strtoul(v, NULL, 10);
            if (find_header(buf, end, "Transfer-Encoding", &vlen))
                goto out; /* chunked uploads are not needed for benchmarks */
            if (hdr_len + body_len > RBUF)
                goto out;
            if (len < hdr_len + body_len)
                break; /* wait for the rest of the body */

            int keep_alive = 1;
            v = find_header(buf, end, "Connection", &vlen);
            if (v && vlen >= 5 && strncasecmp(v, "close", 5) == 0)
                keep_alive = 0;

            const char *sp1 = memchr(buf, ' ', hdr_len);
            const char *path = sp1 ? sp1 + 1 : "/";
            const char *sp2 = sp1 ? memchr(path, ' ', (size_t)(end - path)) : NULL;
            size_t plen = sp2 ? (size_t)(sp2 - path) : 1;
            const char *q = memchr(path, '?', plen);
            if (q)
                plen = (size_t)(q - path);
            int is_head = sp1 && (size_t)(sp1 - buf) == 4 && memcmp(buf, "HEAD", 4) == 0;

            if (respond(fd, path, plen, is_head, keep_alive) < 0 || !keep_alive)
                goto out;

            size_t used = hdr_len + body_len;
            memmove(buf, buf + used, len - used);
            len -= used;
        }
        if (len == RBUF)
            goto out; /* header too large */
        ssize_t r = recv(fd, buf + len, RBUF - len, 0);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            goto out;
        len += (size_t)r;
    }
out:
    free(buf);
    close(fd);
    return NULL;
}

int main(int argc, char **argv)
{
    int port = argc > 1 ? atoi(argv[1]) : 9101;
    if (argc > 2)
        g_name = argv[2];
    signal(SIGPIPE, SIG_IGN);

    g_fill = malloc(MAX_BODY);
    if (!g_fill)
        return 1;
    memset(g_fill, 'x', MAX_BODY);

    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(ls, (struct sockaddr *)&sa, sizeof sa) < 0 || listen(ls, 4096) < 0) {
        perror("bench-origin: bind/listen");
        return 1;
    }
    fprintf(stderr, "%s listening on 127.0.0.1:%d\n", g_name, port);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 256u << 10);

    for (;;) {
        int fd = accept(ls, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR || errno == ECONNABORTED || errno == EMFILE)
                continue;
            perror("accept");
            continue;
        }
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        pthread_t t;
        if (pthread_create(&t, &attr, conn_main, (void *)(intptr_t)fd) != 0)
            close(fd);
    }
}
