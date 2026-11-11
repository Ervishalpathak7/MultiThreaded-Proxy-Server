/*
 * config.c - tokenizer + recursive descent parser for the config format.
 *
 * Two passes are not needed: directives are applied as soon as they are
 * parsed, and cross references (route -> upstream) are validated at the end.
 */
#include "config/config.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/log.h"
#include "core/util.h"

/* ------------------------------------------------------------------------
 * Tokenizer
 * ---------------------------------------------------------------------- */
typedef enum { TOK_WORD, TOK_SEMI, TOK_LBRACE, TOK_RBRACE, TOK_EOF } tok_kind_t;

typedef struct {
    tok_kind_t kind;
    char *text; /* malloc'd for TOK_WORD, NULL otherwise */
    int line;
} token_t;

typedef struct {
    const char *src;
    size_t pos;
    int line;
    token_t tok; /* one token of lookahead */
    char *err;
    size_t errlen;
    bool failed;
} parser_t;

static void tok_free(token_t *t)
{
    free(t->text);
    t->text = NULL;
}

static void skip_space_and_comments(parser_t *p)
{
    for (;;) {
        char c = p->src[p->pos];
        if (c == '\n') {
            p->line++;
            p->pos++;
        } else if (isspace((unsigned char)c)) {
            p->pos++;
        } else if (c == '#') {
            while (p->src[p->pos] && p->src[p->pos] != '\n')
                p->pos++;
        } else {
            return;
        }
    }
}

static void next_token(parser_t *p)
{
    tok_free(&p->tok);
    skip_space_and_comments(p);
    p->tok.line = p->line;
    char c = p->src[p->pos];
    if (c == '\0') {
        p->tok.kind = TOK_EOF;
        return;
    }
    if (c == ';' || c == '{' || c == '}') {
        p->tok.kind = c == ';' ? TOK_SEMI : c == '{' ? TOK_LBRACE : TOK_RBRACE;
        p->pos++;
        return;
    }
    size_t start = p->pos;
    if (c == '"' || c == '\'') {
        /* Quoted word: everything up to the matching quote. */
        char q = c;
        p->pos++;
        start = p->pos;
        while (p->src[p->pos] && p->src[p->pos] != q)
            p->pos++;
        p->tok.kind = TOK_WORD;
        p->tok.text = xstrndup(p->src + start, p->pos - start);
        if (p->src[p->pos] == q)
            p->pos++;
        return;
    }
    while (p->src[p->pos] && !isspace((unsigned char)p->src[p->pos]) && p->src[p->pos] != ';' &&
           p->src[p->pos] != '{' && p->src[p->pos] != '}' && p->src[p->pos] != '#')
        p->pos++;
    p->tok.kind = TOK_WORD;
    p->tok.text = xstrndup(p->src + start, p->pos - start);
}

