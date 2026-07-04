// Distilled — a linear ranker trained to imitate cordite (CD_BUDGET=prod),
// with an optional confidence gate that falls back to the real Monte-Carlo
// bot on uncertain decisions.
//
// Per decision: extract distill_features for every candidate (the four
// heuristic oracles inside the extractor run once per decision), score
// w·x with the logistic-regression weights from tools/distill_train.py
// (distilled_weights.h), and pick the argmax. Microseconds instead of
// cordite's milliseconds.
//
// DL_TAU — the gate, an int in HUNDREDTHS OF A LOGIT (the natural score unit
// of w·x: the pairwise trainer fits P(a beats b) = sigmoid(w·(xa-xb)), so a
// margin of 100 means "the model gives the runner-up < 27% odds"). If the
// top-two score margin is below DL_TAU/100, defer to cordite_strategy_choose.
// DL_TAU=0 => pure distilled, cordite is never called. Unset defaults to
// DL_TAU_DEFAULT below.
//
// DL_STATS=1 (native builds only): count decisions/deferrals and time the
// choose body; a report prints at process exit. Counters are plain globals —
// run with OMP_NUM_THREADS=1 for exact numbers.
//
// wasm-clean: no libm, no exp; the wasm build compiles the stats path out.

#include "strategy.h"
#include "distill_feat.h"
#include "distilled_weights.h"
#include <stdlib.h>

#if DISTILLED_W_FEATURES != DISTILL_NUM_FEATURES
#error "distilled_weights.h is stale: regenerate with tools/distill_train.py"
#endif

// Measured at pc4 vs espresso (400 games, cordite baseline 40.0%/1.990):
// tau=0 defers 0% (8 us/decision) but plays at 22.5%/2.665; tau=49 defers
// ~21% at 25.8%/2.470; tau=130 defers ~66-68% at 39.0%/2.100 — the cheapest
// gate within ~0.1 mean finish of cordite (~1.4x end-to-end speedup);
// parity itself needs tau~200 (>90% deferred, no real speedup). Distillation
// is NOT strength-neutral here; 130 is the least-bad compromise default.
#define DL_TAU_DEFAULT 130

static _Thread_local int dl_loaded = 0;
static _Thread_local int dl_tau = DL_TAU_DEFAULT;

#if !defined(__wasm__) && !defined(__wasm32__)
#include <stdio.h>
#include <time.h>
static int dl_stats = 0;
static long dl_n_decisions = 0, dl_n_deferred = 0;
static double dl_secs = 0.0;
static void dl_report(void) {
    fprintf(stderr, "DL_STATS: decisions=%ld deferred=%ld (%.1f%%) "
            "mean=%.2f us/decision\n",
            dl_n_decisions, dl_n_deferred,
            dl_n_decisions ? 100.0 * (double)dl_n_deferred / (double)dl_n_decisions : 0.0,
            dl_n_decisions ? 1e6 * dl_secs / (double)dl_n_decisions : 0.0);
}
static double dl_wall(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}
#endif

int distilled_strategy_choose(const Game *g, int bot_idx,
                              const LegalMoves *moves, void *ctx) {
    if (moves->n == 0) return -1;
    if (moves->n == 1) return 0;

    if (!dl_loaded) {
        const char *v = getenv("DL_TAU");
        dl_tau = (v && v[0]) ? atoi(v) : DL_TAU_DEFAULT;
#if !defined(__wasm__) && !defined(__wasm32__)
        v = getenv("DL_STATS");
        if (v && v[0] && v[0] != '0' && !dl_stats) {
            dl_stats = 1;
            atexit(dl_report);
        }
#endif
        dl_loaded = 1;
    }

#if !defined(__wasm__) && !defined(__wasm32__)
    double t0 = dl_stats ? dl_wall() : 0.0;
#endif

    double feats[DISTILL_NUM_FEATURES];
    double best = 0.0, second = 0.0;
    int best_i = 0;
    distill_decision_reset();
    for (int i = 0; i < moves->n; i++) {
        distill_features(g, bot_idx, &moves->moves[i], moves, feats);
        double s = 0.0;
        for (int f = 0; f < DISTILL_NUM_FEATURES; f++)
            s += DISTILLED_W[f] * feats[f];
        if (i == 0 || s > best) {
            second = (i == 0) ? -1e300 : best;
            best = s;
            best_i = i;
        } else if (i == 1 || s > second) {
            second = s;
        }
    }

    bool defer = dl_tau > 0 && (best - second) * 100.0 < (double)dl_tau;

#if !defined(__wasm__) && !defined(__wasm32__)
    if (dl_stats) {
        dl_n_decisions++;
        if (defer) dl_n_deferred++;
        // Deferred decisions bill cordite's time too — the mean reflects the
        // real end-to-end cost of the gated policy.
        if (!defer) { dl_secs += dl_wall() - t0; }
    }
    if (defer) {
        int r = cordite_strategy_choose(g, bot_idx, moves, ctx);
        if (dl_stats) dl_secs += dl_wall() - t0;
        return r;
    }
#else
    if (defer) return cordite_strategy_choose(g, bot_idx, moves, ctx);
#endif
    return best_i;
}
