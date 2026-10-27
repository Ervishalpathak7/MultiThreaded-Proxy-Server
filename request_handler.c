#include "request_handler.h"
#include "global.h"
#include "limiter.h"
#include "request_parser.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

// send() may write only part of the data, so loop.
static int send_all(int fd, const char *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len)
    {
        ssize_t n = send(fd, buf + sent, len - sent, 0);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return -1; // EPIPE etc. (SIGPIPE is ignored in server_init)
        }
        sent += (size_t)n;
    }
    return 0;
}

static void send_error(int fd, int status, const char *text)
{
    char resp[128];
    int n = snprintf(resp, sizeof resp,
                     "HTTP/1.1 %d %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
                     status, text);
    if (n > 0)
        send_all(fd, resp, (size_t)n);
}

// Reads until the end of the headers. Returns total bytes in buf,
// -1 on error/close/timeout, -2 if the head doesn't fit in cap.
static ssize_t read_head(int fd, char *buf, size_t cap, size_t *head_len)
{
    size_t total = 0;
    while (total < cap - 1)
    {
        ssize_t n = recv(fd, buf + total, cap - 1 - total, 0);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1; // client closed before finishing the head
        total += (size_t)n;

        ssize_t end = find_header_end(buf, total);
        if (end >= 0)
        {
            *head_len = (size_t)end;
            return (ssize_t)total;
        }
    }
    return -2;
}

void *request_handler(void *arg)
{
    // extract cliend socket to. local variable
    int client_socket = *(int *)arg;
    // free heap memory
    free(arg);

    // variables
    char *buffer = NULL;
    struct ParsedRequest *request = NULL;
    size_t total = 0, head_len = 0, body_needed = 0;
    ssize_t r;
    const char *cl;

    // timeinterval to read a request
    // Don't let a silent client hold a slot forever.
    struct timeval tv = {.tv_sec = RECV_TIMEOUT, .tv_usec = 0};
    setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    buffer = malloc(MAX_BYTES);
    request = ParsedRequest_create();
    if (!buffer || !request)
    {
        fprintf(stderr, "out of memory\n");
        goto cleanup;
    }

    r = read_head(client_socket, buffer, MAX_BYTES, &head_len);
    if (r == -2)
    {
        send_error(client_socket, 431, "Request Header Fields Too Large");
        goto cleanup;
    }
    if (r < 0)
        goto cleanup;
    total = (size_t)r;

    if (ParsedRequest_parse(request, buffer, head_len) != 0)
    {
        send_error(client_socket, 400, "Bad Request");
        goto cleanup;
    }

    // ---- body: read exactly Content-Length bytes (already-read bytes count) ----
    cl = ParsedRequest_get_header(request, "Content-Length");
    if (cl)
    {
        char *endp = NULL;
        errno = 0;
        long v = strtol(cl, &endp, 10);
        if (errno != 0 || endp == cl || *endp != '\0' || v < 0)
        {
            send_error(client_socket, 400, "Bad Request");
            goto cleanup;
        }
        if ((size_t)v > MAX_BYTES - 1 - head_len)
        {
            send_error(client_socket, 413, "Payload Too Large");
            goto cleanup;
        }
        body_needed = (size_t)v;
    }

    while (total - head_len < body_needed)
    {
        ssize_t n = recv(client_socket, buffer + total, MAX_BYTES - 1 - total, 0);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            goto cleanup;
        }
        if (n == 0)
            goto cleanup;
        total += (size_t)n;
    }

    // ---- temporary response (next milestone: forward to an upstream server) ----
    {
        const char *response =
            "HTTP/1.1 200 OK\r\nContent-Length: 13\r\nConnection: close\r\n\r\nHello, World!";
        send_all(client_socket, response, strlen(response));
    }

cleanup: // single exit path: nothing can leak
    ParsedRequest_destroy(request);
    free(buffer);
    close(client_socket);
    limiter_release();
    return NULL;
}