/* The harness every tallybones/c test shares (pickemup/c/tests/pk_check.h's
 * shape): CHECK names the test and the line, counts assertions, and the
 * binary exits 1 on any failure so `make run` goes red. Tests may poke
 * TbGame fields to build a position; the kernel's structs are plain data. */
#ifndef TB_CHECK_H
#define TB_CHECK_H

#include "../src/tb.h"
#include "../src/tb_code.h"
#include "../../../shared/c/sha256.h"
#include "../../../shared/c/deal_rng.h"
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

/* The test's own derivation, from DECISIONS.md T6/T11 and T14's byte order. */
static inline void spec_roll(const uint8_t seed[32], const uint8_t *body, int bl, int seat, int turn, int roll,
                      uint8_t out[5])
{
    Sha256 c;
    uint8_t d[32], w[2] = { (uint8_t)bl, (uint8_t)(bl >> 8) }, s = (uint8_t)seat, r = (uint8_t)roll;
    sha256_init(&c);
    sha256_update(&c, seed, 32);
    sha256_update(&c, w, 2);
    sha256_update(&c, body, (size_t)bl);
    sha256_update(&c, &s, 1);
    w[0] = (uint8_t)turn; w[1] = (uint8_t)(turn >> 8);
    sha256_update(&c, w, 2);
    sha256_update(&c, &r, 1);
    sha256_final(&c, d);
    DealRng rng;
    deal_rng_seed(&rng, d);
    for (int i = 0; i < 5; i++) out[i] = (uint8_t)(1 + deal_rng_bounded(&rng, 6));
}

/* Check the roll the newest bubble of `g` made against spec_roll over the
 * body the ENCODER writes for the history through it (tb_code_body, the
 * backward fold): the resident replay's forward body must be the same bytes.
 * `prev` is the game one bubble earlier. The count of dice checked. */
static inline int roll_is_the_spec(const TbGame *prev, const TbGame *g, int *bad)
{
    TbMove m = g->hist[g->hist_n - 1];
    int rolled = m.kind == TB_M_KEEP || g->turn != prev->turn || g->turns != prev->turns;
    if (!rolled || g->over) return 0;
    uint8_t body[TB_CODE_MAX], want[5];
    int bl = tb_code_body(g, g->hist_n, body, sizeof body);
    if (bl <= 0) { (*bad)++; return 0; }
    spec_roll(g->seed, body, bl, g->turn, g->turns, g->roll, want);
    for (int i = 0; i < 5; i++) {
        int kept = m.kind == TB_M_KEEP && (m.arg >> i & 1);
        if (g->dice[i] != (kept ? prev->dice[i] : want[i])) (*bad)++;
    }
    return 5;
}

#endif
