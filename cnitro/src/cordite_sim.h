// Compact bitboard rollout engine for cordite.
//
// The root MC (corditeChoose) keeps running on the heavyweight `Game` struct;
// only each fully-determinized sampled WORLD is converted once into a SimState
// here, then all the fast playouts run on bitboards. A SimState is ~100 bytes
// and clones with a single memcpy, vs the 44 KB Game.
//
// Card id encoding: id = suit*13 + (value-1), range 0..51. Each player's hand
// is a uint64 bitmask over ids. Hand ops are O(1) bit twiddles; "cards of value
// v" / "cards of suit s" / trumps are precomputed masks.
//
// The rollout reimplements the same rules (handle_attack/cover/pass/pickup/good
// + refill + elimination) and the same rollout POLICY (handwritten — see
// cordite_sim.c for why espresso is never actually reached through
// cd_rollout_for) directly on the bitboard state.
#ifndef CNITRO_CORDITE_SIM_H
#define CNITRO_CORDITE_SIM_H

#include "game.h"
#include <stdint.h>

// Must cover the ENGINE's battle capacity: the wasm build compiles with
// -DMAX_BATTLES=64 and an overflow here corrupts the adjacent SimState
// fields (observed: garbage atk ids ORed into hands as card bits >= 52 via
// pickup's 1<<id). 64 is also the ceiling covered_mask can index.
#define SIM_MAX_BATTLES 64
_Static_assert(MAX_BATTLES <= SIM_MAX_BATTLES, "SimState battle arrays smaller than the engine's");

typedef struct {
    uint8_t  num_players;
    uint8_t  power_suit;
    int8_t   defender;
    int8_t   first_attacker;
    uint8_t  status;          // GAME_STATUS_*
    uint8_t  num_battles;
    int16_t  deck_count;
    int16_t  discard_pile_length;
    uint8_t  has_flipped;
    uint8_t  flipped_id;       // card id of the flipped trump (when has_flipped)
    uint32_t good_mask;        // players that have said good
    uint8_t  num_eliminated;

    uint64_t hand[MAX_PLAYERS];     // card-id bitmask per player
    uint8_t  status_p[MAX_PLAYERS]; // PLAYER_STATUS_*
    // Bitmask mirrors of status_p (bit p set iff status_p[p] is IN / OUT).
    // The playout runs several O(num_players) status scans per ply
    // (sim_done, sim_next_player, should_act, the good-action attacker
    // count) — the second-biggest cost in the wasm profile after the policy
    // chooser. Statuses only ever transition to OUT mid-rollout, so the
    // masks are trivially maintained alongside the array.
    uint32_t in_mask;
    uint32_t out_mask;

    int8_t   elim_order[MAX_PLAYERS];

    uint8_t  atk[SIM_MAX_BATTLES];      // attack card ids
    uint8_t  def[SIM_MAX_BATTLES];      // defense card ids (valid iff covered bit)
    uint64_t covered_mask;              // bit i set => battle i is covered
    // Cached OR of VALUE_MASK over every card on the table. The playout
    // queries this several times per ply (attack grouping, solver movegen);
    // rebuilding it per query was the single hottest loop in the wasm
    // profile. Maintained incrementally by the sim_apply_* functions (cards
    // are only ever added to the table; round end clears it), rides along in
    // struct-copy clones.
    uint64_t table_vmask;

    int16_t  deck_n;                    // mirror of deck_count for the array
    uint8_t  deck[MAX_DECK];            // remaining deck card ids, in draw order
} SimState;

#include "legal.h"

// Build a SimState from a fully-determinized Game world (all hands known).
void cd_sim_from_game(SimState *s, const Game *g);

// Apply a root LegalMove (the bot's candidate move) to a SimState. Returns 1 on
// success, 0 on validation failure (mirrors cd_apply/handle_*). Used to run the
// candidate move on the bitboard state so the whole world can be converted once
// and each candidate just clones the SimState.
int cd_sim_apply_root_move(SimState *s, int p_idx, const LegalMove *m);

// Forced-draw queue (novichok NV_PEEK=2 refill pinning): the next n sim_draw
// calls return exactly these card ids in order (spliced out of the deck by id,
// no RNG consumed) instead of random picks; a miss (id not in the deck) drops
// the remainder of the queue and reverts to random draws. Thread-local; call
// with n=0 (ids may be NULL) to clear. Callers arm it immediately before the
// root-move apply whose refill they are pinning.
void cd_sim_set_forced_draws(const uint8_t *ids, int n);

