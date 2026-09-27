/* The harness every tallybones/c test shares (pickemup/c/tests/pk_check.h's
 * shape): CHECK names the test and the line, counts assertions, and the
 * binary exits 1 on any failure so `make run` goes red. Tests may poke
 * TbGame fields to build a position; the kernel's structs are plain data. */
#ifndef TB_CHECK_H
#define TB_CHECK_H

#include "../src/tb.h"
#include "../src/tb_code.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int         g_checks, g_fails;
static const char *g_test = "";

#define TEST(name) (g_test = (name))

static const char *g_named[64];
static int         g_named_fails[64];
static inline int first_fails_of(const char *test)
{
    int i = 0;
    while (i < 64 && g_named[i] && strcmp(g_named[i], test)) i++;
    if (i == 64) return 0;
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

/* All 32 bytes from splitmix64 of k. */
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

static inline TbMove mv(int kind, int seat, int arg)
{
    TbMove m = { (uint8_t)kind, (uint8_t)seat, (uint8_t)arg, 0 };
    return m;
}

/* The random bot: a move off the menu, a LEAVE about once in `leave_odds`
 * bubbles (never, for 0). */
static inline TbMove bot_move(const TbGame *g, int leave_odds)
{
    TbMove m[TB_MENU_MAX];
    int n = tb_menu(g, m, TB_MENU_MAX), play = 0;
    while (play < n && m[play].kind != TB_M_LEAVE) play++;
    if (leave_odds > 0 && n > play && rnd((uint32_t)leave_odds) == 0) return m[play + rnd((uint32_t)(n - play))];
    /* a turn goes to a SCORE about a third of the time it may still reroll */
    int keeps = 0;
    while (keeps < play && m[keeps].kind == TB_M_KEEP) keeps++;
    if (keeps && rnd(3)) return m[rnd((uint32_t)keeps)];
    return m[keeps + rnd((uint32_t)(play - keeps))];
}

/* Play `g` forward by one bot move, resident (every roll derived). */
static inline int bot_step(TbGame *g, int leave_odds)
{
    if (g->over) return 0;
    TbMove m = bot_move(g, leave_odds);
    static TbMove h[TB_HIST_CAP];
    memcpy(h, g->hist, sizeof(TbMove) * g->hist_n);
    h[g->hist_n] = m;
    return tb_replay(g, g->seed, g->n, g->starter, h, g->hist_n + 1);
}

#endif
