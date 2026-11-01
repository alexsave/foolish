// Infinite Oracle - what the overlay says about each row, and in what order
// (docs/INFINITE_ORACLE_DESIGN.md "Ties, Best and the classes"). ONE owner: both
// oracle modules export it, and both modes hand it the same per-row numbers, so
// the overlay only renders what comes back.
//
//   OgRankAsk     what the host accumulated per row: the noise-free race's mean
//                 and standard error, fusion's noisy race's, the exact probe's
//                 verdict, and what the trump-keep tax needs. Written through
//                 the writer structgen makes from this header.
//   OgRankAnswer  the rows in display order, each with the number it shows
//                 (fusion's noisy race's expected finish) and its error, its
//                 class, whether it is Best (tied with fusion's pick), whether
//                 it IS fusion's pick, and its bar.
//
// It used to be four rules in TypeScript, two of them twice: a sort in each
// mode, fusion's Best pick (accumulator.ts fusionBest) and the class chips
// (types.ts oracleClassify), and the chips measured from the lowest number
// while Best was fusion's pick - two yardsticks, so of eleven rows printing
// "EF 3.76 +-0.00" one read BEST and ten read BLUNDER.
//
// Compiled only into the two oracle modules (-DFOOLISH_ORACLE_BUILD) and the
// native test that holds the rule (tests/oracle_rank_test.c).
#ifdef FOOLISH_ORACLE_BUILD
#ifndef CNITRO_ORACLE_RANK_H
#define CNITRO_ORACLE_RANK_H

#include <stdint.h>
#include <stdbool.h>
#include "oracle_answer.h"

// Every row an answer can carry: 128 candidates and 600 proven moves
// (oracle_answer.h OG_ANSWER_MAX_ROWS, asserted in oracle_rank.c where both are
// visible). Mode A merges answers by move, so it never holds more.
#define OG_RANK_MAX_ROWS 728

// A row's class, as chess.com classifies moves: by expected-points loss
// against the Best group (oracle_rank.c og_rank_class has the table).
enum {
    OG_CLASS_NONE       = 0,        // not scored, or the exact regime (verdicts, no classes)
    OG_CLASS_BEST       = 1,        // tied with fusion's pick
    OG_CLASS_EXCELLENT  = 2,        // loss < 0.02 expected points
    OG_CLASS_GOOD       = 3,        // < 0.05
    OG_CLASS_INACCURACY = 4,        // < 0.10
    OG_CLASS_MISTAKE    = 5,        // < 0.20
    OG_CLASS_BLUNDER    = 6,
};

typedef struct {
    bool    scored;                 // mean is over nsim > 0 worlds
    bool    shadow_scored;          // fusion's noisy race sampled it
    bool    attack;                 // an attack: the trump-keep tax applies
    uint8_t n_trump;                // trump cards it leads
    uint8_t verdict;                // OG_VERDICT_*
    bool    has_verdict_val;
    int32_t verdict_val;            // +-(1000 - depth)
    int32_t nsim;
    int32_t shadow_nsim;
    double  mean;                   // expected finish, noise-free race, untaxed
    double  se;                     // its standard error; negative = not measured,
                                    //   use the n-based proxy (Mode B)
    double  shadow_mean;            // the noisy race's, untaxed
    double  shadow_se;              // likewise
} OgRankRow;

typedef struct {
    bool     exact;                 // a proven win or loss exists: verdict ordering
    bool     deck_alive;            // the trump-keep tax is on
    uint8_t  n_players;             // seats at the table: finish places 1..n
    uint8_t  n_out;                 // seats already out: the mover finishes in
                                    //   n_out + 1 .. n_players (the host's board says)
    uint16_t n_rows;
    OgRankRow rows[OG_RANK_MAX_ROWS];
} OgRankAsk;

typedef struct {
    uint16_t row;                   // index into OgRankAsk.rows
    uint8_t  cls;                   // OG_CLASS_*
    bool     best;                  // tied with fusion's pick: reads Best
    bool     pick;                  // fusion's pick itself: the move fusion plays
    bool     shown;                 // the row has a number to show (ef, se, bar)
    float    bar;                   // bar width, percent (0 = no bar)
    double   ef;                    // the expected finish shown: the noisy race's
                                    //   (the noise-free one when OgRankAnswer.noisy is false)
    double   se;                    // its standard error, the +- shown
    double   shadow_se;             // the noisy race's error, as fusion's bar weighed it
} OgRankLine;

typedef struct {
    bool     noisy;                 // the numbers are the noisy race's (it scored some row)
    bool     converged;             // every shown row's number has met the checkpoint
                                    //   (OG_RANK_CONVERGE_*): the run may stop
    uint16_t n_lines;
    OgRankLine lines[OG_RANK_MAX_ROWS];
} OgRankAnswer;

// The rule's numbers, named for its test.
#define OG_RANK_TIE_FLOOR  0.0001   // two rows nearer than this always tie
#define OG_RANK_SE_PROXY   1.2      // finish-place spread behind the n-based error
// The convergence checkpoint (docs/INFINITE_ORACLE_DESIGN.md §8.7), on the
// numbers SHOWN: every shown row has at least this many worlds behind its
// number AND its error is at most this. AND, not OR: the error is the test and
// the world count a floor under it, so a lucky error off a handful of samples
// cannot end the run.
#define OG_RANK_CONVERGE_MIN_N  65536
#define OG_RANK_CONVERGE_MAX_SE 0.005

void og_rank(const OgRankAsk *ask, OgRankAnswer *out);

#endif  // CNITRO_ORACLE_RANK_H
#endif  // FOOLISH_ORACLE_BUILD
