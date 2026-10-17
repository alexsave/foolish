/* rng_legacy_test.c - the generators rng.h replaces, verbatim, beside rng.h.
 *
 * Each function below is copied unchanged from its file on main before the
 * move. Some of these sequences are re-derived from data already in people's
 * hands (the legacy LCG deal and the strategy stream from a stored seed, a
 * die's throw from a game's seed, a bot's move where a peer replays it), so
 * each site moves only if rng.h gives the SAME sequence for every seed. This
 * runs both over many seeds and many draws and fails on the first difference.
 *
 * It exists for the move: it is deleted with the old copies, after which
 * rng_test.c's published vectors are what pin each algorithm.
 *
 *   cc -std=c11 -O2 -Wall -Wextra -Werror rng_legacy_test.c -o t && ./t */
#include <stdint.h>
#include "test/check.h"
#include "rng.h"

/* foolish/c/src/game.c game_random_u32 (g_seed) and random_strategy_random
 * (g_rand_seed), and cordite_strategy.c cd_fm_sample_table: one recurrence */
static uint32_t old_game_lcg(uint32_t *g_seed) { *g_seed = *g_seed * 1664525u + 1013904223u; return *g_seed; }

/* foolish/c/src/{analyse,astrolite_strategy,blackpowder_strategy,
 * cordite_strategy,octogen_strategy,robusta_strategy}.c, value style */
static uint32_t old_xorshift32(uint32_t s) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
/* ... as those files spell it, with the step's zero guard (dead once the
 * caller's seed is not 0: xorshift maps nonzero to nonzero) */
static uint32_t old_cd_xorshift(uint32_t s) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s ? s : 0xB1A570u; }
/* foolish/c/src/analyse.c an_xorshift: there the guard is live for seed 0 */
static uint32_t old_an_xorshift(uint32_t s) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s ? s : 0x9E3779B9u; }
/* foolish/c/tools/endgame_retro/{find_crawl,narrow_path,trace_path}.c mkseed */
static void old_mkseed(long idx, uint8_t *seed) { uint64_t x=(uint64_t)idx*0x9E3779B97F4A7C15ull+0x1234567; for(int i=0;i<32;i++){x^=x<<13;x^=x>>7;x^=x<<17;seed[i]=(uint8_t)(x&0xff);} }
/* foolish/c/src/bot_drive.c xs32, pointer style */
static uint32_t old_xs32(uint32_t *s) { uint32_t x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return *s = x; }

/* chuiniu/c/src/cn_roll.c mix + unit, chuiniu/c/bot/cn_bot.h cn_splitmix,
 * uttt/c/src/uttt_big_bot.c splitmix: one splitmix64 step */
