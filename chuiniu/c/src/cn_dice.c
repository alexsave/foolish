/* Chui Niu - the dice (DECISIONS K2, K3).
 *
 * THE DICE ARE DERIVED, NEVER ROLLED AND NEVER SENT. Every round's dice for
 * every seat are a pure function of state that is already in the thread:
 *
 *   log  = SHA-256("chuiniu.log.1|"  || q0 f0 q1 f1 ... )     hist[0 .. round_at)
 *   key  = SHA-256("chuiniu.dice.1|" || seed[32] || round u16 LE || log[32])
 *   die i of seat s = 1 + deal_rng_bounded(6), the i-th draw of
 *                     shared/c/deal_rng keyed by `key` at block s << 32
 *
 * round_at is where the round opened: 0 for round 0, just after the call that
 * closed the previous round otherwise. So a round's dice are fixed by the
 * moment it opens, and nothing a player stages or cancels afterwards can
 * change them; the only lever is a different legal move before the round
 * opened, which is already sent and in every bubble.
 *
 * One stretch of 2^32 blocks per seat (pickemup D21's spacing) means a seat's
 * dice never depend on how many dice another seat holds, even through the
 * rejection sampling, and a seat with k dice holds the first k draws of its
 * stretch. THE RECIPE IS PART OF THE FORMAT: changing a salt, the byte order of
 * the round or the draw order re-rolls every game ever sent. */
#include "cn.h"
#include "../../../shared/c/deal_rng.h"
#include "../../../shared/c/sha256.h"
#include <string.h>

void cn_log_digest(const CnMove *hist, int k, uint8_t out[32])
{
    static const char salt[] = "chuiniu.log.1|";
    Sha256 c;
    sha256_init(&c);
    sha256_update(&c, salt, sizeof salt - 1);
    for (int i = 0; i < k; i++) {
        uint8_t b[2] = { hist[i].q, hist[i].f };
        sha256_update(&c, b, 2);
    }
    sha256_final(&c, out);
}

void cn_round_key(const uint8_t seed[32], int round, const uint8_t log[32], uint8_t out[32])
{
    static const char salt[] = "chuiniu.dice.1|";
    uint8_t r[2] = { (uint8_t)round, (uint8_t)(round >> 8) };
    Sha256 c;
    sha256_init(&c);
    sha256_update(&c, salt, sizeof salt - 1);
    sha256_update(&c, seed, 32);
    sha256_update(&c, r, 2);
    sha256_update(&c, log, 32);
    sha256_final(&c, out);
}

void cn_roll(const uint8_t key[32], int seat, int count, uint8_t *out)
{
    DealRng rng;
    deal_rng_seed_at(&rng, key, (uint64_t)(uint32_t)seat << 32);
    for (int i = 0; i < count; i++) out[i] = (uint8_t)(1 + deal_rng_bounded(&rng, CN_FACES));
}

void cn_roll_round(CnGame *g)
{
    uint8_t log[32], key[32];
    cn_log_digest(g->hist, g->round_at, log);
    cn_round_key(g->seed, g->round, log, key);
    memset(g->dice, 0, sizeof g->dice);
    for (int s = 0; s < g->n; s++) cn_roll(key, s, g->dice_n[s], g->dice[s]);
}
