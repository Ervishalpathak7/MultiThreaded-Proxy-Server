#include "server.h"
#include "global.h"
#include "limiter.h"
#include "request_handler.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

void server_init(int port_number)
{
    signal(SIGPIPE, SIG_IGN);   // a dead client must not kill the whole server
    limiter_init(MAX_CLIENTS);

    int proxy_socketId = socket(AF_INET, SOCK_STREAM, 0);
    if (proxy_socketId < 0)
    {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    int reuse = 1;
    if (setsockopt(proxy_socketId, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse) < 0)
    {
        perror("setsockopt");
        close(proxy_socketId);
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in proxy_addr;
    memset(&proxy_addr, 0, sizeof proxy_addr);
    proxy_addr.sin_family = AF_INET;
    proxy_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    proxy_addr.sin_port = htons((uint16_t)port_number);

    if (bind(proxy_socketId, (struct sockaddr *)&proxy_addr, sizeof proxy_addr) < 0)
    {
        perror("bind");
        close(proxy_socketId);
        exit(EXIT_FAILURE);
    }
    if (listen(proxy_socketId, SOMAXCONN) < 0)
    {
        perror("listen");
        close(proxy_socketId);
        exit(EXIT_FAILURE);
    }

    printf("Proxy listening on port %d\n", port_number);

    while (1)
    {
        // Limit BEFORE creating a thread, so threads never pile up.
        limiter_acquire();

        struct sockaddr_in client_addr;
        socklen_t client_addrlen = sizeof client_addr;   // reset every time: accept() overwrites it
        int fd = accept(proxy_socketId, (struct sockaddr *)&client_addr, &client_addrlen);
        if (fd < 0)
        {
            perror("accept");
            limiter_release();
            if (errno == EMFILE || errno == ENFILE)
                usleep(100000);   // out of fds: back off instead of spinning
            continue;
        }

        int *arg = malloc(sizeof *arg);   // each thread owns its own copy of the fd
        if (!arg)
        {
            close(fd);
            limiter_release();
            continue;
        }
        *arg = fd;

        pthread_t t;
        if (pthread_create(&t, NULL, request_handler, arg) != 0)
        {
            fprintf(stderr, "pthread_create failed\n");
            free(arg);
            close(fd);
            limiter_release();
            continue;
        }
        pthread_detach(t);   // nobody joins these threads
    }
}