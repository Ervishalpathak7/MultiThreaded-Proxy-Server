#include "server.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char *argv[])
{
    if (argc != 2)
    {
        fprintf(stderr, "Usage: %s <port_number>\n", argv[0]);
        return EXIT_FAILURE;
    }

    char *endp = NULL;
    errno = 0;
    long port = strtol(argv[1], &endp, 10);
    if (errno != 0 || endp == argv[1] || *endp != '\0' || port < 1 || port > 65535)
    {
        fprintf(stderr, "Invalid port: %s (expected 1-65535)\n", argv[1]);
        return EXIT_FAILURE;
    }

    server_init((int)port);
    return 0;
}