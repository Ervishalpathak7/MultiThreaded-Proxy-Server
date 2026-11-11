#include "config/config.h"

#include "core/log.h"
#include "test.h"

static const char *FULL =
    "# comment line\n"
    "workers 4;\n"
    "log_level debug;\n"
    "listen 8080;\n"
    "listen 127.0.0.1:8443 ssl;\n"
    "http3 8443;\n"
    "ssl_certificate     certs/server.crt;\n"
    "ssl_certificate_key certs/server.key;\n"
    "tls_host example.com {\n"
    "    ssl_certificate certs/example.crt;\n"
    "    ssl_certificate_key certs/example.key;\n"
    "}\n"
    "max_body_size 2m;\n"
    "idle_timeout 30s;\n"
    "\n"
    "upstream backend {\n"
    "    strategy least_conn;\n"
    "    server 127.0.0.1:9001 weight=3 max_fails=5 fail_timeout=20s;\n"
    "    server 127.0.0.1:9002;  # trailing comment\n"
    "    health_check interval=2s path=/health;\n"
    "    connect_timeout 500ms;\n"
    "    keepalive 8;\n"
    "}\n"
    "upstream api { server \"10.0.0.1:80\"; }\n"
    "cache { max_size 256m; default_ttl 90s; shards 8; }\n"
    "route / { proxy_pass backend; cache on; }\n"
    "route /api { proxy_pass api; cache off; cache_ttl 5s; }\n";

TEST(parses_full_config)
{
    char err[256];
    config_t *c = config_parse(FULL, err, sizeof(err));
    if (!c) {
        TEST_FAIL("parse failed: %s", err);
        return;
    }
    ASSERT_EQ(c->workers, 4);
    ASSERT_EQ(c->log_level, LOG_LEVEL_DEBUG);
    ASSERT_EQ(c->nlistens, 2);
    ASSERT_EQ(c->listens[0].port, 8080);
    ASSERT(!c->listens[0].ssl);
    ASSERT_STR_EQ(c->listens[0].addr, "0.0.0.0");
    ASSERT_EQ(c->listens[1].port, 8443);
    ASSERT(c->listens[1].ssl);
    ASSERT_STR_EQ(c->listens[1].addr, "127.0.0.1");
    ASSERT_EQ(c->http3_port, 8443);

    ASSERT_EQ(c->ncerts, 2);
    ASSERT(c->certs[0].hostname == NULL);
    ASSERT_STR_EQ(c->certs[0].cert_file, "certs/server.crt");
    ASSERT_STR_EQ(c->certs[0].key_file, "certs/server.key");
    ASSERT_STR_EQ(c->certs[1].hostname, "example.com");
    ASSERT_STR_EQ(c->certs[1].key_file, "certs/example.key");

    ASSERT_EQ(c->max_body_size, 2 * 1024 * 1024);
    ASSERT_EQ(c->idle_timeout_ms, 30000);
    ASSERT_EQ(c->header_timeout_ms, 10000); /* default */

    ASSERT_EQ(c->nupstreams, 2);
    config_upstream_t *u = config_find_upstream(c, "backend");
    ASSERT(u != NULL);
    ASSERT_EQ(u->strategy, LB_LEAST_CONN);
    ASSERT_EQ(u->nservers, 2);
    ASSERT_STR_EQ(u->servers[0].host, "127.0.0.1");
    ASSERT_EQ(u->servers[0].port, 9001);
    ASSERT_EQ(u->servers[0].weight, 3);
    ASSERT_EQ(u->servers[0].max_fails, 5);
    ASSERT_EQ(u->servers[0].fail_timeout_ms, 20000);
    ASSERT_EQ(u->servers[1].weight, 1);
    ASSERT(u->health_check);
    ASSERT_EQ(u->health_interval_ms, 2000);
    ASSERT_STR_EQ(u->health_path, "/health");
    ASSERT_EQ(u->connect_timeout_ms, 500);
    ASSERT_EQ(u->keepalive, 8);

    config_upstream_t *api = config_find_upstream(c, "api");
    ASSERT(api != NULL);
    ASSERT_EQ(api->strategy, LB_ROUND_ROBIN);
    ASSERT_STR_EQ(api->servers[0].host, "10.0.0.1");

    ASSERT_EQ(c->cache_max_size, 256 * 1024 * 1024);
    ASSERT_EQ(c->cache_default_ttl_ms, 90000);
    ASSERT_EQ(c->cache_shards, 8);

    ASSERT_EQ(c->nroutes, 2);
    ASSERT(c->routes[0].cache);
    ASSERT(!c->routes[1].cache);
    ASSERT_EQ(c->routes[1].cache_ttl_ms, 5000);
    config_free(c);
}

