/* Pick 'Em Up - the masked per-seat view (RULES_AND_KERNEL.md 3.8).
 *
 * WHAT A HOST DRAWS COMES FROM HERE, and nothing here can show another
 * seat's card count while the game is being played (D22). Not a hidden
 * field, not a zeroed-but-present field: the struct has no slot for one
 * until the game is over, so no renderer can show one by accident and no
 * code review has to catch it. The kernel still knows every count
 * (PkGame.hand_n); animation learns them only as a stream of single-card
 * events (pk_plan.h), which is public information.
 *
 * A seat's fan is drawn at a constant size whatever it holds
 * (PK_FAN_BACKS), and the fan is the tap target for "Caught you!" (can_call).
 *
 * `reveal_n` / `reveal_hand` are the only per-seat counts in the struct, and
 * they are filled ONLY once the game is over (or for PK_VIEW_ALL). They are
 * named for the end reveal on purpose: they are not a live count.
 *
 * Casual trust, as in foolish: the seed is in every bubble, so anyone who
 * decodes their own bubble can compute every hand. Masking removes the
 * one-tap version; it is not cheat-resistance. */
#ifndef PK_VIEW_H
#define PK_VIEW_H

#include "pk.h"

/* How many card backs every other seat's fan shows while playing. */
#define PK_FAN_BACKS 3

typedef struct {
    uint8_t n, turn, dir, live_suit, over, winner;
    uint8_t show_dir;               /* 0 at 2 players: the word is not drawn (D13) */
    uint8_t top;                    /* stack top id                                 */
    uint8_t deck_n;                 /* shown ("27 left", D22)                       */
    uint8_t stack_n;                /* the pile's height, for drawing it            */
    uint8_t said;                   /* LAST stamps, public                          */
    uint8_t me;                     /* viewer seat, PK_SEAT_NONE for a spectator    */
    uint8_t my_exposed;             /* 1 if the viewer may say "Last card!" now     */
    uint8_t can_draw, can_pass, can_seal, can_undo;
    uint8_t can_call;               /* bit t: the viewer may catch seat t           */
    uint8_t draft_open;             /* the viewer has a bubble open                 */
    uint8_t draft_said;             /* ... holding "Last card!"                     */
    uint8_t draft_call;             /* ... catching this seat, or PK_SEAT_NONE      */
    uint8_t my_n;                   /* the viewer's own hand                        */
    uint8_t my_hand[PK_HAND_CAP];
    uint8_t my_playable[PK_HAND_CAP]; /* 1 per position that PLAY accepts          */
    /* only when over (or PK_VIEW_ALL): every hand, face up */
    uint8_t reveal_n[PK_MAX_SEATS];
    uint8_t reveal_hand[PK_MAX_SEATS][PK_HAND_CAP];
} PkView;

/* `viewer`: a seat, PK_VIEW_SPECTATOR or PK_VIEW_ALL. Every byte of `out` is
 * written, so two views compare with memcmp. */
void pk_view(const PkGame *g, int viewer, PkView *out);

#endif
