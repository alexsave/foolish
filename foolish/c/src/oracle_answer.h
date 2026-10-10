// Infinite Oracle - what crosses between octogen and its host
// (docs/INFINITE_ORACLE_DESIGN.md §4.2, §8.5). Two things, and both are structs
// so that no host lays out a byte of either: the host reads and writes them
// through the modules structgen writes from this header
// (tools/structgen/specs/oracle_answer.args, oracle_layout.args).
//
//   OgKnobs    what the host sets before a batch: the handful of OG_* values
//              the Oracle tunes, as numbers. Both oracle modules.
//   OgAnswer   what one deliberation answered: the position as the seat saw
//              it, octogen's belief, every candidate with its score and
//              verdict, and the playout storylines behind each score. Mode A
//              only - Mode B accumulates in C instead (oracle_mt.h).
//
// The answer used to leave the module as a line of JSON text the kernel printed
// into a megabyte buffer with its own freestanding snprintf, beside a packed
// blob ("OGP1") whose offsets lived in a comment on each side. Both are gone:
// the kernel fills this struct where the text was formatted, and a field added
// here reaches the host by regenerating, not by editing two parsers.
//
// Compiled only where octogen's deliberation is read: -DFOOLISH_ORACLE_BUILD
// (the two oracle modules) and -DOG_EXPLAIN_BUILD (the answer; also the native
// research builds, which print it - octogen_strategy.c og_answer_print). The
// shipped bots carry none of it.
#if defined(FOOLISH_ORACLE_BUILD) || defined(OG_EXPLAIN_BUILD)
#ifndef CNITRO_ORACLE_ANSWER_H
#define CNITRO_ORACLE_ANSWER_H

#include <stdint.h>
#include <stdbool.h>
#include "card.h"
#include "game.h"
#include "legal.h"

#ifdef FOOLISH_ORACLE_BUILD
// The knobs the Oracle sets, and the only ones: every other OG_* value is the C
// default. Applying them replaces the whole override table, so a knob the host
// does not name cannot survive from an earlier batch (wasm_bots_api.c
// wasm_og_knobs_apply), and octogen reads the new values at its next choose.
typedef struct {
    int32_t keep1, keep2;           // OG_KEEP1 / OG_KEEP2: 26 turns racing off
    int32_t w1, w2, w3;             // OG_W1..OG_W3: worlds per stage
    int32_t solve_budget;           // OG_EXPLAIN_SOLVE_BUDGET: nodes per root move, 0 = no probe
} OgKnobs;
#endif

#ifdef OG_EXPLAIN_BUILD
// Matches OG_MAX_CANDS for an oracle build and OG_MAX_VOIDS (octogen_strategy.c
// asserts both), so this header never decides how wide octogen thinks.
#define OG_ANSWER_MAX_CANDS    128
#define OG_ANSWER_MAX_VOIDS    6
// The exact probe judges the first this-many root moves (og_ex_verdict), and a
// proven move that is not a candidate gets a row of its own. So the rows are
// bounded by construction and there is no overflow to report: the JSON line
// had one, because text has no natural size.
#define OG_ANSWER_MAX_VERDICTS 600
#define OG_ANSWER_MAX_ROWS     (OG_ANSWER_MAX_CANDS + OG_ANSWER_MAX_VERDICTS)
#define OG_ANSWER_LABEL        96
#define OG_ANSWER_TOP_PATHS    12
#define OG_ANSWER_TOP_REPLIES  3
#define OG_ANSWER_PATH_ROUNDS  4    // CD_ORC_ROUNDS (cordite_sim.h); octogen_strategy.c asserts it

// What the exact endgame probe said of one root move.
enum {
    OG_VERDICT_NONE    = 0,         // the probe did not run, or did not reach this move
    OG_VERDICT_UNKNOWN = 1,         // ran out of budget
    OG_VERDICT_ILLEGAL = 2,
    OG_VERDICT_WIN     = 3,
    OG_VERDICT_LOSS    = 4,
    OG_VERDICT_DRAW    = 5,
};

// How the root endgame solver left the decision.
enum {
    OG_SOLVER_NONE = 0,             // not an endgame: Monte Carlo decided
    OG_SOLVER_WIN  = 1,             // a forced win was taken
    OG_SOLVER_EVAL = 2,             // judged every move, took no forced win
};

