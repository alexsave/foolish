/* Pick 'Em Up - card ids and the one shuffle (RULES_AND_KERNEL.md 3.2, 3.5).
 *
 * ONE RNG, ONE KEYSTREAM: shared/c/deal_rng (ChaCha20) keyed by the game's
 * 32-byte seed, used unchanged. The initial shuffle reads from block 0;
 * reshuffle r reads from block r << 32 (D21), so every reshuffle is its own
 * disjoint stretch of the one stream and a phone that never computed
 * reshuffle r-1 can compute reshuffle r in O(1). Nothing about a shuffle is
 * on the wire. */
#include "pk.h"
#include "../../../shared/c/deal_rng.h"

int pk_rank(uint8_t c)
{
    if (c >= PK_DECK) return 0;
    if (c >= 100) return PK_R_WILD4;
    if (c >= 96) return PK_R_WILD;
    int k = c % 24;
    if (k < 18) return k / 2 + 1;
    if (k < 20) return PK_R_SKIP;
    if (k < 22) return PK_R_REVERSE;
    return PK_R_PLUS2;
}

uint64_t pk_reshuffle_block(int r)
{
    return (uint64_t)(uint32_t)r << 32;
}

/* Fisher-Yates, top of the array last:
 *     for i = m-1 down to 1: j = bounded(i + 1); swap a[i], a[j]
 * THE LOOP IS PART OF THE FORMAT (4.7): its direction, its bound and the
 * one draw per step decide every deal there will ever be. */
uint64_t pk_shuffle(uint8_t *a, int m, const uint8_t seed[32], uint64_t block)
{
    DealRng rng;
    deal_rng_seed_at(&rng, seed, block);
    for (int i = m - 1; i >= 1; i--) {
        uint32_t j = deal_rng_bounded(&rng, (uint32_t)i + 1u);
        uint8_t t = a[i];
        a[i] = a[j];
        a[j] = t;
    }
    /* The counter names the next block the generator would read. */
    return (uint64_t)rng.state[12] | ((uint64_t)rng.state[13] << 32);
}
