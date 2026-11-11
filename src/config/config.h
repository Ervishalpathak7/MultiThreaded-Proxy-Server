/*
 * config.h - configuration model and parser.
 *
 * The file format is deliberately nginx-like: `directive args;` and
 * `block name { ... }`. Example:
 *
 *     workers 4;
 *     listen 8080;
 *     listen 8443 ssl;
 *     http3 8443;
 *     ssl_certificate     certs/server.crt;
 *     ssl_certificate_key certs/server.key;
 *
 *     upstream backend {
 *         strategy least_conn;
 *         server 127.0.0.1:9001 weight=3;
 *         server 127.0.0.1:9002;
 *         health_check interval=5s path=/health;
 *     }
 *
 *     cache { max_size 256m; default_ttl 60s; }
 *
 *     route / { proxy_pass backend; cache on; }
 *
 * Parsing produces an immutable config_t that the server reads from every
 * thread without locking.
 */
#ifndef APROXY_CONFIG_H
#define APROXY_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    LB_ROUND_ROBIN = 0,
    LB_WEIGHTED, /* smooth weighted round robin */
    LB_LEAST_CONN,
    LB_IP_HASH, /* consistent hashing on the client address */
} lb_strategy_t;

typedef struct config_server {
    char *host;
    uint16_t port;
    int weight;               /* default 1 */
    int max_fails;            /* passive health: failures before marking down */
    uint64_t fail_timeout_ms; /* how long a server stays down */
} config_server_t;

typedef struct config_upstream {
    char *name;
    lb_strategy_t strategy;
    config_server_t *servers;
    size_t nservers;

    bool health_check;
    uint64_t health_interval_ms;
    char *health_path;

    uint64_t connect_timeout_ms;
    uint64_t read_timeout_ms;
    size_t keepalive; /* idle upstream connections kept per worker */
} config_upstream_t;

typedef struct config_route {
    char *prefix;   /* path prefix; longest match wins */
    char *upstream; /* name of the upstream block */
    bool cache;
    uint64_t cache_ttl_ms; /* 0 = use cache.default_ttl */
} config_route_t;

typedef struct config_listen {
    char *addr; /* bind address, default "0.0.0.0" */
    uint16_t port;
    bool ssl;
} config_listen_t;

typedef struct config_cert {
    char *hostname; /* NULL for the default certificate */
    char *cert_file;
    char *key_file;
} config_cert_t;

typedef struct config {
    int workers;   /* 0 = number of CPUs */
    int log_level; /* log_level_t */

    config_listen_t *listens;
    size_t nlistens;
    uint16_t http3_port; /* 0 = HTTP/3 disabled */
    char *http3_addr;

    config_cert_t *certs;
    size_t ncerts;

    /* HTTP limits and timeouts */
    size_t max_header_size;
    size_t max_body_size;
    uint64_t header_timeout_ms;
    uint64_t idle_timeout_ms;

    /* cache { } */
    size_t cache_max_size;
    uint64_t cache_default_ttl_ms;
    int cache_shards;

    config_upstream_t *upstreams;
    size_t nupstreams;
    config_route_t *routes;
    size_t nroutes;
} config_t;

/* Parse a file or an in-memory string. On failure NULL is returned and a
 * human readable message (with line number) is written to err. */
config_t *config_load(const char *path, char *err, size_t errlen);
config_t *config_parse(const char *text, char *err, size_t errlen);
void config_free(config_t *cfg);

config_upstream_t *config_find_upstream(const config_t *cfg, const char *name);
/* Longest-prefix match of a request path; NULL if nothing matches. */
config_route_t *config_match_route(const config_t *cfg, const char *path, size_t len);

const char *lb_strategy_name(lb_strategy_t s);

#endif /* APROXY_CONFIG_H */
