#ifndef ALTCELLO_TEST_COMMON_H
#define ALTCELLO_TEST_COMMON_H
#include <stdio.h>
static int g_fail = 0;
#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            g_fail++;                                                          \
            printf("FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond);           \
            printf(__VA_ARGS__);                                               \
            printf("\n");                                                      \
        }                                                                      \
    } while (0)
#define TEST_DONE(name)                                                        \
    do {                                                                       \
        if (g_fail) { printf("%s: %d FAILED\n", name, g_fail); return 1; }     \
        printf("%s: PASS\n", name);                                            \
        return 0;                                                              \
    } while (0)
#endif
