// The post-game analyser: which decisions of a finished game were mistakes, how
// large, and what the evidence is. LLM-free, in C, on the real kernel.
//
// docs/POST_GAME_ANALYSER.md is the document. In one paragraph: a v6 replay
// code is hidden-state-lossless, so the whole game is rebuilt through the real
// engine (replay_steps.h). At every decision of the analysed seat the analyser
// builds that seat's PUBLIC belief (what it could know: its hand, the table,
// the discard, the flip, the cards it watched opponents pick up), samples or
// enumerates the worlds that belief admits, installs each world into the real
// board and plays EVERY legal move out to the end of the game with real bots at
// every seat. The same world list is used for every candidate, so the
// difference between two moves is a paired difference, not two noisy means.
// Where the world count is small enough the worlds are enumerated, and where a
// world is fully determined (the deck holds at most one card, two seats in)
// each candidate is solved exactly rather than played out, which is the one
// region where a verdict is a proof.
//
// It is NOT the Infinite Oracle's score (a rollout mean under a policy that
// declines the trump throw-in 98% of the time while the deck lives), and it
// does NOT trust octogen's candidate list (which keeps at most ten covers and
// can rank the only full cover out). Every legal move is a candidate here, and
// every playout is a real game between real bots. What it still cannot see is
// in the document's "what it cannot see" section; read that before quoting a
// number.
//
// The result crosses as packed bytes, with the reader beside
// the writer below, in the shape of the fio_*_packed entries.
#ifndef CNITRO_ANALYSE_H
#define CNITRO_ANALYSE_H

#include <stdint.h>

#include "game.h"
#include "legal.h"

// ---------- errors ----------------------------------------------------------
// Negated return values of analyse_packed.
#define ANALYSE_EOK       0
#define ANALYSE_EBADARG   1   // null pointer, bad seat, bad roster index
#define ANALYSE_EREPLAY   2   // the code did not decode or rebuild (analyse_last_replay_error)
#define ANALYSE_ECAP      3   // output buffer too small
#define ANALYSE_EMENU     4   // a recorded action is not in the menu the rebuilt board offers
#define ANALYSE_EBELIEF   5   // the belief model broke conservation - a bug, never a verdict
#define ANALYSE_ETRUNC    6   // reader: the packed bytes end early

// ---------- parameters --------------------------------------------------------
typedef struct {
    int      seat;            // seat to analyse, or -1 for every seat
    int      roster_idx;      // scan engine: bot_roster index; plays EVERY seat in a playout
    int      worlds;          // sampled worlds per node (>= 1)
    int      futures;         // deck orders per enumerated world, at most (deck > 1 card only)
    int      exhaustive_cap;  // enumerate when the hand assignments are <= this many (0 = never);
                              // the rest of the budget goes to deck orders, one at least
    int      max_candidates;  // per node; 0 = every legal move
    long     solve_budget;    // exact-solve node budget per (candidate, world); 0 = never solve
    int      deep_roster_idx; // -1 = no deep pass
    int      deep_nodes;      // the K largest-loss scan nodes get the deep pass
    int      deep_worlds;
    uint32_t seed;            // world sampling seed base
    int      threads;         // threads for the world loop; 0 or 1 = this thread only.
                              // The result is the same at any count.
} AnalyseParams;

void analyse_params_default(AnalyseParams *p);

// ---------- the entry ---------------------------------------------------------
//
// Analyse the game `code` (a v6-line replay integer, big-endian bytes, as
// replay_decode takes it) and write the packed result. Returns bytes written
// (> 0) or -ANALYSE_E*. On -ANALYSE_EREPLAY, analyse_last_replay_error() is the
// -REPLAY_E* the decode or rebuild returned.
int analyse_packed(const unsigned char *code, int code_len, const AnalyseParams *p,
                   unsigned char *out, int out_cap);
int analyse_last_replay_error(void);

