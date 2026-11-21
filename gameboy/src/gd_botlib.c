// gd_botlib - see gd_botlib.h.
#include "gd_botlib.h"

gd_game gd_bot_world;
gd_move gd_bot_cand[GD_BOT_CANDS];
gd_sim  gd_bot_sim[3];

// ---- getTrumpAttackProbability, as a threshold on a 16-bit draw ----------------------------
// p = 0.02 while the deck lives (`early`); else 0.65 + 0.35 * discard /
// total, held to [0.5, 0.95]. As a fraction, p = (13t + 7d) / (20t), and the
// count of r in [0, 65536) with r / 65536 < p is ceil(65536 p).
uint16_t gd_trump_threshold(uint8_t early, uint8_t discard, uint8_t total) {
    uint32_t num, den;
    if (early) return 1311;                    // ceil(65536 * 0.02)
    if (total < 1) total = 1;
    if (discard > total) discard = total;
    if (7u * discard >= 6u * total) return 62260;    // held at 0.95: ceil(62259.2)
    num = 16384ul * (13ul * total + 7ul * discard);  // 65536 (13t + 7d) / (20t)
    den = 5ul * total;
    return (uint16_t)((num + den - 1u) / den);
}