TEST(longest_prefix_route_wins)
{
    char err[256];
    config_t *c = config_parse(FULL, err, sizeof(err));
    ASSERT(c != NULL);
    if (!c)
        return;
    config_route_t *r = config_match_route(c, "/api/users", 10);
    ASSERT(r && strcmp(r->upstream, "api") == 0);
    r = config_match_route(c, "/index.html", 11);
    ASSERT(r && strcmp(r->upstream, "backend") == 0);
    r = config_match_route(c, "/ap", 3);
    ASSERT(r && strcmp(r->upstream, "backend") == 0);
    config_free(c);
}

static void expect_error(const char *text, const char *needle)
{
    char err[256];
    config_t *c = config_parse(text, err, sizeof(err));
    if (c) {
        TEST_FAIL("expected error containing '%s' but parse succeeded", needle);
        config_free(c);
        return;
    }
    if (!strstr(err, needle))
        TEST_FAIL("error '%s' does not contain '%s'", err, needle);
}

TEST(rejects_bad_configs)
{
    expect_error("listen 8080;\n", "no 'route'");
    expect_error("listen 8080;\nroute / { proxy_pass nope; }\n", "unknown upstream 'nope'");
    expect_error("listen 8080;\nupstream b { server 127.0.0.1:9001; }\n"
                 "route / { proxy_pass b }\n",
                 "line 3");
    expect_error("listen 8443 ssl;\nupstream b { server 127.0.0.1:9001; }\nroute / { proxy_pass b; }\n",
                 "ssl_certificate");
    expect_error("listen 8080;\nupstream b { server 127.0.0.1:99999; }\nroute / { proxy_pass b; }\n",
                 "bad port");
    expect_error("listen 8080;\nupstream b { strategy magic; server 127.0.0.1:1; }\n"
                 "route / { proxy_pass b; }\n",
                 "unknown strategy");
    expect_error("listen 8080;\nupstream b { }\nroute / { proxy_pass b; }\n", "no servers");
    expect_error("listen 8080;\nbogus 1;\nupstream b { server 127.0.0.1:1; }\nroute / { proxy_pass b; }\n",
                 "unknown directive 'bogus'");
    expect_error("listen 8080;\nupstream b { server 127.0.0.1:1; }\nroute / { proxy_pass b; }\n}\n",
                 "unexpected '}'");
}

TEST(defaults_apply)
{
    char err[256];
    config_t *c = config_parse("listen 80;\nupstream b { server h:1; }\nroute / { proxy_pass b; }\n",
                               err, sizeof(err));
    ASSERT(c != NULL);
    if (!c)
        return;
    ASSERT_EQ(c->workers, 0);
    ASSERT_EQ(c->cache_shards, 16);
    ASSERT_EQ(c->cache_default_ttl_ms, 60000);
    ASSERT_EQ(c->upstreams[0].keepalive, 32);
    ASSERT_EQ(c->upstreams[0].servers[0].max_fails, 3);
    ASSERT(!c->upstreams[0].health_check);
    config_free(c);
}

int main(void)
{
    RUN(parses_full_config);
    RUN(longest_prefix_route_wins);
    RUN(rejects_bad_configs);
    RUN(defaults_apply);
    return TEST_RESULT();
}
