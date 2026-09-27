/* Pick 'Em Up - what one seat can know about the others (the bot's belief).
 *
 * THE SHAPE IS FOOLISH'S OCTOGEN (foolish/c/src/octogen_strategy.c,
 * og_build_belief and og_sample_world): one chronological pass over the
 * public history builds an unseen pool, the cards publicly located in a
 * seat's hand ("pinned"), and void constraints; a sampler deals the pool
 * into a consistent world, constrained slots first, and repairs what it
 * cannot satisfy rather than failing. RULES_AND_KERNEL.md "Bot" and
 * DECISION D59 onwards say what is deducible under this game's rules.
 *
 * PUBLIC INFORMATION ONLY. The pass reads the kernel's own event stream
 * masked for the deciding seat (pk_plan_each with viewer = me), which is
 * exactly what that seat's phone is shown: every card played, every draw as
 * a face-down card, every penalty, every flip and bury of the start card.
 * From the game struct it reads only the deciding seat's own hand, the stack
 * (every card on it was played face up) and the live suit. It never reads
 * another seat's hand[] or the deck[] order; tests/pk_bot_test.c scrambles
 * both and proves the belief does not move.
 *
 * What the rules make deducible (D59):
 *   - HAND SIZES. Seven dealt, plus every draw and penalty card, minus every
 *     play: all public events, so the count is exact even though a screen
 *     never shows it (D22).
 *   - BURIED CARDS. A non-number flipped at the start goes face up to the
 *     bottom of the deck (D14), so its deck position is known; whoever draws
 *     that position holds it until they play it.
 *   - A BARE PASS (a pass with no draw this turn) is legal only with nothing
 *     to draw and nothing playable (D10), so at that moment the seat held no
 *     wild, no card of the live suit and none of the top card's rank. HARD:
 *     every sampled world obeys it.
 *   - A DRAW PROVES NOTHING (D6: a seat may draw while holding a playable
 *     card). Most players only draw when stuck, so the first draw of a turn
 *     and a pass after drawing are recorded as SOFT voids of the same shape,
 *     obeyed in a share of the worlds (octogen's void world-mixture) and
 *     dropped for a seat whose later plays contradict them (octogen's
 *     per-seat distrust).
 *
 * A VOID BINDS THE CARDS HELD WHEN IT WAS SEEN, not cards drawn later, and
 * the history cannot say which later plays were old cards. So each void
 * carries k, the number of cards it still binds: the hand size when seen,
 * less one for every card that seat has played since. Voids of one seat nest
 * (a later one binds its whole hand, a superset of what an earlier one
 * still binds), so a seat's unknown slot i must avoid every void whose k
 * exceeds i.
 *
 * Deterministic, no allocation, no libc beyond memcpy / memset. */
#ifndef PK_BELIEF_H
#define PK_BELIEF_H

#include "pk.h"

#define PK_BELIEF_VOIDS 6          /* per seat, per kind; the oldest drop first */

/* Where a card is, as the deciding seat knows it. */
enum {
    PK_LOC_UNSEEN = 0,             /* in the pool: some other hand or the deck    */
    PK_LOC_MINE,                   /* in the deciding seat's own hand             */
    PK_LOC_STACK,                  /* on the stack, face up                       */
    PK_LOC_DECK,                   /* a buried card, still at its known position  */
    PK_LOC_SEAT                    /* PK_LOC_SEAT + s: publicly in seat s's hand  */
};

/* "Holds no card like this": a suited card whose suit bit is set, or any card
 * whose rank bit is set (rank bits 1..14, PK_R_WILD and PK_R_WILD4 included). */
typedef struct {
    uint16_t ranks;
    uint8_t  suits;
    uint8_t  k;                    /* cards of the seat it still binds            */
    uint8_t  gained;               /* cards the seat gained since it was seen     */
    uint8_t  hits;                 /* plays since that it forbids                 */
    uint8_t  pad0, pad1;
} PkVoid;

typedef struct {
    uint8_t  me, n;
    uint8_t  top, live_suit;
    uint8_t  count[PK_MAX_SEATS];              /* hand sizes, from the events     */
    uint8_t  pinned_n[PK_MAX_SEATS];
    uint8_t  pinned[PK_MAX_SEATS][PK_HAND_CAP];
    uint8_t  loc[PK_DECK];                     /* PK_LOC_*                        */
    uint8_t  deck_n;                           /* cards in the deck               */
    uint8_t  deck_known_n;                     /* deck[0..deck_known_n) known     */
    uint8_t  deck_known[PK_DECK];
    uint8_t  pool_n;                           /* unseen cards                    */
    uint8_t  pool[PK_DECK];                    /* ascending id order              */
    uint8_t  hard_n[PK_MAX_SEATS], soft_n[PK_MAX_SEATS];
    PkVoid   hard[PK_MAX_SEATS][PK_BELIEF_VOIDS];
    PkVoid   soft[PK_MAX_SEATS][PK_BELIEF_VOIDS];
    uint8_t  distrust;                         /* bit s: seat s broke a soft void */
    uint8_t  ok;                               /* 1 when the history replayed     */
    uint16_t plays[PK_MAX_SEATS];              /* cards each seat played          */
    uint16_t draws[PK_MAX_SEATS];              /* own draws (not penalties)       */
} PkBelief;

/* Build `b` for seat `me` from g's public history. 1, or 0 when the history
 * does not replay (b->ok says the same). */
int pk_belief_build(PkBelief *b, const PkGame *g, int me);

/* May seat s's unknown slot `slot` hold card c? use_soft: obey soft voids too
 * (a distrusted seat's are never obeyed). */
int pk_belief_allows(const PkBelief *b, int s, int slot, uint8_t c, int use_soft);

/* One world consistent with `b`: `w` becomes a copy of `g` whose other hands
 * and deck order are dealt from the pool (buried cards at their positions,
 * constrained slots first, a slot nothing fits takes any card), and whose
 * seed is replaced from `seed`, so a reshuffle inside the world is not the
 * real one (the real seed decides the real reshuffles, which no seat may
 * read ahead). Returns the number of slots that could not be satisfied. */
int pk_belief_sample(const PkBelief *b, const PkGame *g, uint64_t seed, int use_soft, PkGame *w);

#endif
