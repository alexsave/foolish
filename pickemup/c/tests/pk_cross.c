/* 7.3.7, NATIVE AGAINST WASM: the same games played by this machine's build
 * of the kernel and by a wasm32 build of it must end in the same states.
 *
 * Each game is a deal of the fuzz harness (seed_wide(900000 + k) at 2 + k % 7
 * players) played to its end by the random tests' bot (pk_bot.h). What is
 * compared is one 64-bit value a game: the kernel's own hash of the final
 * state (pk_hash: hands, deck, stack, flags, counters, the history), folded
 * with every event of the whole game's plan as the all-seeing viewer sees it
 * (pk_plan_each), so the deal, every reshuffle's order, every penalty and the
 * replay that the plan runs are all in it. The bot's randomness is the
 * tests' xorshift, 64-bit integer arithmetic only, so both engines choose the
 * same actions.
 *
 * The same file is both sides. Natively it is a program that prints one hex
 * line a game; for wasm32 (-DPK_CROSS_WASM, freestanding, shared/c/wasm's
 * libc) it exports pk_cross_run and pk_cross_hashes, and tests/pk_cross.mjs
 * prints the same lines from node. `make -C pickemup/c cross` compares them.
 */
#include "pk_bot.h"
#include "../src/pk_plan.h"

#define CROSS_MAX 1000
static uint64_t HASHES[CROSS_MAX];

static void fold(uint64_t *h, uint32_t v)
{
    for (int i = 0; i < 4; i++) {
        *h ^= (uint8_t)(v >> (8 * i));
        *h *= 0x100000001b3ull;
    }
}

static void fold_event(const PkEvent *e, void *ctx)
{
    uint64_t *h = ctx;
    fold(h, (uint32_t)e->kind | (uint32_t)e->half << 8 | (uint32_t)e->seat << 16 | (uint32_t)e->other << 24);
    fold(h, (uint32_t)e->card | (uint32_t)e->n << 8 | (uint32_t)e->i << 16 | (uint32_t)e->suit << 24);
    fold(h, (uint32_t)e->dir | (uint32_t)e->deck_n << 8 | (uint32_t)e->step << 16);
    fold(h, e->bubble);
}

/* Play games 0..games-1 into HASHES; the count played. */
int pk_cross_run(int games);
int pk_cross_run(int games)
{
    static PkGame g;
    if (games > CROSS_MAX) games = CROSS_MAX;
    for (int k = 0; k < games; k++) {
        uint8_t seed[32];
        seed_wide(seed, 900000u + (uint32_t)k);
        RS = UINT64_C(0x9e3779b97f4a7c15) ^ (uint64_t)k;
        if (!pk_new(&g, seed, 2 + k % 7)) return k;
        for (int steps = 0; steps < 20000 && !(g.over && !g.b_open); steps++)
            if (!bot_step(&g)) break;
        uint64_t h = pk_hash(&g);
        fold(&h, (uint32_t)g.bubbles | (uint32_t)g.over << 16 | (uint32_t)g.winner << 24);
        if (pk_plan_each(&g, PK_VIEW_ALL, -1, g.bubbles, fold_event, &h) < 0) h = 0;
        HASHES[k] = h;
    }
    return games;
}

/* Where the values are, for the wasm host to read out of memory. */
const uint64_t *pk_cross_hashes(void);
const uint64_t *pk_cross_hashes(void) { return HASHES; }

#ifndef PK_CROSS_WASM
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    int games = argc > 1 ? atoi(argv[1]) : 100;
    int n = pk_cross_run(games);
    for (int k = 0; k < n; k++) printf("%d %016llx\n", k, (unsigned long long)HASHES[k]);
    return n == games ? 0 : 1;
}
#endif
