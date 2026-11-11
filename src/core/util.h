/*
 * util.h - small helpers shared by every module.
 *
 * Nothing in here knows about HTTP or sockets beyond what POSIX gives us;
 * it is the "stdlib extension" layer of the project.
 */
#ifndef APROXY_CORE_UTIL_H
#define APROXY_CORE_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* Handy sizing macros. */
#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

/* Mark a branch as unlikely so the hot path stays straight-line. */
#if defined(__GNUC__) || defined(__clang__)
#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define LIKELY(x) (x)
#define UNLIKELY(x) (x)
#endif

/* Monotonic clock in milliseconds; never goes backwards, used for timers. */
uint64_t now_ms(void);

/* Wall clock in seconds since the epoch; used for HTTP Date headers and
 * cache expiry calculations. */
uint64_t wall_s(void);

/* Put a file descriptor into non-blocking mode and set close-on-exec. */
int fd_set_nonblocking(int fd);
int fd_set_cloexec(int fd);

/* Case-insensitive comparison of two byte slices (HTTP header names). */
bool str_ieq(const char *a, size_t alen, const char *b, size_t blen);
/* Case-insensitive comparison of a slice with a NUL-terminated literal. */
bool str_ieq_lit(const char *a, size_t alen, const char *lit);

/* Parse a decimal unsigned integer from a slice. Returns false on any
 * non-digit, empty input or overflow. */
bool parse_u64(const char *s, size_t len, uint64_t *out);

/* Parse a size with an optional suffix: "256m", "1g", "512k", "100". */
bool parse_size(const char *s, size_t *out);

/* Parse a duration with optional suffix: "5s", "250ms", "2m", "1h". Result
 * is in milliseconds; a bare number means seconds (nginx convention). */
bool parse_duration_ms(const char *s, uint64_t *out);

/* Format the current time as an RFC 7231 IMF-fixdate into buf.
 * Returns the number of bytes written (always 29 if buf is large enough). */
size_t format_http_date(char *buf, size_t cap, uint64_t epoch_s);

/* FNV-1a 64-bit hash; good enough for the cache and consistent hashing. */
uint64_t hash_fnv1a(const void *data, size_t len);

/* Finalizer (murmur3 fmix64) that spreads entropy across all 64 bits.
 * FNV alone clusters similar short keys in the high bits, which matters
 * when hashes are compared as integers (consistent hashing ring). */
static inline uint64_t hash_mix64(uint64_t h)
{
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ull;
    h ^= h >> 33;
    return h;
}

/* Allocation wrappers that abort on OOM. The server cannot do anything
 * sensible without memory, so failing fast beats scattering NULL checks. */
void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t size);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);

#endif /* APROXY_CORE_UTIL_H */
