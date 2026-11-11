/*
 * log.c - logging implementation (see log.h).
 */
#include "core/log.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

log_level_t g_log_level = LOG_LEVEL_INFO;

static const char *level_names[] = {"DEBUG", "INFO", "WARN", "ERROR", "NONE"};

void log_set_level(log_level_t level)
{
    g_log_level = level;
}

int log_level_from_name(const char *name)
{
    for (int i = 0; i <= LOG_LEVEL_NONE; i++) {
        if (strcasecmp(level_names[i], name) == 0)
            return i;
    }
    return -1;
}

/* Strip the directory so log lines show "http/h1_parser.c" rather than the
 * full absolute path CMake passes to the compiler. */
static const char *short_file(const char *file)
{
    const char *p = strstr(file, "src/");
    return p ? p + 4 : file;
}

void log_write(log_level_t level, const char *file, int line, const char *fmt, ...)
{
    char buf[2048];
    size_t off = 0;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    off += (size_t)snprintf(buf + off, sizeof(buf) - off, "%04d-%02d-%02d %02d:%02d:%02d.%03ld ",
                            tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
                            tm.tm_sec, ts.tv_nsec / 1000000);

    /* A short thread id makes it possible to follow one worker in the log. */
    unsigned long tid = (unsigned long)(uintptr_t)pthread_self();
    off += (size_t)snprintf(buf + off, sizeof(buf) - off, "[%-5s] [%04lx] %s:%d: ",
                            level_names[level], tid & 0xffff, short_file(file), line);

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + off, sizeof(buf) - off, fmt, ap);
    va_end(ap);
    if (n > 0)
        off += (size_t)n;
    if (off >= sizeof(buf) - 1)
        off = sizeof(buf) - 2;
    buf[off++] = '\n';

    /* One write call per line keeps lines atomic across threads. */
    ssize_t rc = write(STDERR_FILENO, buf, off);
    (void)rc;
}
