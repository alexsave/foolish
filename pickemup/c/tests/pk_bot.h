/* THE RANDOM TESTS' BOT, freestanding: the tests' own randomness, the wide
 * seeds and the bot that drives every random game (pk_fuzz, pk_msg_test, the
 * timeline's real games), with nothing but the kernel under them, so the same
 * games are played by the native tests and by a wasm32 build of the kernel
 * (tests/pk_cross.c, RULES_AND_KERNEL 7.3.7). pk_check.h includes it. */
#ifndef PK_BOT_H
#define PK_BOT_H

#include "../src/pk.h"
#include "../../../shared/c/stats/seed_hash.h"
#include <string.h>

/* xorshift64*, the tests' own randomness (never the game's) */
static uint64_t RS = 0x9e3779b97f4a7c15ull;
static inline uint32_t rnd(uint32_t n)
{
    RS ^= RS >> 12; RS ^= RS << 25; RS ^= RS >> 27;
    return n ? (uint32_t)(((RS * 2685821657736338717ull) >> 33) % n) : 0;
}

/* Many different deals (pk_check.h's seed_of has only 256): all 32 bytes
 * from splitmix64 of k (shared/c/stats/seed_hash.h). */
static inline void seed_wide(uint8_t seed[32], uint32_t k)
{
    seed_hash32(k, seed);
}

/* THE BOT the random tests drive: a legal action for a plausible seat,
 * weighted so games end (plays preferred, a pass after a draw most of the
 * time, "Last card!" half the time it is legal, a catch now and then, and
 * out-of-turn bubbles 8% of the time). 1 if it did something. */

static inline int bot_step(PkGame *g)
{
    if (g->over) return g->b_open ? pk_seal(g) : 0;
    PkAct m[PK_MAX_SEATS * 2 + PK_HAND_CAP * 4 + 4];
    int seat;
    if (g->b_open) {
        seat = g->b_sender;
        if (pk_can_seal(g) && (pk_turn_ended(g) || rnd(100) < 60)) return pk_seal(g);
    } else {
        seat = g->turn;
        if (rnd(100) < 8) {
            int o = (int)rnd(g->n);
            PkAct say = { PK_A_SAY_IT, 0, 0, 0 };
            if (pk_is_legal(g, o, say)) seat = o;
            else if (rnd(100) < 20) seat = o;
        }
    }
    int n = pk_legal(g, seat, m, (int)(sizeof m / sizeof m[0]));
    if (n == 0 && !g->b_open && seat != g->turn) {   /* nothing out of turn: the turn seat */
        seat = g->turn;
        n = pk_legal(g, seat, m, (int)(sizeof m / sizeof m[0]));
    }
    if (n == 0) return pk_can_seal(g) ? pk_seal(g) : 0;
    int nd = -1, np = 0, npass = -1, nsay = -1, ncall = 0;
    int plays[PK_HAND_CAP * 4 + 4], calls[PK_MAX_SEATS];
    for (int i = 0; i < n; i++)
        switch (m[i].kind) {
        case PK_A_DRAW: nd = i; break;
        case PK_A_PLAY: plays[np++] = i; break;
        case PK_A_PASS: npass = i; break;
        case PK_A_SAY_IT: nsay = i; break;
        case PK_A_CALL_OUT: calls[ncall++] = i; break;
        }
    int pick;
    if (nsay >= 0 && rnd(100) < 50) pick = nsay;
    else if (ncall && rnd(100) < 4) pick = calls[rnd((uint32_t)ncall)];
    else if (np && (nd < 0 || rnd(100) < 85)) pick = plays[rnd((uint32_t)np)];
    else if (nd >= 0 && (npass < 0 || rnd(100) < 40)) pick = nd;
    else if (npass >= 0) pick = npass;
    else if (ncall) pick = calls[rnd((uint32_t)ncall)];
    else if (nsay >= 0) pick = nsay;
    else return pk_can_seal(g) ? pk_seal(g) : 0;
    return pk_apply(g, seat, m[pick]);
}

#endif