// Roll the world forward under the handwritten rollout policy; returns
// my_idx's finish position (1..N), or 0 if it didn't terminate. Mirrors
// cd_simulate's early-exit-on-elimination semantics. `early_exit` matches
// !cd_no_earlyexit.
int  cd_sim_playout(SimState *s, int my_idx, int max_turns, int early_exit);

// As cd_sim_playout, but small 2-player deck-empty endgames are resolved
// exactly by the bitboard solver (sign-only null window, `leaf_budget` nodes,
// positions with <= `leaf_cards` cards) instead of played out by the policy.
// One attempt per playout; unresolved solves fall back to policy play.
int  cd_sim_playout_leaf(SimState *s, int my_idx, int max_turns, int early_exit,
                         int leaf_cards, long leaf_budget);

// Per-seat rollout policies (cd_sim_playout_pol's pol[] values).
#define CD_POL_HW    0   // handwritten (the default rollout model)
#define CD_POL_LOOSE 1   // weak random-ish opponent model (profiled-weak seats)
#define CD_POL_MCDEF 2   // MC-defender model: strategic trump-saving pickups
                         // half the time (proven-strategic seats, octogen)

// Playout where seat p plays pol[p] (NULL = all handwritten), with optional
// exact leaf endgames when leaf_cards > 0. Superset of cd_sim_playout_leaf.
int  cd_sim_playout_pol(SimState *s, int my_idx, int max_turns, int early_exit,
                        int leaf_cards, long leaf_budget, const uint8_t *pol);

// Reply-tournament playout (octogen): the first opponent decision is chosen
// by search over their full legal reply set (up to reply_cap playouts; the
// opponent takes the reply best for THEM). Returns my finish position.
int  cd_sim_playout_reply(SimState *s, int my_idx, int max_turns,
                          int leaf_cards, long leaf_budget,
                          const uint8_t *pol, int reply_cap);

// Exact 2-player deck-empty endgame solver on the bitboard state. Returns the
// value of position `s` from `me`'s perspective in [-1000,1000] (positive = me
// escapes, magnitude prefers faster wins / slower losses), identical to the
// struct solver's value when fully resolved. Uses a thread-local transposition
// table (reset with cd_sim_solve_reset). `*aborted` is set if the budget or
// depth cap blew (value then meaningless, treat as unresolved).
int  cd_sim_solve(SimState *s, int me, int alpha, int beta, long budget, int *aborted);
int  cd_sim_solve_d(SimState *s, int me, int alpha, int beta, long *budget,
                    int depth0, int *aborted);
void cd_sim_solve_reset(void);

// Shared struct-solver scratch (child states + move lists, indexed by depth),
// ONE copy for all MC families (cordite/semtex/octogen). Safe to share: one
// bot family runs per decision, an exact solve completes before any other
// solve starts (rollout leaf-solves are sequential, never nested inside
// another solve), and recursion indexes by depth. Per-family lazy mallocs
// tripled the wasm footprint (3 x 48 x sizeof(LegalMoves) on the no-free bump
// allocator), which blew the edge isolate's 150MB external budget; a shared
// static lands in BSS — stable initial memory, zero runtime growth.
// _Thread_local keeps the native OMP eval race-free; wasm strips it.
#define SOLVE_SCRATCH_DEPTH 48
// Move-list slots hold 100 moves, not MAX_LEGAL_MOVES: all three families
// abort any solve node whose move count exceeds 96 (*_SOLVE_MAX_MOVES), so
// generation is capped just ABOVE that threshold (legal_set_move_cap) — a
// node with <= 96 moves enumerates identically, and one with more saturates
// at 100, still > 96, so the abort fires exactly as it did with full-size
// buffers. Full-size lists were 30MB of the 41MB wasm static footprint.
#define SOLVE_SCRATCH_MOVES 100
typedef struct {
    int       n;
    LegalMove moves[SOLVE_SCRATCH_MOVES];
} SolveMoves;
_Static_assert(SOLVE_SCRATCH_MOVES <= MAX_LEGAL_MOVES, "cap must fit the generator's own bound");
Game       *solve_scratch_child(void);
SolveMoves *solve_scratch_mv(void);

#endif
