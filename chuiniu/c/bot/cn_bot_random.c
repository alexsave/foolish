/* Chui Niu - the baseline: uniform over every legal option.
 *
 * The call when it is allowed and every legal raise, each equally likely,
 * straight from the kernel's own menu (cn_legal). Nothing more elaborate:
 * the owner's one baseline for the arena (BOT.md). */
#include "cn_bot.h"

CnMove cn_random_choose(const CnGame *g, uint64_t *rng)
{
    CnMove m[CN_MAX_DICE * CN_BID_FACES + 1];
    int n = cn_legal(g, m, (int)(sizeof m / sizeof m[0]));
    if (n <= 0) return (CnMove){ 0, 0 };
    return m[cn_splitmix(rng) % (uint64_t)n];
}
