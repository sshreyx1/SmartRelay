/* testlib.h - tiny test harness: CHECK() records failures, RUN() runs a test
 * and prints PASS/FAIL. */
#ifndef TESTLIB_H
#define TESTLIB_H

#include <stdio.h>

static int g_fail;
static int g_checks;
static int g_tests;
static int g_tests_failed;

#define CHECK(cond)                                                            \
    do                                                                         \
    {                                                                          \
        g_checks++;                                                            \
        if (!(cond))                                                           \
        {                                                                      \
            g_fail++;                                                          \
            printf("    CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                      \
    } while (0)

#define RUN(fn)                                                                \
    do                                                                         \
    {                                                                          \
        int before = g_fail;                                                   \
        g_tests++;                                                             \
        fn();                                                                  \
        if (g_fail == before)                                                  \
        {                                                                      \
            printf("PASS  %s\n", #fn);                                         \
        }                                                                      \
        else                                                                   \
        {                                                                      \
            g_tests_failed++;                                                  \
            printf("FAIL  %s\n", #fn);                                         \
        }                                                                      \
    } while (0)

#define SUMMARY()                                                              \
    (printf("\n%d tests, %d failed, %d checks\n", g_tests, g_tests_failed,     \
            g_checks),                                                         \
     g_tests_failed ? 1 : 0)

#endif

/*** end of file ***/