// ---------- the wire ----------------------------------------------------------
//
// All integers little-endian. Fixed-point fields are x1000 (finish positions)
// or x10000 (probabilities), so nothing floating crosses.
//
//   header
//     u8  version           ANALYSE_WIRE_VERSION
//     u8  n_players
//     u8  seat              analysed seat, 0xFF = every seat
//     u8  trump_suit
//     u8  fool              0xFF while the code ends mid-game
//     u8  roster_idx        scan engine
//     u8  deep_roster_idx   0xFF = none
//     u8  flags             ANALYSE_HF_*
//     u16 n_nodes
//     u16 decisive_node     index into the nodes, 0xFFFF = none (see ANALYSE_V_DECISIVE)
//     u32 n_playouts        measured cost: full games played
//     u32 n_solves          measured cost: exact solves attempted
//     u32 elapsed_ms        measured cost: wall clock
//     per seat (n_players):
//       u8  opening_trumps
//       u16 p_exact          P(exactly that many)   x10000, hypergeometric
//       u16 p_at_most        P(at most that many)   x10000
//       u8  trumps_seen      trumps that entered the hand all game (deal + draws)
//   node (n_nodes of them)
//     u16 step              index of the recorded action (the deal is not a step)
//     u8  seat              the deciding seat
//     u8  verdict           ANALYSE_V_*
//     u8  flags             ANALYSE_NF_*
//     u8  n_cands
//     u8  played            index into the candidates
//     u8  best              index into the candidates (== played when the played move is best)
//     u16 n_worlds          worlds evaluated at this node (scan pass)
//     u8  unknown           |U|, cards the seat could not locate
//     u8  deck              cards left in the stock
//     u16 win_prob          x10000: 1 - P(fool) after the PLAYED move (the running strip)
//     i16 loss              x1000 finish positions: mean(played) - mean(best), paired
//     i16 loss_se           x1000: paired standard error of `loss` (0 under a proof)
//     per candidate:
//       u8  type            MOVE_*
//       u8  n_cards
//       u8  cards[n_cards]  card ids (card.h)
//       u8  targets[n_cards]   cover targets, 0xFF when not a cover
//       u16 n               evaluations
//       u16 n_fool          evaluations that ended with the seat as the fool
//       i16 mean_fp         x1000 mean finish position (1 = first out, N = fool)
//       i16 paired_diff     x1000 mean of (this - played) over shared worlds
//       i16 paired_se       x1000
//       i8  proof           ANALYSE_P_*
//       u16 proven_wins     worlds where the exact solve proved the seat escapes
//       u16 proven_losses   worlds where it proved the seat is the fool
//     if ANALYSE_NF_DEEP:
//       u16 deep_n_worlds
//       u8  deep_best
//       i16 deep_loss, i16 deep_loss_se
//       per candidate: u16 n, u16 n_fool, i16 mean_fp, i16 paired_diff, i16 paired_se
#define ANALYSE_WIRE_VERSION 1

#define ANALYSE_HF_ALL_SEATS   0x01  // seat == 0xFF
#define ANALYSE_HF_BELIEF_FAIL 0x02  // at least one node reported ANALYSE_NF_BELIEF_FAIL

// Verdicts. Finish positions are from the deciding seat's side; lower is better.
#define ANALYSE_V_FORCED    0  // one legal move
#define ANALYSE_V_BEST      1  // the played move is the best candidate (or tied)
#define ANALYSE_V_DECLINED  2  // a better move scored higher, but the paired CI spans zero
#define ANALYSE_V_CHANCE    3  // a mistake: the paired CI excludes zero, the game went on
#define ANALYSE_V_DECISIVE  4  // the LAST mistake before a run of LOST nodes ending in the fool's seat
#define ANALYSE_V_LOST      5  // every candidate is the fool in every world: nothing to be done

// Node flags.
#define ANALYSE_NF_EXHAUSTIVE   0x01  // hand assignments enumerated, not sampled
#define ANALYSE_NF_FUTURES      0x02  // ... but the deck order was sampled (deck > 1), so not every world
#define ANALYSE_NF_PROOF        0x04  // every (candidate, world) resolved by the exact solve
#define ANALYSE_NF_CAPPED       0x08  // max_candidates dropped legal moves (they are not in the list)
#define ANALYSE_NF_BELIEF_FAIL  0x10  // conservation failed: the node carries no verdict
#define ANALYSE_NF_DEEP         0x20  // the deep pass ran here; its block follows the candidates
#define ANALYSE_NF_DEEP_AGREES  0x40  // ... and picked the same best move as the scan

