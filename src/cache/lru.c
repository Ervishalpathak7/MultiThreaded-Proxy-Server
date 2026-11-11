/*
 * lru.c - sharded LRU cache (see lru.h).
 */
#include "cache/lru.h"

#include <stdlib.h>
#include <string.h>

#include "core/log.h"
#include "core/util.h"

#define INITIAL_BUCKETS 256

/* ------------------------------------------------------------------------
 * Entry lifetime
 * ---------------------------------------------------------------------- */
static void entry_free(cache_entry_t *e)
{
    free(e->key);
    headers_free(&e->headers);
    buf_free(&e->body);
    free(e);
}

void cache_entry_unref(cache_entry_t *e)
{
    if (atomic_fetch_sub_explicit(&e->refs, 1, memory_order_acq_rel) == 1)
        entry_free(e);
}

static void entry_ref(cache_entry_t *e)
{
    atomic_fetch_add_explicit(&e->refs, 1, memory_order_relaxed);
}

/* ------------------------------------------------------------------------
 * Shard internals (caller holds shard->lock)
 * ---------------------------------------------------------------------- */
static void lru_unlink(cache_shard_t *s, cache_entry_t *e)
{
    if (e->lprev)
        e->lprev->lnext = e->lnext;
    else
        s->lru_head = e->lnext;
    if (e->lnext)
        e->lnext->lprev = e->lprev;
    else
        s->lru_tail = e->lprev;
    e->lprev = e->lnext = NULL;
}

static void lru_push_front(cache_shard_t *s, cache_entry_t *e)
{
    e->lprev = NULL;
    e->lnext = s->lru_head;
    if (s->lru_head)
        s->lru_head->lprev = e;
    s->lru_head = e;
    if (!s->lru_tail)
        s->lru_tail = e;
}

static cache_entry_t **bucket_for(cache_shard_t *s, uint64_t hash)
{
    return &s->buckets[hash & (s->nbuckets - 1)];
}

static cache_entry_t *shard_find(cache_shard_t *s, uint64_t hash, const char *key, size_t key_len)
{
    for (cache_entry_t *e = *bucket_for(s, hash); e; e = e->hnext)
        if (e->hash == hash && e->key_len == key_len && memcmp(e->key, key, key_len) == 0)
            return e;
    return NULL;
}

/* Detach from both structures and drop the shard's reference. */
static void shard_remove(cache_shard_t *s, cache_entry_t *e)
{
    cache_entry_t **pp = bucket_for(s, e->hash);
    while (*pp && *pp != e)
        pp = &(*pp)->hnext;
    if (*pp)
        *pp = e->hnext;
    lru_unlink(s, e);
    s->nentries--;
    s->bytes -= e->size;
    cache_entry_unref(e);
}

/* Double the table when the load factor passes 1. */
static void shard_maybe_grow(cache_shard_t *s)
{
    if (s->nentries <= s->nbuckets)
        return;
    size_t nb = s->nbuckets * 2;
    cache_entry_t **nbk = xcalloc(nb, sizeof(*nbk));
    for (size_t i = 0; i < s->nbuckets; i++) {
        cache_entry_t *e = s->buckets[i];
        while (e) {
            cache_entry_t *next = e->hnext;
            size_t idx = e->hash & (nb - 1);
            e->hnext = nbk[idx];
            nbk[idx] = e;
            e = next;
        }
    }
    free(s->buckets);
    s->buckets = nbk;
    s->nbuckets = nb;
}

static void shard_insert(cache_shard_t *s, cache_entry_t *e)
{
    cache_entry_t **b = bucket_for(s, e->hash);
    e->hnext = *b;
    *b = e;
    lru_push_front(s, e);
    s->nentries++;
    s->bytes += e->size;
    shard_maybe_grow(s);
}

static size_t shard_evict_to_fit(cache_shard_t *s, size_t incoming)
{
    size_t evicted = 0;
    while (s->lru_tail && s->bytes + incoming > s->max_bytes) {
        shard_remove(s, s->lru_tail);
        evicted++;
    }
    return evicted;
}

/* ------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */
cache_t *cache_new(size_t max_bytes, size_t nshards)
{
    if (nshards == 0)
        nshards = 1;
    cache_t *c = xcalloc(1, sizeof(*c));
    c->nshards = nshards;
    c->shards = xcalloc(nshards, sizeof(cache_shard_t));
    for (size_t i = 0; i < nshards; i++) {
        cache_shard_t *s = &c->shards[i];
        pthread_mutex_init(&s->lock, NULL);
        s->nbuckets = INITIAL_BUCKETS;
        s->buckets = xcalloc(s->nbuckets, sizeof(*s->buckets));
        s->max_bytes = max_bytes / nshards;
    }
    atomic_init(&c->hits, 0);
    atomic_init(&c->misses, 0);
    atomic_init(&c->evictions, 0);
    return c;
}

