// Legal-move enumeration. Mirrors calculateLegalMoves in
// server/api/common/bot_strategy.ts.
#ifndef CNITRO_LEGAL_H
#define CNITRO_LEGAL_H

#include "card.h"
#include "game.h"
#include <stdbool.h>

#define MOVE_ATTACK 0
#define MOVE_COVER  1
#define MOVE_PASS   2
#define MOVE_PICKUP 3
#define MOVE_GOOD   4
#define MOVE_WAIT   5

// Capacities are build parameters (like MAX_LOG_PAIRS): the native arena
// keeps the compact bot-vs-bot sizes; the WASM production build widens them
// (-DMAX_MOVE_CARDS=40 -DMAX_LEGAL_MOVES=65536) because human games reach
// states (huge post-pickup hands) the arena never does. Moves past the caps
// are dropped in enumeration order — a deliberate, documented bound that
// replaces the old TS enumerator's unbounded combinatorial blow-up.
#ifndef MAX_MOVE_CARDS
#define MAX_MOVE_CARDS 8         // hand size 6 + slack
#endif
typedef struct {
    int8_t type;
    int8_t n_cards;
    Card   cards[MAX_MOVE_CARDS];
    Card   attack_cards[MAX_MOVE_CARDS]; // cover only
} LegalMove;

#ifndef MAX_LEGAL_MOVES
#define MAX_LEGAL_MOVES 4096
#endif

typedef struct {
    int      n;
    LegalMove moves[MAX_LEGAL_MOVES];
} LegalMoves;

void calculate_legal_moves(const Game *g, int bot_idx, LegalMoves *out);

// Scoped output cap: generation appends (and the combinatorial recursions
// prune) at `cap` moves instead of MAX_LEGAL_MOVES, so callers may enumerate
// into buffers with fewer than MAX_LEGAL_MOVES slots (the solver scratch).
// 0 or out-of-range resets to MAX_LEGAL_MOVES. Thread-local; set immediately
// around the calculate_legal_moves call and reset after.
void legal_set_move_cap(int cap);

// Faster variant for use inside Monte Carlo simulations where every player
// plays a deterministic policy (handwritten). Skips the combinatorial cover
// enumeration — emits one greedy lowest-cost full-cover move instead, which
// matches handwritten's pick. Attack/pass enumerations are unchanged.
void calculate_legal_moves_lite(const Game *g, int bot_idx, LegalMoves *out);

// One-tap cover resolution (F9). Given `n_cover` selected cover cards and the
// current table, decide whether they cover the uncovered attacks in exactly ONE
// unambiguous way — every valid full pairing of cover cards to distinct
// uncovered attacks covers the SAME set of attacks. On success writes, for each
// cover card, the attack it covers (index-aligned with cover_cards) into
// out_attacks[] and returns 1. Otherwise returns 0 and the caller lets the
// player place cards manually.
//
// ITS ONE CALLER IS THE WEB, through wasm_unambiguous_cover. This used to claim
// every client called it; none of the Apple ones did, and the header said so
// for long enough that iOS grew its own answer in Swift instead. That answer is
// now play_resolve below, which asks a DIFFERENT question - "does exactly one
// entry of the enumerated menu use this selection" rather than "is there
// exactly one pairing" - and the two are not interchangeable: this one pairs
// several cover cards onto several attacks at once, while the menu already has
// each pairing as its own entry. Both are the kernel's; neither is a mirror of
// the other.
int unambiguous_cover(const Card *cover_cards, int n_cover,
                      const Battle *battles, int n_battles, int power_suit,
                      Card *out_attacks);

// ---------- the packed menu wire -------------------------------------------
//
// How an enumerated menu leaves the kernel: u32 count (LE), then per move
//   type(1), n_cards(1), cards[n_cards], attacks[n_cards]
// with each card byte card_to_id() and LEGAL_WIRE_NONE for "no card" (the
// attack bytes are padding on anything but a cover). The layout was written out
// by hand in ios_api.c and again in wasm_api.c; it lives here now, once, with a
// reader beside the writer so the two cannot drift.

#define LEGAL_WIRE_NONE   0xFE
#define LEGAL_WIRE_ECAP   (-3)
#define LEGAL_WIRE_EPARSE (-2)

// Write `count` moves starting at `start` (clamped to the menu) as that wire.
// Returns the byte length, or LEGAL_WIRE_ECAP if `cap` cannot hold it.
int legal_menu_write(const LegalMoves *lm, int start, int count,
                     unsigned char *out, int cap);

// One entry as it sits ON THE WIRE - borrowed, undecoded, valid until the
// caller's buffer moves. Card bytes stay bytes so a comparison against a
// selection that also arrived as bytes needs no decode at all.
typedef struct {
    int type;                       // MOVE_*
    int n_cards;
    const unsigned char *cards;     // n_cards bytes
    const unsigned char *attacks;   // n_cards bytes, meaningful only for COVER
} MenuMove;

