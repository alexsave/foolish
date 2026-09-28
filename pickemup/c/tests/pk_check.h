/* What every pickemup/c test shares on top of shared/c/test/check.h (TEST,
 * CHECK, report): actions, card ids and hand-built tables. Tests may poke
 * PkGame fields directly to build a position; the kernel's structs are plain
 * data on purpose. */
#ifndef PK_CHECK_H
#define PK_CHECK_H

#include "../../../shared/c/test/check.h"
#include "../src/pk.h"
#include "../src/pk_plan.h"
#include "pk_bot.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* seed_of HAS ONLY 256 DEALS: every byte is f(k) + 7i with f(k) a byte, so k
 * and any k' with f(k') == f(k) deal the same game. Kept as it is because
 * the committed goldens (7.3) are read from it; many different deals come
 * from pk_bot.h's seed_wide. */
static inline void seed_of(uint8_t seed[32], uint32_t k)
{
    for (int i = 0; i < 32; i++) seed[i] = (uint8_t)(k * 131u + (uint32_t)i * 7u + (k >> 8) * 17u);
}

static inline PkAct act(int kind, int a, int b)
{
    PkAct x = { (uint8_t)kind, (uint8_t)a, (uint8_t)b, 0 };
    return x;
}
#define DRAW      act(PK_A_DRAW, 0, 0)
#define PASS      act(PK_A_PASS, 0, 0)
#define SAY       act(PK_A_SAY_IT, 0, 0)
#define PLAY(p)   act(PK_A_PLAY, (p), PK_NO_SUIT)
#define PLAYW(p, s) act(PK_A_PLAY, (p), (s))
#define CALL(t)   act(PK_A_CALL_OUT, (t), 0)

/* Card ids by rule: suit s, number r (copy c), or an action. */
static inline uint8_t num(int s, int r, int c) { return (uint8_t)(s * 24 + (r - 1) * 2 + c); }
static inline uint8_t skip_(int s, int c)      { return (uint8_t)(s * 24 + 18 + c); }
static inline uint8_t rev_(int s, int c)       { return (uint8_t)(s * 24 + 20 + c); }
static inline uint8_t plus2_(int s, int c)     { return (uint8_t)(s * 24 + 22 + c); }
static inline uint8_t wild_(int c)             { return (uint8_t)(96 + c); }
static inline uint8_t wild4_(int c)            { return (uint8_t)(100 + c); }

/* A hand-built table: `n` seats, the given top card and live suit, every
 * other card in the deck (in id order, top last), empty hands. Tests then
 * move cards from the deck into hands with give(). Built from pk_new so the
 * seed and every counter start as a real game's. */
static inline void table(PkGame *g, int n, uint8_t top, int live)
{
    uint8_t seed[32];
    seed_of(seed, 1);
    pk_new(g, seed, n);
    memset(g->hand_n, 0, sizeof g->hand_n);
    g->deck_n = 0;
    for (int c = 0; c < PK_DECK; c++)
        if (c != top) g->deck[g->deck_n++] = (uint8_t)c;
    g->stack[0] = top;
    g->stack_n = 1;
    g->live_suit = (uint8_t)live;
    g->turn = 0;
    g->dir = 1;
}

/* Move card `c` from wherever it is in the deck to the end of seat s's hand. */
static inline void give(PkGame *g, int s, uint8_t c)
{
    for (int i = 0; i < g->deck_n; i++)
        if (g->deck[i] == c) {
            for (int j = i; j + 1 < g->deck_n; j++) g->deck[j] = g->deck[j + 1];
            g->deck_n--;
            g->hand[s][g->hand_n[s]++] = c;
            return;
        }
    fprintf(stderr, "give: card %d not in the deck\n", c);
    exit(2);
}

/* Put card `c` (from the deck) on top of the deck. */
static inline void deck_top(PkGame *g, uint8_t c)
{
    for (int i = 0; i < g->deck_n; i++)
        if (g->deck[i] == c) {
            for (int j = i; j + 1 < g->deck_n; j++) g->deck[j] = g->deck[j + 1];
            g->deck[g->deck_n - 1] = c;
            return;
        }
    fprintf(stderr, "deck_top: card %d not in the deck\n", c);
    exit(2);
}

/* Every card is in exactly one place: hands, deck, stack. */
static inline int conserved(const PkGame *g)
{
    int seen[PK_DECK] = { 0 }, total = g->deck_n + g->stack_n;
    for (int i = 0; i < g->deck_n; i++) if (g->deck[i] < PK_DECK) seen[g->deck[i]]++;
    for (int i = 0; i < g->stack_n; i++) if (g->stack[i] < PK_DECK) seen[g->stack[i]]++;
    for (int s = 0; s < g->n; s++) {
        total += g->hand_n[s];
        for (int i = 0; i < g->hand_n[s]; i++) if (g->hand[s][i] < PK_DECK) seen[g->hand[s][i]]++;
    }
    if (total != PK_DECK) return 0;
    for (int c = 0; c < PK_DECK; c++) if (seen[c] != 1) return 0;
    return 1;
}

/* exposed and said only ever mark seats on exactly one card (3.3) */
static inline int one_rep(const PkGame *g)
{
    for (int s = 0; s < g->n; s++)
        if (((g->exposed | g->said) >> s & 1) && g->hand_n[s] != 1) return 0;
    return 1;
}

/* A table whose deck is empty and whose stack holds `m` cards under `top`;
 * everything else is in seat 1's hand. Seat 0 holds two non-matching cards. */
static inline void empty_deck(PkGame *g, int m, uint8_t top, int live)
{
    table(g, 2, top, live);
    give(g, 0, num(3, 1, 0));
    give(g, 0, num(3, 1, 1));
    g->stack_n = 0;
    for (int i = 0; i < m; i++) {
        uint8_t c = g->deck[g->deck_n - 1];
        g->deck_n--;
        g->stack[g->stack_n++] = c;
    }
    g->stack[g->stack_n++] = top;
    while (g->deck_n) give(g, 1, g->deck[g->deck_n - 1]);
}

/* Three seats, seat 0 plays down to one card and seals; turn is seat 1. */
static inline void exposed3(PkGame *g)
{
    table(g, 3, num(0, 5, 0), 0);
    give(g, 0, num(0, 6, 0)); give(g, 0, num(1, 1, 0));
    give(g, 1, num(0, 7, 0)); give(g, 1, num(2, 2, 0)); give(g, 1, num(3, 3, 0));
    give(g, 2, num(0, 8, 0)); give(g, 2, num(2, 4, 0)); give(g, 2, num(3, 6, 0));
    pk_apply(g, 0, PLAY(0));
    pk_seal(g);
}

#endif
