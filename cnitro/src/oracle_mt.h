// Infinite Oracle Mode B — shared control block (docs/INFINITE_ORACLE_DESIGN.md
// §8b, MT4). Compiled ONLY into oracle-mt.wasm (-DFOOLISH_ORACLE_MT). Shared
// between octogen_strategy.c (the MT5 accumulate hook, which has the Candidates
// + score[] arrays in scope) and wasm_oracle_mt.c (the thread loop + the
// control-side snapshot/candidate exports). All cross-thread fields are touched
// with __atomic_*; the descriptor table is CAS-published once per generation.
#ifdef FOOLISH_ORACLE_MT
#ifndef CNITRO_ORACLE_MT_H
#define CNITRO_ORACLE_MT_H

#include <stdint.h>

#define OG_MT_MAX_CANDS 26
#define OG_MT_MAX_CARDS 12          // candidate moves never exceed this in practice

// One candidate's move descriptor, packed for the TS overlay: each card byte is
// (suit << 4) | value — suit 0-3, value 1-13, decoded TS-side to {suit,value}.
typedef struct {
    uint8_t type;                   // MOVE_* id
    uint8_t n_cards;
    uint8_t cards[OG_MT_MAX_CARDS];
    uint8_t n_targets;
    uint8_t targets[OG_MT_MAX_CARDS];
} OgMtCand;

typedef struct {
    // job control (written by the control instance under wasm_mt_setup)
    uint32_t generation;            // bumped by setup; threads re-arm on change
    uint32_t stop;                  // 1 = park
    uint32_t seed_base;
    int32_t  seat;
    uint32_t nthreads;
    uint32_t active;                // # threads currently deliberating

    // raw throughput (the latency probe kept from the first cut)
    uint64_t total_choose;
    uint64_t checksum;

    // per-candidate accumulation (MT5)
    uint32_t cand_state;            // 0 none, 1 publishing, 2 ready
    int32_t  n_candidates;
    int32_t  chosen;                // chosen candidate index (last writer wins)
    OgMtCand cand[OG_MT_MAX_CANDS];
    uint64_t sum_fp[OG_MT_MAX_CANDS];   // Σ finish positions (integral -> exact)
    uint32_t nsim[OG_MT_MAX_CANDS];
    uint32_t forced_loss[OG_MT_MAX_CANDS];
    uint32_t batches;               // # accumulated batches across all threads
    uint32_t desc_mismatch;         // candidate-set disagreement counter

    // exact endgame verdicts (MT6): the full per-candidate win/draw/loss table
    // from the same verdict probe Mode A runs, hoisted behind FOOLISH_ORACLE_MT.
    uint32_t solver_applied;        // 1 = the endgame solver fired this decision
    uint32_t defuse_probe;          // 1 = skip the (expensive) probe — unproven, gave up
    int32_t  verdict[OG_MT_MAX_CANDS];  // og_ex_verdict value / sentinel, per candidate
} OgMtControl;

extern OgMtControl g_ogmt;

#endif  // CNITRO_ORACLE_MT_H
#endif  // FOOLISH_ORACLE_MT