static uint64_t old_splitmix(uint64_t *s)
{
    uint64_t z = (*s += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}
static double old_unit(uint64_t *s) { return (double)(old_splitmix(s) >> 11) * (1.0 / 9007199254740992.0); }

/* pickemup/c/src/pk_bot.c and pk_belief.c mix64 */
static uint64_t old_mix64(uint64_t z)
{
    z += 0x9e3779b97f4a7c15ull;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}
/* uttt/c/src/uttt_analyse.c mix */
static uint64_t old_uttt_mix(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    x ^= x >> 31;
    return x ? x : 1;
}
/* pickemup/c/src/pk_bot.c pk_bot_rand */
static uint64_t old_pk_bot_rand(uint64_t *rng)
{
    uint64_t x = *rng ? *rng : 0x9e3779b97f4a7c15ull;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    *rng = x;
    return x * 2685821657736338717ull;
}
/* uttt/c/src/uttt_bots.c rnd and f_rnd */
static uint32_t old_uttt_rnd(uint64_t *s, uint32_t n)
{
    uint64_t x = *s;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27; *s = x;
    uint32_t v = (uint32_t)((x * 2685821657736338717ull) >> 33);
    if ((n & (n - 1)) == 0) return v & (n - 1);
    return v % n;
}
static uint32_t old_uttt_f_rnd(uint64_t *s, uint32_t n)
{
    uint64_t x = *s;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27; *s = x;
    uint32_t v = (uint32_t)((x * 2685821657736338717ull) >> 32);
    return (uint32_t)(((uint64_t)v * n) >> 32);
}
/* uttt/c/src/uttt_big_bot.c next + below, and its init */
static uint64_t old_big_next(uint64_t *s)
{
    uint64_t x = *s;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    *s = x;
    return x * 0x2545f4914f6cdd1dull;
}
static uint32_t old_big_below(uint64_t *s, uint32_t n) { return (uint32_t)(((old_big_next(s) >> 32) * (uint64_t)n) >> 32); }
static uint64_t old_big_init(uint64_t seed)
{
    uint64_t s = seed, r = old_splitmix(&s);
    if (!r) r = 0x9e3779b97f4a7c15ull;
    return r;
}
/* foolish/foolyard/src/rng.c rng_seed + rng_next */
static uint64_t old_fy_seed(uint64_t seed)
{
    seed += 0x9E3779B97F4A7C15ull;
    seed = (seed ^ (seed >> 30)) * 0xBF58476D1CE4E5B9ull;
    seed = (seed ^ (seed >> 27)) * 0x94D049BB133111EBull;
    seed ^= seed >> 31;
    return seed ? seed : 0x2545F4914F6CDD1Dull;
}
static uint32_t old_fy_next(uint64_t *state)
{
    uint64_t x = *state;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    *state = x;
    return (uint32_t)(x >> 32);
}

/* The new side of each, as the migrated sites now spell it */
static uint64_t new_pk_bot_rand(uint64_t *rng) { if (!*rng) *rng = RNG_GOLDEN; return rng_xorshift64star(rng); }
static uint32_t new_uttt_rnd(uint64_t *s, uint32_t n)
{
    uint32_t v = (uint32_t)(rng_xorshift64star(s) >> 33);
    if ((n & (n - 1)) == 0) return v & (n - 1);
    return v % n;
}
static uint32_t new_uttt_f_rnd(uint64_t *s, uint32_t n)
{
    uint32_t v = (uint32_t)(rng_xorshift64star(s) >> 32);
    return (uint32_t)(((uint64_t)v * n) >> 32);
}
static uint32_t new_big_below(uint64_t *s, uint32_t n) { return (uint32_t)(((rng_xorshift64star(s) >> 32) * (uint64_t)n) >> 32); }
static uint64_t new_big_init(uint64_t seed) { uint64_t s = seed, r = rng_splitmix64(&s); return r ? r : RNG_GOLDEN; }
static double   new_unit(uint64_t *s) { return rng_unit(rng_splitmix64(s)); }
static void new_mkseed(long idx, uint8_t seed[32])
{
    uint64_t x = (uint64_t)idx * RNG_GOLDEN + 0x1234567;
    for (int i = 0; i < 32; i++) seed[i] = (uint8_t)(rng_xorshift64(&x) & 0xff);
}
static uint32_t new_an_step(uint32_t *s) { if (!rng_xorshift32(s)) *s = 0x9E3779B9u; return *s; }
static uint64_t new_uttt_mix(uint64_t x) { uint64_t z = rng_splitmix64_at(x); return z ? z : 1; }

#define SEEDS 200000
#define DRAWS 64

int main(void)
{
    TEST("every replaced generator equals rng.h, seed by seed and draw by draw");
    uint64_t seed_src = 0x0123456789abcdefull;
    long diffs = 0;
    for (int i = 0; i < SEEDS; i++) {
        /* seeds: the small ones, the edges, then a spread */
        uint64_t s64 = i < 64 ? (uint64_t)i : i < 72 ? ~(uint64_t)(i - 64) : rng_splitmix64(&seed_src);
        /* and the one seed splitmix64 mixes to 0, where the fallbacks live */
        if (i == 72) s64 = (uint64_t)0 - RNG_GOLDEN;
        uint32_t s32 = (uint32_t)s64 ^ (uint32_t)(s64 >> 32);

        uint32_t a32 = s32, b32 = s32, c32 = s32, d32 = s32, e32 = s32;
        uint64_t a = s64, b = s64, c = s64, d = s64, e = s64, f = s64, g = s64, h = s64, k = s64, l = s64;
        uint64_t fa = s64 ? s64 : 1, fb = fa;
        uint64_t fy_a = old_fy_seed(s64), fy_b = rng_seed64(s64);
        diffs += fy_a != fy_b;
        diffs += old_big_init(s64) != new_big_init(s64);
        diffs += old_mix64(s64) != rng_splitmix64_at(s64);
        diffs += old_uttt_mix(s64) != new_uttt_mix(s64);
        {
            uint8_t o[32], nw[32];
            old_mkseed((long)s64, o); new_mkseed((long)s64, nw);
            diffs += memcmp(o, nw, 32) != 0;
        }
        uint32_t cd_a = s32 ? s32 : 0xCAFEu, cd_b = cd_a, an_a = s32, an_b = s32;
        for (int j = 0; j < DRAWS; j++) {
            uint32_t n = 1u + (uint32_t)j * 7u;   /* bounds that are and are not powers of two */
            diffs += old_game_lcg(&a32) != rng_lcg32(&b32);
            uint32_t ov = old_xorshift32(c32); c32 = ov;
            diffs += ov != rng_xorshift32(&d32);
            diffs += old_xs32(&e32) != d32;
            diffs += old_splitmix(&a) != rng_splitmix64(&b);
            cd_a = old_cd_xorshift(cd_a);
            diffs += cd_a != rng_xorshift32(&cd_b);
            an_a = old_an_xorshift(an_a);
            diffs += an_a != new_an_step(&an_b);
            diffs += old_unit(&c) != new_unit(&d);
            diffs += old_pk_bot_rand(&e) != new_pk_bot_rand(&f);
            diffs += old_uttt_rnd(&g, n) != new_uttt_rnd(&h, n);
            diffs += old_uttt_f_rnd(&k, n) != new_uttt_f_rnd(&l, n);
            diffs += old_big_below(&fa, n) != new_big_below(&fb, n);
            diffs += old_fy_next(&fy_a) != (uint32_t)(rng_xorshift64(&fy_b) >> 32);
        }
        CHECK(a32 == b32 && c32 == d32 && a == b && c == d && e == f && g == h && k == l && fa == fb && fy_a == fy_b,
              "seed %d: the states agree after %d draws", i, DRAWS);
    }
    CHECK(diffs == 0, "%ld outputs differ", diffs);
    printf("rng_legacy: %d seeds x %d draws, 19 generators: %ld differences\n", SEEDS, DRAWS, diffs);
    return report("rng_legacy");
}
