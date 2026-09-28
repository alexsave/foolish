/* seed_hash.h - a game's seed from who is playing and which game it is.
 *
 * An arena plays game i of some pairing on a seed that is a pure function of
 * (identity, i), so a run is reproducible and can be cut into shards that
 * pool exactly. The mixing is splitmix64 (Steele, Lea and Flood, "Fast
 * splittable pseudorandom number generators", 2014): the state steps by the
 * golden-ratio increment and each output is the state through the 64-bit
 * finaliser. A kernel's `new` takes 32 seed bytes, so four outputs fill them,
 * each written little-endian.
 *
 * Header only: two short pure functions used by arenas and by test helpers,
 * so nothing has to be added to a build list to reach them. */
#ifndef SHARED_SEED_HASH_H
#define SHARED_SEED_HASH_H

#include <stdint.h>

#define SEED_HASH_GOLDEN 0x9e3779b97f4a7c15ull

/* One splitmix64 step: advance *state and return the mixed output. */
static inline uint64_t seed_splitmix64(uint64_t *state)
{
    uint64_t z = (*state += SEED_HASH_GOLDEN);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

/* Four steps from *state into 32 bytes, little-endian. *state is left after
 * the fourth step, so a caller can keep drawing from the same stream. */
static inline void seed_hash32_from(uint64_t *state, uint8_t out[32])
{
    for (int k = 0; k < 32; k += 8) {
        uint64_t z = seed_splitmix64(state);
        for (int j = 0; j < 8; j++) out[k + j] = (uint8_t)(z >> (8 * j));
    }
}

/* 32 seed bytes for a 64-bit key: the stream starts at GOLDEN * (key + 1),
 * so key 0 is not the all-zero state. A caller folds its identity and game
 * index into the key however it likes. */
static inline void seed_hash32(uint64_t key, uint8_t out[32])
{
    uint64_t x = SEED_HASH_GOLDEN * (key + 1);
    seed_hash32_from(&x, out);
}

#endif
