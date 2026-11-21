// gd_rules - two-player Durak, exactly foolish's variant (foolish/c/src/game.c,
// legal.c), in plain portable C: no GBDK headers, no malloc, no float, and
// types that are right with a 16-bit int. The same file builds the Game Boy
// ROM (SDCC) and the differential test on the Mac (clang).
//
// The variant (foolish's defaults, see README.md "Rules"):
//   - 36 cards (6..A), 6 dealt to each seat, seat 0 first.
//   - The next stock card is turned for trump; an Ace is not turned while a
//     non-Ace is left (it goes to the bottom of the stock and the next is tried).
//   - The lowest trump leads; nobody dealt a trump -> a random seat leads.
//   - Perevodnoy: an uncovered table of one rank can be transferred.
//   - Attacks: any number of cards of one rank to open; throw-ins of any rank on
//     the table; never more uncovered attacks than the defender holds cards.
//     There is no other limit (no 6-card or first-bout-of-5 cap).
//   - Draws after a bout: the non-defender first, the defender last.
//   - An attacker or transferer who empties their hand with no stock left is
//     out at once; a defender who covers with their last card ends the bout.
//   - The repetition draw: the third occurrence of a position right after a
//     pickup that drew nobody a card ends the game as a draw.
//
// CARDS are ids 0..35: id = suit * 9 + rank, rank 0..8 for 6..A. A set of cards
// is a gd_mask, 36 bits in 5 bytes.
#ifndef GD_RULES_H
#define GD_RULES_H

#include <stdint.h>

#define GD_NCARDS  36
#define GD_NRANKS  9
#define GD_NSUITS  4
#define GD_ACE     8            // rank of the Ace
#define GD_HAND    6            // cards dealt, and the draw target
#define GD_MAXB    18           // a table of b battles holds 2b of 36 cards
#define GD_NONE    0xFFu        // no card (an uncovered battle's cover)
#define GD_STOCK   24           // the stock after the deal (36 - 12)

#define GD_PLAYING 1
#define GD_OVER    2

// Move kinds: the same numbers as foolish's MOVE_* (legal.h).
#define GD_ATTACK  0
#define GD_COVER   1
#define GD_PASS    2
#define GD_PICKUP  3
#define GD_GOOD    4

// gd_result
#define GD_RESULT_NONE  0       // still playing
#define GD_RESULT_FOOL0 1       // seat 0 is the fool (seat 1 won)
#define GD_RESULT_FOOL1 2       // seat 1 is the fool (seat 0 won)
#define GD_RESULT_DRAW  3       // the repetition draw: both are fools

typedef struct { uint8_t b[5]; } gd_mask;

#define GD_HAS(m, c) (((m).b[(c) >> 3] >> ((c) & 7)) & 1u)
#define GD_SET(m, c) ((m).b[(c) >> 3] |= (uint8_t)(1u << ((c) & 7)))
#define GD_CLR(m, c) ((m).b[(c) >> 3] &= (uint8_t)~(1u << ((c) & 7)))

extern const uint8_t gd_suit_of[GD_NCARDS];
extern const uint8_t gd_rank_of[GD_NCARDS];

// THE GAME, compact so a search bot can clone it with one struct copy. The
// repetition rule's memory is NOT in here (gd_rep below), for the reason foolish
// keeps it past the logs: a clone for a rollout does not carry it.
typedef struct {
    gd_mask hand[2];
    uint8_t stock[GD_STOCK];    // stock[0] is the next card drawn
    uint8_t stock_n;            // cards in stock[], not counting the trump card
    uint8_t flipped;            // the face-up trump card under the stock
    uint8_t has_flipped;        // it has not been drawn yet
    uint8_t trump;              // suit
    uint8_t att[GD_MAXB];       // battles in table order
    uint8_t def[GD_MAXB];       // GD_NONE while uncovered
    uint8_t nb;
    uint8_t fa;                 // first attacker of the bout
    uint8_t defender;
    uint8_t good;               // seat bits: said good (done) this bout
    uint8_t out;                // seat bits: out of the game
    uint8_t status;             // GD_PLAYING / GD_OVER
    uint8_t discard;            // cards in the discard pile
} gd_game;

// THE REPETITION DRAW's ring (foolish game.h GameRepetition), for two seats.
// Within one history (same stock, discard and seats in) a checkpoint's whole
// position is seat 0's hand plus who leads: everything else is fixed or empty
// (see gd_rules.c rep_checkpoint). So the ring holds those positions exactly,
// 5 bytes each (36 hand bits + the leader in bit 39), where foolish holds a
// 64-bit fingerprint of the same position.
#define GD_REP_RING 16
typedef struct {
    uint8_t pos[GD_REP_RING][5];
    uint8_t stock, discard, in_mask;
    uint8_t fill, head;
} gd_rep;

typedef struct {
    uint8_t type;               // GD_ATTACK..GD_GOOD
    uint8_t n;
    uint8_t cards[GD_MAXB];
    uint8_t atts[GD_MAXB];      // a cover only: the attack each card covers
} gd_move;

// ---- the deal ---------------------------------------------------------------
// `deck` is the shuffled deck in pop order. `fallback_seat` leads when nobody
// was dealt a trump (the caller draws it from the one RNG on every deal, used
// or not, so the number of draws never depends on the deal).
void gd_deal(gd_game *g, gd_rep *r, const uint8_t deck[GD_NCARDS], uint8_t fallback_seat);

// ---- the legal-move menu (foolish legal.c calculate_legal_moves) -------------
// The menu is a COUNT and an UNRANK: gd_menu_move(g, seat, i) for i < count is
// every legal move exactly once (tests/difftest.c proves the set equals
// foolish's). A uniform random move is one draw below the count, which is how
// the bot plays and why it never builds a list.
uint32_t gd_menu_count(const gd_game *g, uint8_t seat);
void     gd_menu_move(const gd_game *g, uint8_t seat, uint32_t idx, gd_move *m);
// Is `m` on the seat's menu (set equality of the cards).
uint8_t  gd_in_menu(const gd_game *g, uint8_t seat, const gd_move *m);

// ---- applying a move (foolish handle_*) -----------------------------------
// Would foolish's handler accept it? This is wider than the menu in exactly the
// places foolish's is: a pickup over a fully covered table, and an attack or a
// good after this seat has said good.
uint8_t gd_legal(const gd_game *g, uint8_t seat, const gd_move *m);
// Apply if gd_legal; returns 1 when applied. Settles the game's end. `r` may be
// NULL (a clone that plays without the repetition rule).
uint8_t gd_apply(gd_game *g, gd_rep *r, uint8_t seat, const gd_move *m);

// ---- reading the board ---------------------------------------------------------
uint8_t gd_count(const gd_mask *m);
uint8_t gd_hand_n(const gd_game *g, uint8_t seat);
uint8_t gd_uncovered(const gd_game *g);
uint8_t gd_result(const gd_game *g);

#endif
