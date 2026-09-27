/* The kernel test harness: TEST names the test in hand, CHECK counts an
 * assertion and names the test and the line of a failure, and report()
 * prints the tally and hands main its exit status, 1 on any failure, so
 * `make run` goes red. Header-only and test-only: nothing here ships.
 *
 * A product's own test header includes this one first, by relative path,
 * and adds what pokes its own game struct (action builders, a hand-built
 * table, seed makers). Tests may poke those structs directly to build a
 * position; a kernel's structs are plain data on purpose. */
#ifndef SHARED_TEST_CHECK_H
#define SHARED_TEST_CHECK_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int         g_checks, g_fails;
static const char *g_test = "";

#define TEST(name) (g_test = (name))

/* A failure report is capped PER TEST, not per binary, so a test that goes
 * red after a noisier one is still named in the output (a mutation check
 * reads it by name). The first CHECK_NAMED_CAP distinct test names that
 * fail each print their first 5 failures; a name past the cap still counts
 * in g_fails but prints nothing. */
#ifndef CHECK_NAMED_CAP
#define CHECK_NAMED_CAP 128
#endif

static const char *g_named[CHECK_NAMED_CAP];
static int         g_named_fails[CHECK_NAMED_CAP];
static inline int first_fails_of(const char *test)
{
    int i = 0;
    while (i < CHECK_NAMED_CAP && g_named[i] && strcmp(g_named[i], test)) i++;
    if (i == CHECK_NAMED_CAP) return 0;
    g_named[i] = test;
    return ++g_named_fails[i] <= 5;
}

#define CHECK(cond, ...) do {                                                   \
        g_checks++;                                                             \
        if (!(cond)) {                                                          \
            g_fails++;                                                          \
            if (first_fails_of(g_test)) {                                       \
                fprintf(stderr, "FAIL %s:%d [%s] %s: ", __FILE__, __LINE__,     \
                        g_test, #cond);                                         \
                fprintf(stderr, __VA_ARGS__);                                   \
                fprintf(stderr, "\n");                                          \
            }                                                                   \
        }                                                                       \
    } while (0)

static inline int report(const char *what)
{
    printf("%s: %d assertions, %d failed\n", what, g_checks, g_fails);
    return g_fails ? 1 : 0;
}

#endif
