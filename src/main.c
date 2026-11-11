/*
 * main.c - command line entry point.
 *
 *   proxy -c config/proxy.conf     run the server
 *   proxy -t -c config/proxy.conf  validate the config and exit
 *   proxy -v                       print the version
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config/config.h"
#include "core/log.h"

static void usage(const char *argv0)
{
    fprintf(stderr,
            "Usage: %s [-c config] [-t] [-v]\n"
            "  -c FILE   configuration file (default: config/proxy.conf)\n"
            "  -t        test the configuration and exit\n"
            "  -v        print version and exit\n",
            argv0);
}

static void print_config_summary(const config_t *cfg)
{
    for (size_t i = 0; i < cfg->nlistens; i++)
        printf("  listen %s:%u%s\n", cfg->listens[i].addr, cfg->listens[i].port,
               cfg->listens[i].ssl ? " (tls)" : "");
    if (cfg->http3_port)
        printf("  http3  %s:%u (quic)\n", cfg->http3_addr, cfg->http3_port);
    for (size_t i = 0; i < cfg->nupstreams; i++) {
        const config_upstream_t *u = &cfg->upstreams[i];
        printf("  upstream %s [%s] %zu server(s)%s\n", u->name, lb_strategy_name(u->strategy),
               u->nservers, u->health_check ? ", health checks on" : "");
    }
    for (size_t i = 0; i < cfg->nroutes; i++)
        printf("  route %s -> %s%s\n", cfg->routes[i].prefix, cfg->routes[i].upstream,
               cfg->routes[i].cache ? " (cached)" : "");
}

int main(int argc, char **argv)
{
    const char *config_path = "config/proxy.conf";
    int test_only = 0;
    int opt;
    while ((opt = getopt(argc, argv, "c:tvh")) != -1) {
        switch (opt) {
        case 'c':
            config_path = optarg;
            break;
        case 't':
            test_only = 1;
            break;
        case 'v':
            printf("aproxy %s (http/2: %s, http/3: %s)\n", APROXY_VERSION, HAVE_HTTP2 ? "yes" : "no",
                   HAVE_HTTP3 ? "yes" : "no");
            return 0;
        case 'h':
            usage(argv[0]);
            return 0;
        default:
            usage(argv[0]);
            return 2;
        }
    }

    char err[512];
    config_t *cfg = config_load(config_path, err, sizeof(err));
    if (!cfg) {
        fprintf(stderr, "%s: %s\n", config_path, err);
        return 1;
    }
    log_set_level((log_level_t)cfg->log_level);

    if (test_only) {
        printf("%s: syntax ok\n", config_path);
        print_config_summary(cfg);
        config_free(cfg);
        return 0;
    }

    /* Server start-up is added in the next step. */
    LOG_INFO("configuration loaded from %s", config_path);
    config_free(cfg);
    return 0;
}
