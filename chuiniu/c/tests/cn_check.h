/* The test harness every chuiniu/c test shares (pickemup/c/tests/pk_check.h's
 * shape): CHECK names the test and the line, counts assertions, and the
 * binary exits 1 on any failure so `make run` goes red. Tests may poke
 * CnGame fields directly to build a position; the kernel's structs are plain
 * data on purpose. */
#ifndef CN_CHECK_H
#define CN_CHECK_H

#include "../src/cn.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int         g_checks, g_fails;
static const char *g_test = "";

#define TEST(name) (g_test = (name))

/* A failure report is capped PER TEST, so a test that goes red after a
 * noisier one is still named (a mutation check reads it by name). */
static const char *g_named[128];
static int         g_named_fails[128];
static inline int first_fails_of(const char *test)
{
    int i = 0;
    while (i < 128 && g_named[i] && strcmp(g_named[i], test)) i++;
    if (i == 128) return 0;
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

/* xorshift64*, the tests' own randomness (never the game's) */
static uint64_t RS = 0x9e3779b97f4a7c15ull;
static inline uint32_t rnd(uint32_t n)
{
    RS ^= RS >> 12; RS ^= RS << 25; RS ^= RS >> 27;
    return n ? (uint32_t)(((RS * 2685821657736338717ull) >> 33) % n) : 0;
}

/* All 32 bytes from splitmix64 of k: a different game for every k. */
static inline void seed_wide(uint8_t seed[32], uint32_t k)
{
    uint64_t x = 0x9e3779b97f4a7c15ull * ((uint64_t)k + 1);
    for (int i = 0; i < 32; i += 8) {
        uint64_t z = (x += 0x9e3779b97f4a7c15ull);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        z ^= z >> 31;
        for (int j = 0; j < 8; j++) seed[i + j] = (uint8_t)(z >> (8 * j));
    }
}

static inline CnMove bid(int q, int f) { CnMove m = { (uint8_t)q, (uint8_t)f }; return m; }
static inline CnMove call_move(void) { CnMove m = { 0, 0 }; return m; }

/* THE RANDOM PLAYER: calls a standing bid about a third of the time, else
 * raises by the smallest step most of the time and jumps up to a few ranks
 * otherwise, so games finish in a realistic number of moves while every
 * kind of move is played. */
static inline CnMove bot_move(const CnGame *g)
{
    CnMove menu[CN_MAX_DICE * CN_BID_FACES + 1];
    int n = cn_legal(g, menu, (int)(sizeof menu / sizeof menu[0]));
    int first_bid = cn_can_call(g) ? 1 : 0;
    if (n == first_bid) return menu[0];                        /* only the call */
    if (first_bid && rnd(3) == 0) return menu[0];
    int span = n - first_bid;
    int k = rnd(4) ? (int)rnd(span < 2 ? span : 2) : (int)rnd(span < 8 ? span : 8);
    return menu[first_bid + k];
}

/* THE LONGEST GAME there can be (cn.h's CN_MAX_MOVES): every round bids
 * every rank in order, then calls the top bid. */
static inline CnMove long_move(const CnGame *g)
{
    int q, f;
    if (cn_min_raise(g, &q, &f)) return bid(q, f);
    return call_move();
}

#endif
