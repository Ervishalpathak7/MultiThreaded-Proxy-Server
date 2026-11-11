/*
 * log.h - levelled, thread-safe logging to stderr.
 *
 * Each line is built in a stack buffer and emitted with a single write(2),
 * so lines from different worker threads never interleave. The level check
 * happens in the macro, so disabled levels cost one branch.
 */
#ifndef APROXY_CORE_LOG_H
#define APROXY_CORE_LOG_H

typedef enum {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARN,
    LOG_LEVEL_ERROR,
    LOG_LEVEL_NONE,
} log_level_t;

extern log_level_t g_log_level;

void log_set_level(log_level_t level);
/* Parse "debug" / "info" / "warn" / "error"; returns -1 on unknown names. */
int log_level_from_name(const char *name);

void log_write(log_level_t level, const char *file, int line, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

#define LOG_AT(lvl, ...)                                                   \
    do {                                                                   \
        if ((lvl) >= g_log_level)                                          \
            log_write((lvl), __FILE__, __LINE__, __VA_ARGS__);             \
    } while (0)

#define LOG_DEBUG(...) LOG_AT(LOG_LEVEL_DEBUG, __VA_ARGS__)
#define LOG_INFO(...) LOG_AT(LOG_LEVEL_INFO, __VA_ARGS__)
#define LOG_WARN(...) LOG_AT(LOG_LEVEL_WARN, __VA_ARGS__)
#define LOG_ERROR(...) LOG_AT(LOG_LEVEL_ERROR, __VA_ARGS__)

#endif /* APROXY_CORE_LOG_H */