static void fail(parser_t *p, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void fail(parser_t *p, const char *fmt, ...)
{
    if (p->failed)
        return;
    p->failed = true;
    int n = snprintf(p->err, p->errlen, "line %d: ", p->tok.line);
    if (n < 0 || (size_t)n >= p->errlen)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p->err + n, p->errlen - (size_t)n, fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------------------
 * Directive parsing helpers
 *
 * A directive is `name arg arg ... ;` or `name arg ... { ... }`. We collect
 * the args into a small array, then dispatch on the name.
 * ---------------------------------------------------------------------- */
#define MAX_ARGS 16

typedef struct {
    char *name;
    char *args[MAX_ARGS];
    int nargs;
    bool has_block; /* true if the directive opened a '{' */
    int line;
} directive_t;

static void directive_free(directive_t *d)
{
    free(d->name);
    for (int i = 0; i < d->nargs; i++)
        free(d->args[i]);
    memset(d, 0, sizeof(*d));
}

/* Parse one directive header. Returns false at '}' or EOF. */
static bool read_directive(parser_t *p, directive_t *d)
{
    memset(d, 0, sizeof(*d));
    if (p->tok.kind != TOK_WORD) {
        if (p->tok.kind == TOK_SEMI) { /* stray ';' is tolerated */
            next_token(p);
            return read_directive(p, d);
        }
        return false;
    }
    d->name = p->tok.text;
    d->line = p->tok.line;
    p->tok.text = NULL;
    next_token(p);
    while (p->tok.kind == TOK_WORD) {
        if (d->nargs == MAX_ARGS) {
            fail(p, "too many arguments to '%s'", d->name);
            return false;
        }
        d->args[d->nargs++] = p->tok.text;
        p->tok.text = NULL;
        next_token(p);
    }
    if (p->tok.kind == TOK_SEMI) {
        next_token(p);
    } else if (p->tok.kind == TOK_LBRACE) {
        d->has_block = true;
        next_token(p);
    } else {
        fail(p, "expected ';' or '{' after '%s'", d->name);
        return false;
    }
    return true;
}

static bool expect_args(parser_t *p, directive_t *d, int min, int max)
{
    if (d->nargs < min || d->nargs > max) {
        fail(p, "'%s' takes %d%s argument(s)", d->name, min, min == max ? "" : "+");
        return false;
    }
    if (d->has_block) {
        fail(p, "'%s' does not take a block", d->name);
        return false;
    }
    return true;
}

static bool arg_duration(parser_t *p, directive_t *d, const char *s, uint64_t *out)
{
    if (!parse_duration_ms(s, out)) {
        fail(p, "'%s': bad duration '%s'", d->name, s);
        return false;
    }
    return true;
}

static bool arg_size(parser_t *p, directive_t *d, const char *s, size_t *out)
{
    if (!parse_size(s, out)) {
        fail(p, "'%s': bad size '%s'", d->name, s);
        return false;
    }
    return true;
}

static bool arg_int(parser_t *p, directive_t *d, const char *s, int lo, int hi, int *out)
{
    uint64_t v;
    if (!parse_u64(s, strlen(s), &v) || v < (uint64_t)lo || v > (uint64_t)hi) {
        fail(p, "'%s': expected a number between %d and %d, got '%s'", d->name, lo, hi, s);
        return false;
    }
    *out = (int)v;
    return true;
}

static bool arg_bool(parser_t *p, directive_t *d, const char *s, bool *out)
{
    if (strcmp(s, "on") == 0 || strcmp(s, "true") == 0 || strcmp(s, "yes") == 0)
        *out = true;
    else if (strcmp(s, "off") == 0 || strcmp(s, "false") == 0 || strcmp(s, "no") == 0)
        *out = false;
    else {
        fail(p, "'%s': expected on/off, got '%s'", d->name, s);
        return false;
    }
    return true;
}

/* Split "key=value" in place; returns false if there is no '='. */
static bool split_kv(char *arg, char **key, char **val)
{
    char *eq = strchr(arg, '=');
    if (!eq)
        return false;
    *eq = '\0';
    *key = arg;
    *val = eq + 1;
    return true;
}

/* Parse "host:port" into a newly allocated host and a port. */
static bool parse_hostport(parser_t *p, directive_t *d, const char *s, char **host, uint16_t *port)
{
    const char *colon = strrchr(s, ':');
    if (!colon || colon == s) {
        fail(p, "'%s': expected host:port, got '%s'", d->name, s);
        return false;
    }
    uint64_t v;
    if (!parse_u64(colon + 1, strlen(colon + 1), &v) || v == 0 || v > 65535) {
        fail(p, "'%s': bad port in '%s'", d->name, s);
        return false;
    }
    *host = xstrndup(s, (size_t)(colon - s));
    *port = (uint16_t)v;
    return true;
}

/* Skip the rest of an unknown block so one typo does not cascade. */
static void skip_block(parser_t *p)
{
    int depth = 1;
    while (depth > 0 && p->tok.kind != TOK_EOF) {
        if (p->tok.kind == TOK_LBRACE)
            depth++;
        else if (p->tok.kind == TOK_RBRACE)
            depth--;
        next_token(p);
    }
}

static void end_block(parser_t *p, const char *what)
{
    if (p->tok.kind != TOK_RBRACE) {
        fail(p, "expected '}' to close '%s'", what);
        return;
    }
    next_token(p);
}

/* ------------------------------------------------------------------------
 * Blocks
 * ---------------------------------------------------------------------- */
static void parse_upstream(parser_t *p, config_t *cfg, directive_t *hdr)
{
    if (hdr->nargs != 1 || !hdr->has_block) {
        fail(p, "usage: upstream NAME { ... }");
        return;
    }
    if (config_find_upstream(cfg, hdr->args[0])) {
        fail(p, "duplicate upstream '%s'", hdr->args[0]);
        return;
    }
    cfg->upstreams = xrealloc(cfg->upstreams, (cfg->nupstreams + 1) * sizeof(*cfg->upstreams));
    config_upstream_t *u = &cfg->upstreams[cfg->nupstreams++];
    memset(u, 0, sizeof(*u));
    u->name = xstrdup(hdr->args[0]);
    u->strategy = LB_ROUND_ROBIN;
    u->connect_timeout_ms = 3000;
    u->read_timeout_ms = 30000;
    u->keepalive = 32;
    u->health_interval_ms = 5000;

    directive_t d;
    while (!p->failed && read_directive(p, &d)) {
        if (strcmp(d.name, "server") == 0) {
            if (d.nargs < 1 || d.has_block) {
                fail(p, "usage: server HOST:PORT [weight=N] [max_fails=N] [fail_timeout=T];");
            } else {
                u->servers = xrealloc(u->servers, (u->nservers + 1) * sizeof(*u->servers));
                config_server_t *s = &u->servers[u->nservers++];
                memset(s, 0, sizeof(*s));
                s->weight = 1;
                s->max_fails = 3;
                s->fail_timeout_ms = 10000;
                parse_hostport(p, &d, d.args[0], &s->host, &s->port);
                for (int i = 1; i < d.nargs && !p->failed; i++) {
                    char *k, *v;
                    if (!split_kv(d.args[i], &k, &v)) {
                        fail(p, "server: expected key=value, got '%s'", d.args[i]);
                    } else if (strcmp(k, "weight") == 0) {
                        arg_int(p, &d, v, 1, 1000000, &s->weight);
                    } else if (strcmp(k, "max_fails") == 0) {
                        arg_int(p, &d, v, 0, 1000000, &s->max_fails);
                    } else if (strcmp(k, "fail_timeout") == 0) {
                        arg_duration(p, &d, v, &s->fail_timeout_ms);
                    } else {
                        fail(p, "server: unknown option '%s'", k);
                    }
                }
            }
        } else if (strcmp(d.name, "strategy") == 0) {
            if (expect_args(p, &d, 1, 1)) {
                const char *s = d.args[0];
                if (strcmp(s, "round_robin") == 0)
                    u->strategy = LB_ROUND_ROBIN;
                else if (strcmp(s, "weighted") == 0 || strcmp(s, "weighted_round_robin") == 0)
                    u->strategy = LB_WEIGHTED;
                else if (strcmp(s, "least_conn") == 0)
                    u->strategy = LB_LEAST_CONN;
                else if (strcmp(s, "ip_hash") == 0 || strcmp(s, "consistent_hash") == 0)
                    u->strategy = LB_IP_HASH;
                else
                    fail(p, "unknown strategy '%s'", s);
            }
        } else if (strcmp(d.name, "health_check") == 0) {
            if (d.has_block) {
                fail(p, "health_check does not take a block");
            } else {
                u->health_check = true;
                u->health_path = u->health_path ? u->health_path : xstrdup("/");
                for (int i = 0; i < d.nargs && !p->failed; i++) {
                    char *k, *v;
                    if (!split_kv(d.args[i], &k, &v)) {
                        fail(p, "health_check: expected key=value, got '%s'", d.args[i]);
                    } else if (strcmp(k, "interval") == 0) {
                        arg_duration(p, &d, v, &u->health_interval_ms);
                    } else if (strcmp(k, "path") == 0) {
                        free(u->health_path);
                        u->health_path = xstrdup(v);
                    } else {
                        fail(p, "health_check: unknown option '%s'", k);
                    }
                }
            }
        } else if (strcmp(d.name, "connect_timeout") == 0) {
            if (expect_args(p, &d, 1, 1))
                arg_duration(p, &d, d.args[0], &u->connect_timeout_ms);
        } else if (strcmp(d.name, "read_timeout") == 0) {
            if (expect_args(p, &d, 1, 1))
                arg_duration(p, &d, d.args[0], &u->read_timeout_ms);
        } else if (strcmp(d.name, "keepalive") == 0) {
            int n;
            if (expect_args(p, &d, 1, 1) && arg_int(p, &d, d.args[0], 0, 100000, &n))
                u->keepalive = (size_t)n;
        } else {
            fail(p, "unknown directive '%s' in upstream", d.name);
        }
        directive_free(&d);
    }
    directive_free(&d);
    if (!p->failed && u->nservers == 0)
        fail(p, "upstream '%s' has no servers", u->name);
    if (!p->failed)
        end_block(p, "upstream");
}

static void parse_cache(parser_t *p, config_t *cfg, directive_t *hdr)
{
    if (hdr->nargs != 0 || !hdr->has_block) {
        fail(p, "usage: cache { ... }");
        return;
    }
    directive_t d;
    while (!p->failed && read_directive(p, &d)) {
        if (strcmp(d.name, "max_size") == 0) {
            if (expect_args(p, &d, 1, 1))
                arg_size(p, &d, d.args[0], &cfg->cache_max_size);
        } else if (strcmp(d.name, "default_ttl") == 0) {
            if (expect_args(p, &d, 1, 1))
                arg_duration(p, &d, d.args[0], &cfg->cache_default_ttl_ms);
        } else if (strcmp(d.name, "shards") == 0) {
            if (expect_args(p, &d, 1, 1))
                arg_int(p, &d, d.args[0], 1, 4096, &cfg->cache_shards);
        } else {
            fail(p, "unknown directive '%s' in cache", d.name);
        }
        directive_free(&d);
    }
    directive_free(&d);
    if (!p->failed)
        end_block(p, "cache");
}

static void parse_route(parser_t *p, config_t *cfg, directive_t *hdr)
{
    if (hdr->nargs != 1 || !hdr->has_block) {
        fail(p, "usage: route PREFIX { proxy_pass NAME; cache on|off; }");
        return;
    }
    cfg->routes = xrealloc(cfg->routes, (cfg->nroutes + 1) * sizeof(*cfg->routes));
    config_route_t *r = &cfg->routes[cfg->nroutes++];
    memset(r, 0, sizeof(*r));
    r->prefix = xstrdup(hdr->args[0]);

    directive_t d;
    while (!p->failed && read_directive(p, &d)) {
        if (strcmp(d.name, "proxy_pass") == 0) {
            if (expect_args(p, &d, 1, 1)) {
                free(r->upstream);
                r->upstream = xstrdup(d.args[0]);
            }
        } else if (strcmp(d.name, "cache") == 0) {
            if (expect_args(p, &d, 1, 1))
                arg_bool(p, &d, d.args[0], &r->cache);
        } else if (strcmp(d.name, "cache_ttl") == 0) {
            if (expect_args(p, &d, 1, 1))
                arg_duration(p, &d, d.args[0], &r->cache_ttl_ms);
        } else {
            fail(p, "unknown directive '%s' in route", d.name);
        }
        directive_free(&d);
    }
    directive_free(&d);
    if (!p->failed && !r->upstream)
        fail(p, "route '%s' has no proxy_pass", r->prefix);
    if (!p->failed)
        end_block(p, "route");
}

static void add_cert(config_t *cfg, const char *host, const char *cert, const char *key)
{
    /* Merge with an existing entry for the same host so that
     * ssl_certificate and ssl_certificate_key can be separate lines. */
    for (size_t i = 0; i < cfg->ncerts; i++) {
        config_cert_t *c = &cfg->certs[i];
        bool same = (!host && !c->hostname) || (host && c->hostname && strcmp(host, c->hostname) == 0);
        if (same) {
            if (cert) {
                free(c->cert_file);
                c->cert_file = xstrdup(cert);
            }
            if (key) {
                free(c->key_file);
                c->key_file = xstrdup(key);
            }
            return;
        }
    }
    cfg->certs = xrealloc(cfg->certs, (cfg->ncerts + 1) * sizeof(*cfg->certs));
    config_cert_t *c = &cfg->certs[cfg->ncerts++];
    c->hostname = host ? xstrdup(host) : NULL;
    c->cert_file = cert ? xstrdup(cert) : NULL;
    c->key_file = key ? xstrdup(key) : NULL;
}

/* tls_host NAME { ssl_certificate ...; ssl_certificate_key ...; } */
static void parse_tls_host(parser_t *p, config_t *cfg, directive_t *hdr)
{
    if (hdr->nargs != 1 || !hdr->has_block) {
        fail(p, "usage: tls_host NAME { ssl_certificate F; ssl_certificate_key F; }");
        return;
    }
    directive_t d;
    while (!p->failed && read_directive(p, &d)) {
        if (strcmp(d.name, "ssl_certificate") == 0) {
            if (expect_args(p, &d, 1, 1))
                add_cert(cfg, hdr->args[0], d.args[0], NULL);
        } else if (strcmp(d.name, "ssl_certificate_key") == 0) {
            if (expect_args(p, &d, 1, 1))
                add_cert(cfg, hdr->args[0], NULL, d.args[0]);
        } else {
            fail(p, "unknown directive '%s' in tls_host", d.name);
        }
        directive_free(&d);
    }
    directive_free(&d);
    if (!p->failed)
        end_block(p, "tls_host");
}

/* listen [ADDR:]PORT [ssl]; */
static void parse_listen(parser_t *p, config_t *cfg, directive_t *d)
{
    if (d->nargs < 1 || d->nargs > 2 || d->has_block) {
        fail(p, "usage: listen [ADDR:]PORT [ssl];");
        return;
    }
    cfg->listens = xrealloc(cfg->listens, (cfg->nlistens + 1) * sizeof(*cfg->listens));
    config_listen_t *l = &cfg->listens[cfg->nlistens++];
    memset(l, 0, sizeof(*l));
    if (strchr(d->args[0], ':')) {
        parse_hostport(p, d, d->args[0], &l->addr, &l->port);
    } else {
        int port;
        if (arg_int(p, d, d->args[0], 1, 65535, &port))
            l->port = (uint16_t)port;
        l->addr = xstrdup("0.0.0.0");
    }
    if (d->nargs == 2) {
        if (strcmp(d->args[1], "ssl") == 0)
            l->ssl = true;
        else
            fail(p, "listen: unknown option '%s'", d->args[1]);
    }
}

static void parse_top_level(parser_t *p, config_t *cfg)
{
    directive_t d;
    while (!p->failed && read_directive(p, &d)) {
        const char *n = d.name;
        if (strcmp(n, "upstream") == 0) {
            parse_upstream(p, cfg, &d);
        } else if (strcmp(n, "cache") == 0) {
            parse_cache(p, cfg, &d);
        } else if (strcmp(n, "route") == 0) {
            parse_route(p, cfg, &d);
        } else if (strcmp(n, "tls_host") == 0) {
            parse_tls_host(p, cfg, &d);
        } else if (strcmp(n, "listen") == 0) {
            parse_listen(p, cfg, &d);
        } else if (strcmp(n, "http3") == 0) {
            if (expect_args(p, &d, 1, 1)) {
                if (strchr(d.args[0], ':')) {
                    free(cfg->http3_addr);
                    parse_hostport(p, &d, d.args[0], &cfg->http3_addr, &cfg->http3_port);
                } else {
                    int port;
                    if (arg_int(p, &d, d.args[0], 1, 65535, &port))
                        cfg->http3_port = (uint16_t)port;
                }
            }
        } else if (strcmp(n, "ssl_certificate") == 0) {
            if (expect_args(p, &d, 1, 1))
                add_cert(cfg, NULL, d.args[0], NULL);
        } else if (strcmp(n, "ssl_certificate_key") == 0) {
            if (expect_args(p, &d, 1, 1))
                add_cert(cfg, NULL, NULL, d.args[0]);
        } else if (strcmp(n, "workers") == 0) {
            if (expect_args(p, &d, 1, 1))
                arg_int(p, &d, d.args[0], 0, 1024, &cfg->workers);
        } else if (strcmp(n, "log_level") == 0) {
            if (expect_args(p, &d, 1, 1)) {
                int lvl = log_level_from_name(d.args[0]);
                if (lvl < 0)
                    fail(p, "unknown log level '%s'", d.args[0]);
                else
                    cfg->log_level = lvl;
            }
        } else if (strcmp(n, "max_header_size") == 0) {
            if (expect_args(p, &d, 1, 1))
                arg_size(p, &d, d.args[0], &cfg->max_header_size);
        } else if (strcmp(n, "max_body_size") == 0) {
            if (expect_args(p, &d, 1, 1))
                arg_size(p, &d, d.args[0], &cfg->max_body_size);
        } else if (strcmp(n, "header_timeout") == 0) {
            if (expect_args(p, &d, 1, 1))
                arg_duration(p, &d, d.args[0], &cfg->header_timeout_ms);
        } else if (strcmp(n, "idle_timeout") == 0) {
            if (expect_args(p, &d, 1, 1))
                arg_duration(p, &d, d.args[0], &cfg->idle_timeout_ms);
        } else {
            fail(p, "unknown directive '%s'", n);
            if (d.has_block)
                skip_block(p);
        }
        directive_free(&d);
    }
    directive_free(&d);
    if (!p->failed && p->tok.kind != TOK_EOF)
        fail(p, "unexpected '}'");
}

/* ------------------------------------------------------------------------
 * Validation of cross references and defaults
 * ---------------------------------------------------------------------- */
static bool validate(parser_t *p, config_t *cfg)
{
    if (cfg->nlistens == 0 && cfg->http3_port == 0) {
        fail(p, "no 'listen' directive");
        return false;
    }
    for (size_t i = 0; i < cfg->nroutes; i++) {
        if (!config_find_upstream(cfg, cfg->routes[i].upstream)) {
            fail(p, "route '%s' refers to unknown upstream '%s'", cfg->routes[i].prefix,
                 cfg->routes[i].upstream);
            return false;
        }
    }
    bool needs_tls = cfg->http3_port != 0;
    for (size_t i = 0; i < cfg->nlistens; i++)
        needs_tls |= cfg->listens[i].ssl;
    for (size_t i = 0; i < cfg->ncerts; i++) {
        config_cert_t *c = &cfg->certs[i];
        if (!c->cert_file || !c->key_file) {
            fail(p, "certificate for '%s' needs both ssl_certificate and ssl_certificate_key",
                 c->hostname ? c->hostname : "default");
            return false;
        }
    }
    if (needs_tls && cfg->ncerts == 0) {
        fail(p, "'listen ... ssl' / 'http3' requires ssl_certificate and ssl_certificate_key");
        return false;
    }
    if (cfg->nroutes == 0) {
        fail(p, "no 'route' blocks; nothing to proxy to");
        return false;
    }
    return true;
}

static void set_defaults(config_t *cfg)
{
    cfg->workers = 0;
    cfg->log_level = LOG_LEVEL_INFO;
    cfg->max_header_size = 16 * 1024;
    cfg->max_body_size = 8 * 1024 * 1024;
    cfg->header_timeout_ms = 10000;
    cfg->idle_timeout_ms = 60000;
    cfg->cache_max_size = 64 * 1024 * 1024;
    cfg->cache_default_ttl_ms = 60000;
    cfg->cache_shards = 16;
    cfg->http3_addr = xstrdup("0.0.0.0");
}

config_t *config_parse(const char *text, char *err, size_t errlen)
{
    parser_t p = {.src = text, .line = 1, .err = err, .errlen = errlen};
    if (errlen)
        err[0] = '\0';
    config_t *cfg = xcalloc(1, sizeof(*cfg));
    set_defaults(cfg);

    next_token(&p);
    parse_top_level(&p, cfg);
    if (!p.failed)
        validate(&p, cfg);
    tok_free(&p.tok);

    if (p.failed) {
        config_free(cfg);
        return NULL;
    }
    return cfg;
}

config_t *config_load(const char *path, char *err, size_t errlen)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errlen, "cannot open %s: %s", path, strerror(errno));
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) {
        fclose(f);
        snprintf(err, errlen, "cannot read %s", path);
        return NULL;
    }
    char *text = xmalloc((size_t)size + 1);
    size_t n = fread(text, 1, (size_t)size, f);
    fclose(f);
    text[n] = '\0';
    config_t *cfg = config_parse(text, err, errlen);
    free(text);
    return cfg;
}

