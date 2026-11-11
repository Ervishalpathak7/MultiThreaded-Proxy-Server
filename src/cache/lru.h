/*
 * lru.h - sharded, byte-bounded LRU cache of HTTP responses.
 *
 * Layout: the key space is split into N shards by hash. Each shard has its
 * own mutex, hash table and LRU list, so workers contend only when they hit
 * the same shard at the same instant. Within a shard:
 *
 *     hash table (chained)        LRU list (most recent at head)
 *     bucket[h % nbuckets] ──▶ entry ◀──▶ entry ◀──▶ entry
 *
 * Every entry is in both structures, so lookup is O(1) and promoting an
 * entry to "most recently used" is an O(1) list unlink + push.
 *
 * Entries are reference counted: a reader holds a reference while it
 * copies the response out, so an eviction on another thread cannot free
 * memory that is being read.
 */
#ifndef APROXY_CACHE_LRU_H
#define APROXY_CACHE_LRU_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "http/request.h"

typedef struct cache_entry {
    char *key;
    size_t key_len;
    uint64_t hash;

    int status;
    http_headers_t headers;
    buf_t body;

    uint64_t stored_at_s;  /* wall clock, for the Age header */
    uint64_t expires_ms;   /* monotonic deadline */
    size_t size;           /* bytes charged against the shard budget */

    atomic_int refs;
    struct cache_entry *hnext;         /* hash chain */
    struct cache_entry *lprev, *lnext; /* LRU list */
} cache_entry_t;

typedef struct cache_shard {
    pthread_mutex_t lock;
    cache_entry_t **buckets;
    size_t nbuckets;
    size_t nentries;
    cache_entry_t *lru_head, *lru_tail; /* head = most recently used */
    size_t bytes;
    size_t max_bytes;
} cache_shard_t;

typedef struct cache {
    cache_shard_t *shards;
    size_t nshards;
    atomic_uint_fast64_t hits, misses, evictions;
} cache_t;

cache_t *cache_new(size_t max_bytes, size_t nshards);
void cache_free(cache_t *c);

/* Look up `key`. Returns a referenced entry (release with
 * cache_entry_unref) or NULL. Expired entries are removed on the way. */
cache_entry_t *cache_get(cache_t *c, const char *key, size_t key_len, uint64_t now_ms);
void cache_entry_unref(cache_entry_t *e);

/* Store a copy of `resp` under `key` for `ttl_ms`. Replaces any existing
 * entry. Responses larger than a shard's whole budget are not stored. */
bool cache_put(cache_t *c, const char *key, size_t key_len, const http_response_t *resp,
               uint64_t ttl_ms, uint64_t now_ms);

/* Remove one key; returns true if it existed. */
bool cache_delete(cache_t *c, const char *key, size_t key_len);

size_t cache_bytes(const cache_t *c);
size_t cache_entries(const cache_t *c);

#endif /* APROXY_CACHE_LRU_H */