void cache_free(cache_t *c)
{
    if (!c)
        return;
    for (size_t i = 0; i < c->nshards; i++) {
        cache_shard_t *s = &c->shards[i];
        while (s->lru_tail)
            shard_remove(s, s->lru_tail);
        free(s->buckets);
        pthread_mutex_destroy(&s->lock);
    }
    free(c->shards);
    free(c);
}

static cache_shard_t *shard_of(cache_t *c, uint64_t hash)
{
    /* Use the high bits for shard selection and the low bits for the
     * bucket, so the two choices are independent. */
    return &c->shards[(hash >> 48) % c->nshards];
}

cache_entry_t *cache_get(cache_t *c, const char *key, size_t key_len, uint64_t now_ms)
{
    uint64_t hash = hash_mix64(hash_fnv1a(key, key_len));
    cache_shard_t *s = shard_of(c, hash);

    pthread_mutex_lock(&s->lock);
    cache_entry_t *e = shard_find(s, hash, key, key_len);
    if (e && e->expires_ms <= now_ms) {
        shard_remove(s, e);
        e = NULL;
    }
    if (e) {
        /* Promote to most recently used and hand out a reference. */
        lru_unlink(s, e);
        lru_push_front(s, e);
        entry_ref(e);
    }
    pthread_mutex_unlock(&s->lock);

    atomic_fetch_add_explicit(e ? &c->hits : &c->misses, 1, memory_order_relaxed);
    return e;
}

bool cache_put(cache_t *c, const char *key, size_t key_len, const http_response_t *resp,
               uint64_t ttl_ms, uint64_t now_ms)
{
    uint64_t hash = hash_mix64(hash_fnv1a(key, key_len));
    cache_shard_t *s = shard_of(c, hash);

    cache_entry_t *e = xcalloc(1, sizeof(*e));
    e->key = xstrndup(key, key_len);
    e->key_len = key_len;
    e->hash = hash;
    e->status = resp->status;
    headers_init(&e->headers);
    for (size_t i = 0; i < headers_count(&resp->headers); i++) {
        size_t nl, vl;
        const char *n = headers_name(&resp->headers, i, &nl);
        const char *v = headers_value(&resp->headers, i, &vl);
        headers_add(&e->headers, n, nl, v, vl);
    }
    buf_init(&e->body);
    buf_append(&e->body, buf_rptr(&resp->body), buf_rlen(&resp->body));
    e->stored_at_s = wall_s();
    e->expires_ms = now_ms + ttl_ms;
    e->size = sizeof(*e) + key_len + e->headers.arena.len +
              headers_count(&e->headers) * sizeof(hdr_ref_t) + buf_rlen(&e->body);
    atomic_init(&e->refs, 1); /* the shard's reference */

    if (e->size > s->max_bytes) {
        entry_free(e);
        return false;
    }

    pthread_mutex_lock(&s->lock);
    cache_entry_t *old = shard_find(s, hash, key, key_len);
    if (old)
        shard_remove(s, old);
    size_t evicted = shard_evict_to_fit(s, e->size);
    shard_insert(s, e);
    pthread_mutex_unlock(&s->lock);

    if (evicted)
        atomic_fetch_add_explicit(&c->evictions, evicted, memory_order_relaxed);
    return true;
}

bool cache_delete(cache_t *c, const char *key, size_t key_len)
{
    uint64_t hash = hash_mix64(hash_fnv1a(key, key_len));
    cache_shard_t *s = shard_of(c, hash);
    pthread_mutex_lock(&s->lock);
    cache_entry_t *e = shard_find(s, hash, key, key_len);
    if (e)
        shard_remove(s, e);
    pthread_mutex_unlock(&s->lock);
    return e != NULL;
}

size_t cache_bytes(const cache_t *c)
{
    size_t total = 0;
    for (size_t i = 0; i < c->nshards; i++)
        total += c->shards[i].bytes; /* racy read; statistics only */
    return total;
}

size_t cache_entries(const cache_t *c)
{
    size_t total = 0;
    for (size_t i = 0; i < c->nshards; i++)
        total += c->shards[i].nentries;
    return total;
}
