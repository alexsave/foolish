// Distill — shared per-move feature extractor for the cordite imitation
// experiment. ONE extractor used by both the training dumper
// (main_distill.c) and the inference strategy (distilled_strategy.c) so the
// vectors the linear policy scores at play time are bit-identical to the
// vectors it was trained on.
#ifndef CNITRO_DISTILL_FEAT_H
#define CNITRO_DISTILL_FEAT_H

#include "game.h"
#include "legal.h"

// Number of features distill_features writes. Keep in sync with the layout
// documented in distill_feat.c (tools/distill_train.py infers the count from
// the CSV header).
#define DISTILL_NUM_FEATURES 55

// Invalidate the per-decision cache (oracle picks + state scalars). MUST be
// called once before the first distill_features call of every new decision;
// subsequent calls for the other candidates of the same (g, bot_idx, all)
// reuse the cached oracle answers, so each oracle runs exactly once per
// decision.
void distill_decision_reset(void);

// Write the feature vector for candidate `m` (a pointer INTO all->moves —
// the meta-features compare its index against the oracle picks) into
// out[0..DISTILL_NUM_FEATURES-1]. Returns DISTILL_NUM_FEATURES.
int distill_features(const Game *g, int bot_idx, const LegalMove *m,
                     const LegalMoves *all, double *out);

#endif
