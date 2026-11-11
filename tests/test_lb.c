#include "lb/lb.h"

#include "test.h"

/* Build a group from a config snippet; servers point at loopback so
 * resolution never touches DNS. */
static config_t *make_cfg(const char *upstream_body)
{
    char text[1024];
    snprintf(text, sizeof(text),
             "listen 1;\nupstream u {\n%s\n}\nroute / { proxy_pass u; }\n", upstream_body);
    char err[256];
    config_t *c = config_parse(text, err, sizeof(err));
    if (!c)
        printf("config error: %s\n", err);
    return c;
}

TEST(round_robin_cycles)
{
    config_t *c = make_cfg("server 127.0.0.1:1; server 127.0.0.1:2; server 127.0.0.1:3;");
    lb_group_t *g = lb_group_new(&c->upstreams[0]);
    ASSERT(g != NULL);
    int counts[3] = {0};
    for (int i = 0; i < 30; i++) {
        lb_server_t *s = lb_pick(g, "k", 1, 0);
        counts[s - g->servers]++;
    }
    ASSERT_EQ(counts[0], 10);
    ASSERT_EQ(counts[1], 10);
    ASSERT_EQ(counts[2], 10);
    lb_group_free(g);
    config_free(c);
}

TEST(weighted_is_smooth)
{
    config_t *c = make_cfg("strategy weighted; server 127.0.0.1:1 weight=3; server 127.0.0.1:2;");
    lb_group_t *g = lb_group_new(&c->upstreams[0]);
    ASSERT(g != NULL);
    /* Classic nginx sequence for weights 3,1: A A B A, repeated. */
    int seq[8];
    for (int i = 0; i < 8; i++)
        seq[i] = (int)(lb_pick(g, "k", 1, 0) - g->servers);
    int a = 0;
    for (int i = 0; i < 8; i++)
        a += seq[i] == 0;
    ASSERT_EQ(a, 6);
    /* B must never be picked twice in a row (that is the "smooth" part). */
    for (int i = 1; i < 8; i++)
        ASSERT(!(seq[i] == 1 && seq[i - 1] == 1));
    lb_group_free(g);
    config_free(c);
}

TEST(least_conn_prefers_idle_server)
{
    config_t *c = make_cfg("strategy least_conn; server 127.0.0.1:1; server 127.0.0.1:2;");
    lb_group_t *g = lb_group_new(&c->upstreams[0]);
    ASSERT(g != NULL);
    lb_server_begin(&g->servers[0]);
    lb_server_begin(&g->servers[0]);
    lb_server_begin(&g->servers[1]);
    for (int i = 0; i < 5; i++)
        ASSERT(lb_pick(g, "k", 1, 0) == &g->servers[1]);
    lb_server_end(&g->servers[1]);
    lb_server_begin(&g->servers[1]);
    lb_server_begin(&g->servers[1]);
    lb_server_begin(&g->servers[1]);
    ASSERT(lb_pick(g, "k", 1, 0) == &g->servers[0]);
    lb_group_free(g);
    config_free(c);
}

TEST(ip_hash_is_sticky_and_balanced)
{
    config_t *c = make_cfg("strategy ip_hash; server 127.0.0.1:1; server 127.0.0.1:2; "
                           "server 127.0.0.1:3;");
    lb_group_t *g = lb_group_new(&c->upstreams[0]);
    ASSERT(g != NULL);
    int counts[3] = {0};
    for (int i = 0; i < 3000; i++) {
        char ip[32];
        int n = snprintf(ip, sizeof(ip), "10.0.%d.%d", i / 256, i % 256);
        lb_server_t *first = lb_pick(g, ip, (size_t)n, 0);
        /* Same key -> same server, every time. */
        for (int k = 0; k < 3; k++)
            ASSERT(lb_pick(g, ip, (size_t)n, 0) == first);
        counts[first - g->servers]++;
    }
    /* Roughly balanced: each server within 20..50% of the keys. */
    for (int i = 0; i < 3; i++)
        ASSERT(counts[i] > 600 && counts[i] < 1500);

    /* Taking a server down moves only its keys. */
    lb_server_t *victim = lb_pick(g, "client-a", 8, 0);
    atomic_store(&victim->health_ok, false);
    lb_server_t *moved = lb_pick(g, "client-a", 8, 0);
    ASSERT(moved != victim);
    for (int i = 0; i < 3000; i++) {
        char ip[32];
        int n = snprintf(ip, sizeof(ip), "10.0.%d.%d", i / 256, i % 256);
        ASSERT(lb_pick(g, ip, (size_t)n, 0) != victim);
    }
    lb_group_free(g);
    config_free(c);
}

TEST(passive_failures_mark_server_down)
{
    config_t *c = make_cfg("server 127.0.0.1:1 max_fails=2 fail_timeout=10s; server 127.0.0.1:2;");
    lb_group_t *g = lb_group_new(&c->upstreams[0]);
    ASSERT(g != NULL);
    lb_server_t *a = &g->servers[0], *b = &g->servers[1];

    lb_server_failure(a, 1000);
    ASSERT(lb_server_available(a, 1000)); /* one failure is tolerated */
    lb_server_failure(a, 1000);
    ASSERT(!lb_server_available(a, 1000));
    ASSERT(!lb_server_available(a, 10999));
    ASSERT(lb_server_available(a, 11000)); /* fail_timeout elapsed */

    /* While down, every pick goes to b. */
    for (int i = 0; i < 10; i++)
        ASSERT(lb_pick(g, "k", 1, 5000) == b);

    /* Success resets the failure count. */
    lb_server_success(a);
    lb_server_failure(a, 20000);
    ASSERT(lb_server_available(a, 20000));

    /* Everyone down -> NULL. */
    lb_server_failure(b, 20000);
    lb_server_failure(b, 20000);
    lb_server_failure(b, 20000);
    lb_server_failure(a, 20000);
    ASSERT(lb_pick(g, "k", 1, 20000) == NULL);
    lb_group_free(g);
    config_free(c);
}

TEST(unresolvable_host_fails)
{
    config_t *c = make_cfg("server nonexistent.invalid:80;");
    ASSERT(c != NULL);
    if (!c)
        return;
    lb_group_t *g = lb_group_new(&c->upstreams[0]);
    ASSERT(g == NULL);
    config_free(c);
}

int main(void)
{
    RUN(round_robin_cycles);
    RUN(weighted_is_smooth);
    RUN(least_conn_prefers_idle_server);
    RUN(ip_hash_is_sticky_and_balanced);
    RUN(passive_failures_mark_server_down);
    RUN(unresolvable_host_fails);
    return TEST_RESULT();
}
