/* The two-phone harness's assertions: one whole game played end to end
 * through a product's iOS bridge, one phone at a time, as a script of named
 * steps. STEP names the step in hand and OK counts an assertion, on the same
 * counters and report() as check.h.
 *
 * WHY NOT CHECK. A script is one long walk, not a list of independent
 * tests, so its failure report is capped once for the binary (the first 40
 * lines: past that a broken walk only repeats itself), not per name. And a
 * script may first rehearse a game with g_quiet set, looking for a seed
 * that reaches every step, before it plays that seed again for real: a
 * quiet OK still evaluates its condition, so a bridge call made inside one
 * still happens, but counts nothing and prints nothing.
 *
 * TWO PHONES, ONE RESIDENT SLOT is the design these scripts share: a bridge
 * keeps one resident message, as the extension's process does, so a phone
 * is what a device keeps between bubbles (its identity bytes, its nickname,
 * its seat records). Switching phone saves the records of the one put down,
 * loads the other's, drops the sender fact and adopts the newest bubble of
 * the thread, exactly as a tap does. Those calls are each product's own. */
#ifndef SHARED_TEST_TWOPHONE_H
#define SHARED_TEST_TWOPHONE_H

#include "check.h"

static int g_quiet;

#define STEP(name) TEST(name)
#define OK(c, ...) do {                                                          \
        int ok_ = (c) ? 1 : 0;                                                   \
        if (g_quiet) break;                                                      \
        g_checks++;                                                              \
        if (!ok_) {                                                              \
            g_fails++;                                                           \
            if (g_fails <= 40) {                                                 \
                fprintf(stderr, "FAIL %s:%d [%s] %s: ", __FILE__, __LINE__,      \
                        g_test, #c);                                             \
                fprintf(stderr, __VA_ARGS__);                                    \
                fputc('\n', stderr);                                             \
            }                                                                    \
        }                                                                        \
    } while (0)

#endif
