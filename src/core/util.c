/*
 * util.c - implementation of the small shared helpers (see util.h).
 */
#include "core/util.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

uint64_t wall_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec;
}

int fd_set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return -1;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        return -1;
    return 0;
}

int fd_set_cloexec(int fd)
{
    int flags = fcntl(fd, F_GETFD, 0);
    if (flags < 0)
        return -1;
    return fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

bool str_ieq(const char *a, size_t alen, const char *b, size_t blen)
{
    if (alen != blen)
        return false;
    for (size_t i = 0; i < alen; i++) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
            return false;
    }
    return true;
}

bool str_ieq_lit(const char *a, size_t alen, const char *lit)
{
    return str_ieq(a, alen, lit, strlen(lit));
}

bool parse_u64(const char *s, size_t len, uint64_t *out)
{
    if (len == 0)
        return false;
    uint64_t v = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9')
            return false;
        uint64_t d = (uint64_t)(s[i] - '0');
        /* Overflow check: v * 10 + d must fit in 64 bits. */
        if (v > (UINT64_MAX - d) / 10)
            return false;
        v = v * 10 + d;
    }
    *out = v;
    return true;
}

bool parse_size(const char *s, size_t *out)
{
    size_t len = strlen(s);
    if (len == 0)
        return false;
    uint64_t mult = 1;
    char suffix = (char)tolower((unsigned char)s[len - 1]);
    if (suffix == 'k' || suffix == 'm' || suffix == 'g') {
        mult = suffix == 'k' ? 1024ull : suffix == 'm' ? 1024ull * 1024 : 1024ull * 1024 * 1024;
        len--;
    }
    uint64_t v;
    if (!parse_u64(s, len, &v))
        return false;
    if (v > SIZE_MAX / mult)
        return false;
    *out = (size_t)(v * mult);
    return true;
}

bool parse_duration_ms(const char *s, uint64_t *out)
{
    size_t len = strlen(s);
    if (len == 0)
        return false;
    uint64_t mult = 1000; /* bare numbers are seconds */
    if (len >= 2 && s[len - 2] == 'm' && s[len - 1] == 's') {
        mult = 1;
        len -= 2;
    } else {
        char suffix = (char)tolower((unsigned char)s[len - 1]);
        if (suffix == 's') {
            mult = 1000;
            len--;
        } else if (suffix == 'm') {
            mult = 60ull * 1000;
            len--;
        } else if (suffix == 'h') {
            mult = 3600ull * 1000;
            len--;
        }
    }
    uint64_t v;
    if (!parse_u64(s, len, &v))
        return false;
    *out = v * mult;
    return true;
}

size_t format_http_date(char *buf, size_t cap, uint64_t epoch_s)
{
    static const char *days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    time_t t = (time_t)epoch_s;
    struct tm tm;
    gmtime_r(&t, &tm);
    int n = snprintf(buf, cap, "%s, %02d %s %04d %02d:%02d:%02d GMT", days[tm.tm_wday],
                     tm.tm_mday, months[tm.tm_mon], tm.tm_year + 1900, tm.tm_hour, tm.tm_min,
                     tm.tm_sec);
    return n < 0 ? 0 : (size_t)n;
}

uint64_t hash_fnv1a(const void *data, size_t len)
{
    const unsigned char *p = data;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

static void oom(void)
{
    fputs("fatal: out of memory\n", stderr);
    abort();
}

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p)
        oom();
    return p;
}

void *xcalloc(size_t n, size_t size)
{
    void *p = calloc(n ? n : 1, size ? size : 1);
    if (!p)
        oom();
    return p;
}

void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q)
        oom();
    return q;
}

char *xstrdup(const char *s)
{
    return xstrndup(s, strlen(s));
}

char *xstrndup(const char *s, size_t n)
{
    char *p = xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}
