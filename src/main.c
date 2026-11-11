/*
 * main.c - command line entry point.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *argv0)
{
    fprintf(stderr,
            "Usage: %s [-c config] [-t] [-v]\n"
            "  -c FILE   configuration file (default: config/proxy.conf)\n"
            "  -t        test the configuration and exit\n"
            "  -v        print version and exit\n",
            argv0);
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) {
            printf("aproxy %s\n", APROXY_VERSION);
            return 0;
        }
        if (strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        }
    }
    usage(argv[0]);
    return 1;
}
