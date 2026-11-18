#ifndef JOBBOT_TEST_H
#define JOBBOT_TEST_H

#include <stdio.h>
#include <string.h>

static int t_checks;
static int t_failed;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        t_checks++;                                                                 \
        if (!(cond)) {                                                              \
            t_failed++;                                                             \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                           \
    } while (0)

/* Compares two strings; NULL only equals NULL. */
#define CHECK_STR(actual, expected)                                                       \
    do {                                                                                  \
        const char *t_a = (actual);                                                       \
        const char *t_e = (expected);                                                     \
        t_checks++;                                                                       \
        if (!((t_a == NULL && t_e == NULL) || (t_a && t_e && strcmp(t_a, t_e) == 0))) {   \
            t_failed++;                                                                   \
            fprintf(stderr, "%s:%d: CHECK_STR failed: %s\n  actual:   %s\n  expected: %s\n", \
                    __FILE__, __LINE__, #actual, t_a ? t_a : "(null)", t_e ? t_e : "(null)"); \
        }                                                                                 \
    } while (0)

#define CHECK_INT(actual, expected)                                                  \
    do {                                                                             \
        long long t_a = (long long)(actual);                                         \
        long long t_e = (long long)(expected);                                       \
        t_checks++;                                                                  \
        if (t_a != t_e) {                                                            \
            t_failed++;                                                              \
            fprintf(stderr, "%s:%d: CHECK_INT failed: %s\n  actual:   %lld\n  expected: %lld\n", \
                    __FILE__, __LINE__, #actual, t_a, t_e);                          \
        }                                                                            \
    } while (0)

static inline int test_report(void)
{
    printf("%d checks, %d failed\n", t_checks, t_failed);
    return t_failed ? 1 : 0;
}

#endif