void config_free(config_t *cfg)
{
    if (!cfg)
        return;
    for (size_t i = 0; i < cfg->nlistens; i++)
        free(cfg->listens[i].addr);
    free(cfg->listens);
    free(cfg->http3_addr);
    for (size_t i = 0; i < cfg->ncerts; i++) {
        free(cfg->certs[i].hostname);
        free(cfg->certs[i].cert_file);
        free(cfg->certs[i].key_file);
    }
    free(cfg->certs);
    for (size_t i = 0; i < cfg->nupstreams; i++) {
        config_upstream_t *u = &cfg->upstreams[i];
        for (size_t j = 0; j < u->nservers; j++)
            free(u->servers[j].host);
        free(u->servers);
        free(u->name);
        free(u->health_path);
    }
    free(cfg->upstreams);
    for (size_t i = 0; i < cfg->nroutes; i++) {
        free(cfg->routes[i].prefix);
        free(cfg->routes[i].upstream);
    }
    free(cfg->routes);
    free(cfg);
}

config_upstream_t *config_find_upstream(const config_t *cfg, const char *name)
{
    for (size_t i = 0; i < cfg->nupstreams; i++)
        if (strcmp(cfg->upstreams[i].name, name) == 0)
            return &cfg->upstreams[i];
    return NULL;
}

config_route_t *config_match_route(const config_t *cfg, const char *path, size_t len)
{
    config_route_t *best = NULL;
    size_t best_len = 0;
    for (size_t i = 0; i < cfg->nroutes; i++) {
        config_route_t *r = &cfg->routes[i];
        size_t plen = strlen(r->prefix);
        if (plen <= len && memcmp(r->prefix, path, plen) == 0 && plen >= best_len) {
            best = r;
            best_len = plen;
        }
    }
    return best;
}

const char *lb_strategy_name(lb_strategy_t s)
{
    switch (s) {
    case LB_ROUND_ROBIN:
        return "round_robin";
    case LB_WEIGHTED:
        return "weighted";
    case LB_LEAST_CONN:
        return "least_conn";
    case LB_IP_HASH:
        return "ip_hash";
    }
    return "?";
}