// One row of the answer: a candidate octogen weighed, or (pruned) a move the
// exact probe proved that octogen had already excluded from candidacy - the
// very move a reviewer most wants judged when it is the one that was played.
typedef struct {
    uint8_t type;                   // MOVE_* id
    uint8_t n_cards;
    Card    cards[MAX_MOVE_CARDS];
    Card    targets[MAX_MOVE_CARDS];    // a cover's attack cards, in cards' order
    uint8_t n_targets;              // n_cards for a cover, 0 for everything else
    bool    scored;                 // score is a mean over nsim > 0 playouts
    bool    alive;                  // still in the race when the batch ended
    bool    pruned;
    bool    forced_loss;
    bool    chosen;                 // the move octogen returned
    uint8_t verdict;                // OG_VERDICT_*
    bool    has_verdict_val;
    int32_t verdict_val;            // +-(1000 - depth), for a win, loss or draw
    int32_t nsim;
    double  score;                  // mean finish position (lower is better), untaxed
    uint8_t label_len;
    char    label[OG_ANSWER_LABEL]; // the move in the kernel's notation: "cover 10H->7H"
} OgAnswerRow;

// First move by any other seat after the root move, counted over the playouts.
typedef struct {
    uint8_t  type;                  // cordite_sim MV_ATTACK..MV_GOOD (0..4)
    uint8_t  card;                  // 0..51 card id, 52 for a card-less move
    uint32_t n;
} OgAnswerReply;

// One storyline: the outcome of the first rounds after the root move, as
// cordite_sim.h cd_orc symbols (1 = I beat the round, 2 = I picked up, 3 = an
// opponent beat it, 4 = an opponent picked up). Fewer than
// OG_ANSWER_PATH_ROUNDS symbols means the playout resolved inside the window.
typedef struct {
    uint8_t  len;
    uint8_t  sym[OG_ANSWER_PATH_ROUNDS];
    uint32_t n;                     // playouts that told this story
    float    fin;                   // their mean finish position
} OgAnswerPath;

// Why a row scored what it scored: its most told storylines, the replies it
// most often met, and whole-playout means. One per row that was sampled.
typedef struct {
    uint8_t  row;                   // index into OgAnswer.rows
    uint8_t  n_paths;
    uint8_t  n_replies;
    uint32_t n;                     // playouts folded
    float    mepk, oppk;            // pickups per playout: mine, the opponents'
    float    metr, opptr;           // trump cards spent per playout
    float    rnds;                  // rounds resolved per playout
    OgAnswerReply replies[OG_ANSWER_TOP_REPLIES];
    OgAnswerPath  paths[OG_ANSWER_TOP_PATHS];
} OgAnswerWhy;

typedef struct { uint8_t n; Card cards[MAX_HAND_SIZE]; } OgAnswerPinned;
typedef struct { uint8_t n; Card cards[OG_ANSWER_MAX_VOIDS]; } OgAnswerVoids;
typedef struct { Card attack; Card defense; bool covered; } OgAnswerBattle;

typedef struct {
    int8_t   seat;
    int8_t   defender;
    int8_t   trump;
    uint8_t  n_players;
    int32_t  deck;
    int32_t  n_logs;
    bool     solver_applied;        // the exact probe judged this decision
    uint8_t  solver_result;         // OG_SOLVER_*
    // The position as the seat saw it.
    uint8_t  n_hand;
    Card     hand[MAX_HAND_SIZE];
    uint8_t  hand_count[MAX_PLAYERS];
    uint8_t  n_battles;
    OgAnswerBattle table[MAX_BATTLES];
    // Octogen's belief about what it cannot see, straight from og_build_belief:
    // cards publicly pinned to each seat's hand, attack cards a seat
    // demonstrably could not beat, the lowest rank a seat can hold (0 = none),
    // and how many cards are still genuinely unknown.
    OgAnswerPinned pinned[MAX_PLAYERS];
    OgAnswerVoids  voids[MAX_PLAYERS];
    int8_t   floor[MAX_PLAYERS];
    uint8_t  n_pool;
    uint16_t n_rows;
    uint8_t  n_why;
    OgAnswerRow rows[OG_ANSWER_MAX_ROWS];
    OgAnswerWhy why[OG_ANSWER_MAX_CANDS];
} OgAnswer;

// The answer of the last deliberation, or NULL when no choose has answered
// since og_answer_reset (a seat with no legal move answers nothing).
const OgAnswer *og_answer(void);
void og_answer_reset(void);
#endif  // OG_EXPLAIN_BUILD

#endif  // CNITRO_ORACLE_ANSWER_H
#endif  // FOOLISH_ORACLE_BUILD || OG_EXPLAIN_BUILD
