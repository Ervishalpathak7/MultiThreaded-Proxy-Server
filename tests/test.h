/*
 * test.h - tiny header-only unit test harness.
 *
 * Usage:
 *     TEST(name) { ASSERT_EQ(1 + 1, 2); }
 *     int main(void) { RUN(name); return TEST_RESULT(); }
 *
 * No dependencies, works under the sanitizers, prints one line per test.
 */
#ifndef APROXY_TEST_H
#define APROXY_TEST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int test_failures = 0;
static int test_count = 0;
static int test_current_failed = 0;

#define TEST(name) static void test_##name(void)

#define RUN(name)                                                              \
    do {                                                                       \
        test_current_failed = 0;                                               \
        test_count++;                                                          \
        test_##name();                                                         \
        printf("%s %s\n", test_current_failed ? "FAIL" : "ok  ", #name);       \
        if (test_current_failed)                                               \
            test_failures++;                                                   \
    } while (0)

#define TEST_FAIL(...)                                                         \
    do {                                                                       \
        printf("    %s:%d: ", __FILE__, __LINE__);                             \
        printf(__VA_ARGS__);                                                   \
        printf("\n");                                                          \
        test_current_failed = 1;                                               \
    } while (0)

#define ASSERT(cond)                                                           \
    do {                                                                       \
        if (!(cond))                                                           \
            TEST_FAIL("assertion failed: %s", #cond);                          \
    } while (0)

#define ASSERT_EQ(a, b)                                                        \
    do {                                                                       \
        long long _a = (long long)(a), _b = (long long)(b);                    \
        if (_a != _b)                                                          \
            TEST_FAIL("%s == %s: got %lld, want %lld", #a, #b, _a, _b);        \
    } while (0)

#define ASSERT_STR_EQ(a, b)                                                    \
    do {                                                                       \
        const char *_a = (a), *_b = (b);                                       \
        if (strcmp(_a, _b) != 0)                                               \
            TEST_FAIL("%s: got \"%s\", want \"%s\"", #a, _a, _b);              \
    } while (0)

/* Compare a (ptr, len) slice with a string literal. */
#define ASSERT_SLICE_EQ(p, n, lit)                                             \
    do {                                                                       \
        size_t _n = (n);                                                       \
        if (_n != strlen(lit) || memcmp((p), (lit), _n) != 0)                  \
            TEST_FAIL("%s: got \"%.*s\", want \"%s\"", #p, (int)_n, (p), lit); \
    } while (0)

#define TEST_RESULT()                                                          \
    (printf("%d/%d tests passed\n", test_count - test_failures, test_count),   \
     test_failures ? 1 : 0)

#endif /* APROXY_TEST_H */
