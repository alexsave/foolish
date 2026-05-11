// Dynamite — the GRPO-trained policy at play time. Loads a checkpointed
// GrpoNet and, for each decision, scores the current legal-move set and
// returns the argmax index.
//
// This is the play-time strategy companion to grpo_train.c (training-time).
// Naming: "dynamite" was chosen as a clean break from the older transformer
// track's "nitro_strategy" — they share no infrastructure.
#ifndef CNITRO_DYNAMITE_STRATEGY_H
#define CNITRO_DYNAMITE_STRATEGY_H

#include "game.h"
#include "legal.h"
#include "grpo_net.h"

// Register the network used by all subsequent dynamite_strategy_choose
// calls. The caller owns the GrpoNet — dynamite_strategy holds a borrowed
// pointer and never frees it.
void dynamite_strategy_set_net(const GrpoNet *net);

int dynamite_strategy_choose(const Game *g, int bot_idx,
                             const LegalMoves *moves, void *ctx);

// Same as _choose, but additionally writes the move-level softmax (one
// float per legal move) into `out_log_probs[0..moves->n-1]`. Use for
// verbose / inspector-style displays. Returns the chosen index.
int dynamite_strategy_choose_verbose(const Game *g, int bot_idx,
                                     const LegalMoves *moves,
                                     float *out_log_probs);

#endif
