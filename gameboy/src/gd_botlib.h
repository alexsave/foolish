// gd_botlib - small pieces the bots share, in the fixed bank 0 so a brain in
// any bank can call them.
#ifndef GD_BOTLIB_H
#define GD_BOTLIB_H

#include <stdint.h>
#include "gd_sim.h"

// The search bots' working memory, one copy for both (Seoul and Moscow never
// think at once, and work RAM is short): a sampled world, the candidate moves,
// and rollout states.
#define GD_BOT_CANDS 6
extern gd_game gd_bot_world;
extern gd_sim  gd_bot_sim[3];
// The candidate list is also the sound driver's working state
// (audio/snd.c SND_OVERLAY, README "Sound"): no cue sounds while Seoul or
// Moscow thinks (snd_yield before, snd_claim after), and the list is rebuilt
// from nothing on every decision, so the two never need it at once.
typedef union {
    gd_move cand[GD_BOT_CANDS];
    uint8_t snd[GD_BOT_CANDS * sizeof(gd_move)];
} gd_bot_scratch_t;
extern gd_bot_scratch_t gd_bot_scratch;
#define gd_bot_cand   (gd_bot_scratch.cand)
#define SND_RAM       (gd_bot_scratch.snd)
#define SND_RAM_BYTES sizeof(gd_bot_scratch)

// foolish's getTrumpAttackProbability as an integer: an attack with trumps
// goes ahead when the 16-bit draw r is below this. Exactly `r / 65536.0 < p`
// for foolish's double p (tests/difftest.c checks every case). `early` is
// "0.02 while the deck lives" (handwritten_prod reads only the face-up trump,
// the arena handwritten the whole stock); else p = 0.65 + 0.35 x discard /
// total cards, held to [0.5, 0.95].
uint16_t gd_trump_threshold(uint8_t early, uint8_t discard, uint8_t total);

#endif
