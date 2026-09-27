/* What every chuiniu/c test shares on top of shared/c/test/check.h (TEST,
 * CHECK, report): the tests' own randomness, seeds, moves and the random
 * players. Tests may poke CnGame fields directly to build a position; the
 * kernel's structs are plain data on purpose. */
#ifndef CN_CHECK_H
#define CN_CHECK_H

#include "../../../shared/c/test/check.h"
#include "../src/cn.h"
#include "../../../shared/c/stats/seed_hash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* xorshift64*, the tests' own randomness (never the game's) */
static uint64_t RS = 0x9e3779b97f4a7c15ull;
static inline uint32_t rnd(uint32_t n)
{
    RS ^= RS >> 12; RS ^= RS << 25; RS ^= RS >> 27;
    return n ? (uint32_t)(((RS * 2685821657736338717ull) >> 33) % n) : 0;
}

/* All 32 bytes from splitmix64 of k: a different game for every k
 * (shared/c/stats/seed_hash.h). */
static inline void seed_wide(uint8_t seed[32], uint32_t k)
{
    seed_hash32(k, seed);
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