// A walk over one menu. `index` is the entry legal_menu_next just handed back,
// which is what a caller returns when it has found the move it wanted.
typedef struct {
    const unsigned char *buf;
    int len;
    int n;        // the header count
    int index;    // -1 before the first entry
    int q;        // byte cursor
} MenuWalk;

// Start a walk. Returns the entry count, or LEGAL_WIRE_EPARSE for a buffer too
// short to be a menu.
int legal_menu_begin(MenuWalk *w, const unsigned char *buf, int len);

// The next entry: 1 and `out` filled, 0 after the last one, or
// LEGAL_WIRE_EPARSE for an entry that runs off the end of the buffer. Every
// offset is bounds-checked before it is read.
int legal_menu_next(MenuWalk *w, MenuMove *out);

// ---------- what a gesture on a board means --------------------------------
//
// The rules a client applies between a finger and a move: which menu entry a
// drop resolves to, which battles a selection could cover, which one the cover
// button aims at, and which moves a human may actually make. They were Swift
// (ios/FoolishKit/Boards/CardPlay.swift) and are the same answers on any
// screen, so they are here.
//
// THE BOARD IS THE CLIENT'S PUBLISHED PAIR, NOT THE LIVE GAME. A client hands
// in the menu the kernel enumerated for its seat and the table it was
// enumerated on, and these rules read nothing else. That is deliberate: a board
// publishes a snapshot, sometimes a doctored one (the iMessage board publishes
// an EMPTY menu while it holds a bout settlement back, so the player cannot act
// on cards they have not been shown yet), and it asks these questions from a
// render pass that has no way to reach the live game at all. A rule that
// re-derived the menu from the current state would answer about a position
// other than the one on screen.
//
// Everything crosses as wire bytes, so this is allocation-free and cheap enough
// to call on every frame of a drag.
typedef struct {
    const unsigned char *menu;    // the packed menu wire for this seat
    int menu_len;
    // 2 bytes per battle: the attack, then its cover or LEGAL_WIRE_NONE.
    const unsigned char *table;
    int n_battles;
    int power_suit;               // trump suit 0..3, or -1 for none
    int is_defender;              // does this seat defend the current bout
} PlayBoard;

// Where a gesture ended. A battle is named by its index (0..n_battles-1).
#define PLAY_TARGET_HAND  (-2)
#define PLAY_TARGET_TABLE (-1)

// The menu index a gesture resolves to, or -1 when it names no legal move.
//
// A DROP BACK IN THE HAND IS A REARRANGE FOR BOTH ROLES, never a play. The
// attacker branch reads only the cards, so without that answered first a
// resolver told "the hand" would hand back a perfectly good attack.
int play_resolve(const PlayBoard *b, const unsigned char *sel, int n_sel,
                 int target);

// Which battles this selection could legally cover, as a bitmask of battle
// indices - the drop-target highlight. Battles past 63 are not represented.
uint64_t play_coverable_battles(const PlayBoard *b,
                                const unsigned char *sel, int n_sel);

// Which battle the COVER BUTTON aims a selection at, or -1 for none.
//
// The highest attack this selection can beat, where every trump outranks every
// non-trump; ties go to the leftmost so two identical taps cannot disagree.
// Spending a card on the biggest thing it beats keeps the most of a hand
// useful; the leftmost coverable index, which is what a plain first-match
// gives, is not a rule at all - it is the order the attackers happened to throw
// in. The DRAG path names its own target and never comes here.
int play_best_cover_target(const PlayBoard *b,
                           const unsigned char *sel, int n_sel);

// Does the menu hold a `move_type` (MOVE_ATTACK / MOVE_PASS) move made of
// exactly this selection - the Attack and Pass buttons' enable state.
int play_has_verb(const PlayBoard *b, int move_type,
                  const unsigned char *sel, int n_sel);

// May this seat say good yet? The kernel menu always offers GOOD (bots need it,
// and the actor mask has to agree with the menu), but a human may not end a
// bout over an attack that is still uncovered, and an empty table has no bout
// to end. That gate is a rule about the UI, not about legality, which is why it
// is applied here rather than by narrowing the menu.
int play_can_say_good(const PlayBoard *b);

// The moves a HUMAN may make on this board, written back out as the same menu
// wire: the kernel's menu minus `wait`, minus `good` unless play_can_say_good.
// Returns the byte length or LEGAL_WIRE_ECAP / LEGAL_WIRE_EPARSE.
//
// The same answer as play_can_say_good, as a SET, for the callers that ask "can
// this seat do anything at all" rather than "is this one button live". They
// have to agree: a turn handoff reading the raw menu passes the game to a seat
// whose only offer is a good the board will not let it make, and the run stops
// with no button on screen.
int play_human_menu(const PlayBoard *b, unsigned char *out, int cap);

#endif
