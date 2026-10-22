// t.h - the one assertion every adventurer test uses.
#ifndef ADV_T_H
#define ADV_T_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int t_fails = 0, t_checks = 0;

#define T_CHECK(cond)                                                                            \
    do {                                                                                         \
        t_checks++;                                                                              \
        if (!(cond)) {                                                                           \
            t_fails++;                                                                           \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                     \
        }                                                                                        \
    } while (0)
#define T_CHECK_MSG(cond, ...)                                                                   \
    do {                                                                                         \
        t_checks++;                                                                              \
        if (!(cond)) {                                                                           \
            t_fails++;                                                                           \
            fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);                     \
            fprintf(stderr, __VA_ARGS__);                                                        \
            fprintf(stderr, "\n");                                                               \
        }                                                                                        \
    } while (0)

static inline int t_done(const char *name) {
    if (t_fails) {
        fprintf(stderr, "%s: %d of %d checks FAILED\n", name, t_fails, t_checks);
        return 1;
    }
    printf("%s: %d checks ok\n", name, t_checks);
    return 0;
}

#endif