// Candidate proof.
#define ANALYSE_P_NONE   0   // frequencies only
#define ANALYSE_P_WIN    1   // proven escape in every evaluated world
#define ANALYSE_P_LOSS  -1   // proven fool in every evaluated world
#define ANALYSE_P_MIXED  2   // proven in every world, and the worlds disagree

// ---------- the reader --------------------------------------------------------
// A cursor over the packed bytes. Each read returns bytes consumed (> 0) or
// -ANALYSE_ETRUNC; nothing is ever read past `len`.
#define ANALYSE_MAX_CANDS 255

typedef struct {
    uint8_t  opening_trumps;
    uint16_t p_exact, p_at_most;
    uint8_t  trumps_seen;
} AnalyseDealSeat;

typedef struct {
    uint8_t  version, n_players, seat, trump_suit, fool, roster_idx, deep_roster_idx, flags;
    uint16_t n_nodes, decisive_node;
    uint32_t n_playouts, n_solves, elapsed_ms;
    AnalyseDealSeat deal[MAX_PLAYERS];
} AnalyseHeader;

typedef struct {
    uint8_t  type, n_cards;
    uint8_t  cards[MAX_MOVE_CARDS], targets[MAX_MOVE_CARDS];
    uint16_t n, n_fool;
    int16_t  mean_fp, paired_diff, paired_se;
    int8_t   proof;
    uint16_t proven_wins, proven_losses;
    // deep pass (valid when the node has ANALYSE_NF_DEEP)
    uint16_t deep_n, deep_n_fool;
    int16_t  deep_mean_fp, deep_paired_diff, deep_paired_se;
} AnalyseCand;

typedef struct {
    uint16_t step;
    uint8_t  seat, verdict, flags, n_cands, played, best;
    uint16_t n_worlds;
    uint8_t  unknown, deck;
    uint16_t win_prob;
    int16_t  loss, loss_se;
    uint16_t deep_n_worlds;
    uint8_t  deep_best;
    int16_t  deep_loss, deep_loss_se;
    AnalyseCand cands[ANALYSE_MAX_CANDS];
} AnalyseNode;

int analyse_read_header(const unsigned char *buf, int len, AnalyseHeader *h);
int analyse_read_node(const unsigned char *buf, int len, AnalyseNode *n);

// ---------- pieces exposed for the tests ---------------------------------------
// The belief a seat can hold about a board, from the public log alone.
typedef struct {
    Card pool[MAX_DECK];                        // U: cards the seat cannot locate
    int  n;
    Card pinned[MAX_PLAYERS][MAX_HAND_SIZE];    // cards the seat watched p pick up and not yet play
    int  pinned_n[MAX_PLAYERS];
    int  free_n[MAX_PLAYERS];                   // hand slots of p that hold a pool card
    int  deck_n;                                // stock cards, all from the pool
    int  ok;                                    // conservation held: n == deck_n + sum(free_n)
} AnalyseBelief;

// Build `seat`'s belief about `g`. `ok` says whether |U| == d + sum f_p; a
// caller that ignores it is quoting a world sampler that is silently wrong.
void analyse_belief(const Game *g, int seat, AnalyseBelief *B);

// Install one world: `perm` is the pool in some order; opponents' free slots
// take the first cards in seat order, the rest become the stock in draw order.
// Own hand, table, discard and log are untouched. deterministic_deck is set.
void analyse_install_world(Game *g, int seat, const AnalyseBelief *B, const Card *perm);

// One sampled world: the pool shuffled by `seed`, in the order
// analyse_install_world takes. Same seed, same world; the whole pool, always.
void analyse_sample_world(const AnalyseBelief *B, uint32_t seed, Card *perm);

// P(k trumps in a hand of CARDS_PER_PLAYER dealt from a deck of `deck` cards
// holding `trumps` trumps), and P(at most k). Exact.
double analyse_hypergeom(int deck, int trumps, int k);
double analyse_hypergeom_at_most(int deck, int trumps, int k);

// The verdict rule on its own, so it can be tested against numbers.
// `loss` and `se` in finish positions; `all_lost` = every candidate is the
// fool in every world; `played_best` = the played candidate is the best.
int analyse_verdict(int n_cands, int played_best, int all_lost, double loss, double se, int proof);

#endif
