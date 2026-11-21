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
#include "server.h"

// Print usage information for the command line interface
static void usage(const char *argv0)
{
    fprintf(stderr,
            "Usage: %s [-c config] [-t] [-v]\n"
            "  -c FILE   configuration file (default: config/proxy.conf)\n"
            "  -t        test the configuration and exit\n"
            "  -v        print version and exit\n",
            argv0);
}

// Print a summary of the loaded configuration
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

    // Parse command line arguments
    const char *config_path = "config/proxy.conf";

    // test_only flag indicates whether to only test the configuration and exit
    int test_only = 0;

    // Use getopt to parse command line options
    int opt;
    while ((opt = getopt(argc, argv, "c:tvh")) != -1) {
        switch (opt) {
        // 'c' option specifies the configuration file path
        case 'c':
            config_path = optarg;
            break;
        // 't' option indicates to test the configuration and exit
        case 't':
            test_only = 1;
            break;
        // 'v' option prints the version information and exits
        case 'v':
            printf("aproxy %s (http/2: %s, http/3: %s)\n", APROXY_VERSION, HAVE_HTTP2 ? "yes" : "no",
                   HAVE_HTTP3 ? "yes" : "no");
            return 0;
        // 'h' option prints the usage information and exits
        case 'h':
            usage(argv[0]);
            return 0;
        // Default case handles unknown options by printing usage information and exiting with an error code
        default:
            usage(argv[0]);
            return 2;
        }
    }

    // err buffer to hold error messages from config_load
    char err[512];

    // Load the configuration from the specified file path
    config_t *cfg = config_load(config_path, err, sizeof(err));

    // If the configuration fails to load, print the error message and exit with an error code
    if (!cfg) {
        fprintf(stderr, "%s: %s\n", config_path, err);
        return 1;
    }

    // Set the log level based on the loaded configuration
    log_set_level((log_level_t)cfg->log_level);

    // If the test_only flag is set, print a success message, the configuration summary, free the configuration, and exit
    if (test_only) {
        printf("%s: syntax ok\n", config_path);
        print_config_summary(cfg);
        config_free(cfg);
        return 0;
    }

    LOG_INFO("configuration loaded from %s", config_path);

    /* Create a new server instance using the loaded configuration. 
    * The server takes ownership of the configuration,so it will be responsible for freeing it when no longer needed.*/
    server_t *srv = server_new(cfg);

    // If the server fails to initialize, exit with an error code
    if (!srv)
        return 1;
    
    // Run the server and store the return code
    int rc = server_run(srv);
    server_free(srv);
    return rc;
}
