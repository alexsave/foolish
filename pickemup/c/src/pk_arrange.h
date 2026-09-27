/* Pick 'Em Up - this phone's own arrangement of its own hand
 * (ORCHESTRATION O9, RULES_AND_KERNEL D54 to D57).
 *
 * THE WIRE NEVER SEES IT. A hand is acquisition order in PkGame, on the wire
 * and in every event (a PLAY codes a position in acquisition order, 4.4); the
 * arrangement is a per-phone permutation laid over it, kept in the phone's
 * own record and applied only where a host draws the hand. So there is one
 * owner of hand order for the rules (pk.c) and one for the picture (here),
 * and no second derivation of either (D24's worry, answered by O9).
 *
 * AN ENTRY IS AN ACQUISITION, NOT A CARD. Entry (card, receipt) means "the
 * receipt-th card this seat was ever given, which was `card`" (the deal
 * counts 1..7, then every draw and penalty card in order). The receipt is a
 * fact of the history, computed by one replay (pk_arr_receipts), so:
 *   - a card drawn now is a new receipt and goes on the RIGHT;
 *   - a played card's entry stays, so an undo (or a lost race, 4.8) that puts
 *     the same receipt back in the hand puts it back in its slot;
 *   - the same card id coming back by a later draw (after a reshuffle) is a
 *     new receipt, so its stale entry is dropped and it goes on the right;
 *   - a received bubble rebuilds the hand by replay, and the receipts of the
 *     cards already held are the same, so nothing moves;
 *   - an arrangement recorded for some other hand matches nothing, and the
 *     hand reads in acquisition order: it is reset, never trusted.
 * At most one entry per card id, so the list never outgrows the deck.
 *
 * Plain integers, no pointers, no allocation, no libc past memcpy / memset. */
#ifndef PK_ARRANGE_H
#define PK_ARRANGE_H

#include "pk.h"

typedef struct {
    uint8_t  id[8];              /* the game (pk_game_id), set by the owner          */
    uint8_t  seat;               /* whose hand, PK_SEAT_NONE for an unused slot      */
    uint8_t  n;                  /* entries                                          */
    uint8_t  card[PK_DECK];      /* left to right, a card id at most once            */
    uint16_t receipt[PK_DECK];   /* which receipt of the seat's it is, 1-based       */
} PkArr;

/* Empty: every hand reads in acquisition order. */
void pk_arr_reset(PkArr *a, const uint8_t id[8], int seat);

/* Every entry a card id at most once, a receipt of at least 1, a seat on the
 * table or none. */
int  pk_arr_valid(const PkArr *a);

/* Each hand position's receipt, from one replay of the history. 1, or 0 when
 * the history does not replay or disagrees with the hand. */
int  pk_arr_receipts(const PkGame *g, int seat, uint16_t out[PK_HAND_CAP]);

/* Fold `seat`'s hand in `g` into the arrangement: a receipt not in it goes on
 * the right, in acquisition order, replacing any older entry of that card id.
 * An invalid arrangement is emptied first. 1 changed, 0 unchanged, -1 the
 * receipts could not be computed (nothing changed). */
int  pk_arr_sync(PkArr *a, const PkGame *g, int seat);

/* Where each of the `n` cards of `hand` (acquisition order, a view's or a
 * frame's) is drawn: slot[i] for position i, a permutation of 0..n-1. The
 * cards the arrangement holds keep its order; any other card follows them in
 * the order given. Matched by card id, so it answers for a frame of the
 * board at any moment of a plan without a replay. */
void pk_arr_slots(const PkArr *a, const uint8_t *hand, int n, uint8_t *slot);

/* The hand position drawn at `slot`, or -1. */
int  pk_arr_pos(const PkArr *a, const uint8_t *hand, int n, int slot);

/* Move the card at arranged slot `from` to arranged slot `to` (the others
 * close up behind it and open in front of it). Every card of `hand` must be
 * in the arrangement (pk_arr_sync first). 1 moved; 0 for a slot off the hand,
 * from == to, or a card the arrangement does not hold. */
int  pk_arr_move(PkArr *a, const uint8_t *hand, int n, int from, int to);

/* The record's bytes: id, seat, n, then n x (card, receipt low, receipt high),
 * zero-filled to PK_ARR_LEN. pk_arr_get resets `a` and returns 0 for bytes
 * that are not a valid arrangement. */
#define PK_ARR_LEN (10 + 3 * PK_DECK)
void pk_arr_put(const PkArr *a, uint8_t out[PK_ARR_LEN]);
int  pk_arr_get(PkArr *a, const uint8_t in[PK_ARR_LEN]);

#endif
