/* Pick 'Em Up - the plan's events laid out on a clock (UI.html's motion grid).
 *
 * THE TIMELINE IS THE KERNEL'S. pk_plan says WHAT happened, in the order the
 * kernel applied it; this file says WHEN each thing moves, for how long, on
 * which curve, from which anchor to which, and what the board shows at any
 * moment of it. A host samples it every frame and tweens between the anchors
 * its views report; it holds no duration, no easing and no ordering of its
 * own (foolish's anim_plan_at shape: a plan and a sampler, never a scheduler
 * in a view).
 *
 * THE NUMBERS ARE UI.html's, once: the vocabulary table and the `T` / `E`
 * objects of its demo script. The ORDER is the motion grid's order badges
 * (equal numbers move together; a row's B steps wait for the one before).
 * Where the grid's prose and a demo disagree, pickemup/docs/ANIMATION_DECISIONS.md
 * says which won and why.
 *
 * THREE KINDS OF EVENT IN A BUILD. Each plan event is PLAYED (it gets beats
 * with times), DONE (it already played at an earlier tap: its effect is in
 * the frame at t = 0 and it moves nothing) or HELD (a settle event of the
 * bubble's last turn while it is staged: the cut, 5.3.6; it neither moves nor
 * shows until Send). The mode decides which, from `half` alone.
 *
 * Fixed-layout structs, no allocation, no libc: the file builds for wasm32
 * and is read from Swift through structgen's readers. */
#ifndef PK_BEATS_H
#define PK_BEATS_H

#include "pk.h"
#include "pk_plan.h"
#include "pk_view.h"

/* ---- the vocabulary (UI.html "The vocabulary", demo `const T`) ----------- */
#define PK_T_FLIGHT        500   /* foolish flightTime: a play, the start card   */
#define PK_T_GAP            25   /* foolish flightGap: between two steps          */
#define PK_T_DRAW          320   /* one drawn card, deck to hand                  */
#define PK_T_DRAW_FLIP     180   /* ...then it turns over where it landed         */
#define PK_T_DRAW_STEP     110   /* start to start of replayed and penalty draws  */
#define PK_T_DEAL          320   /* one dealt card                                */
#define PK_T_DEAL_FLIP     160   /* my dealt card turns over where it landed      */
#define PK_T_DEAL_SPREAD  1800   /* U20: start to start is SPREAD / cards ...     */
#define PK_T_DEAL_MIN       45   /* ... clamped to 45 ...                         */
#define PK_T_DEAL_MAX      110   /* ... and 110                                   */
#define PK_T_DEAL_REST     250   /* the deal lands, then the start card turns     */
#define PK_T_FLIP_REST     300   /* a turned-up non-number is seen before it goes */
#define PK_T_BURY_REST     120   /* a buried card settles before the next flip    */
#define PK_T_HALF          170   /* TURN: each half of the rotate-out / rotate-in */
#define PK_T_FADE          220
#define PK_T_STAMP         340
#define PK_T_POP           260   /* a suit tile pops out of the wild              */
#define PK_T_POP_STEP       30   /* ... the next one 30ms later                   */
#define PK_T_COLLAPSE      200   /* the tiles fall back into the card             */
#define PK_T_SHAKE          60   /* one of three refused-undo shakes              */
#define PK_T_DECK_SHAKE    180   /* the empty deck shakes once (PENALTY_SHORT)    */
#define PK_T_SPRING        320   /* FMotion.card's response                       */
#define PK_T_SPRING_DAMP    82   /* ... and its damping, in hundredths            */
#define PK_T_GATHER        360   /* reshuffle: an under-card slides to the deck   */
#define PK_T_GATHER_STEP    40   /* ... the next one 40ms later                   */
#define PK_T_FATTEN        240   /* ... the deck fattens                          */
#define PK_T_RIFFLE        140   /* ... one riffle of one layer                   */
#define PK_T_RIFFLE_STEP     8   /* ... the next layer 8ms later                  */
#define PK_T_SLASH         260   /* a skipped seat's red slash wipes across       */
#define PK_T_DIM           900   /* ... its badge dims to .45 and back            */
#define PK_T_SHRUG         200   /* a passer's fan                                */
#define PK_T_RING          120   /* a called fan presses and rings                */
#define PK_T_PULSE         120   /* the strip's chip counts up                    */
#define PK_T_REVEAL_FLIP   160   /* the end reveal turns one back                 */
#define PK_T_REVEAL_STEP    60   /* ... the next one 60ms later                   */
#define PK_T_GAME_OVER    1000   /* foolish gameOverHold                          */
#define PK_T_LOBBY_REST    500   /* ANIM_SURFACE_HOLD_MS: the lobby rests         */
#define PK_T_LEAD_LIVE      16   /* a sequence starts one beat after the touch    */
#define PK_T_LEAD_OPEN     100   /* ... or after a bubble was opened              */
#define PK_T_COLLAPSE_WAIT 250   /* a staged play: 250 + the plan + 500, then the */
#define PK_T_COLLAPSE_REST 500   /* drawer may collapse (foolish)                 */

/* How many under-cards the pile draws beneath its top one (UI.html `.und`):
 * also how many ghosts a reshuffle's gather slides into the deck. */
#define PK_BEAT_UNDER       3
/* The deck's layers while the reshuffle gag fattens and riffles it. */
#define PK_BEAT_FAT_LAYERS  8

/* ---- anchors (UI.html "What holds which edge") -------------------------- */
enum {
    PK_ANC_NONE = 0,
    PK_ANC_DECK,        /* the deck's top layer                                  */
    PK_ANC_STACK,       /* the pile's top card                                   */
    PK_ANC_HAND,        /* my hand, card i (anchor hand.i)                        */
    PK_ANC_FAN,         /* seat k's fan (fan.k); a flight lands at its right end  */
    PK_ANC_SLOT,        /* seat k's 40pt stamp slot (slot.k)                      */
    PK_ANC_SEAT,        /* seat k's badge (seat.k)                                */
    PK_ANC_DIR,         /* the direction box                                     */
    PK_ANC_STRIP,       /* the staged-turn strip                                  */
    PK_ANC_PICKER,      /* suit tile k (0..3), 4 the x                            */
    PK_ANC_SCRIM,
    PK_ANC_BURY,        /* under the deck, where a buried start card peeks out   */
    PK_ANC_RESULTS,     /* the results plank                                     */
    PK_ANC_BOARD,       /* the whole table (the lobby fades to it)               */
    PK_ANC_ROW,         /* lobby roster row k                                    */
    PK_ANC_COUNT
};

/* ---- curves (UI.html demo `const E`) -------------------------------------- */
enum {
    PK_EASE_LINEAR = 0,
    PK_EASE_FLIGHT,     /* cubic-bezier(.25,.46,.45,.94)                         */
    PK_EASE_SPRING,     /* cubic-bezier(.3,1.22,.45,1), card-spring's stand-in   */
    PK_EASE_STAMP,      /* cubic-bezier(.2,1.5,.5,1)                             */
    PK_EASE_IN,         /* cubic-bezier(.55,0,.75,.2): a flip's first half      */
    PK_EASE_OUT,        /* cubic-bezier(.25,.8,.45,1): a flip's second half     */
    PK_EASE_EASE_OUT,   /* CSS ease-out (0,0,.58,1)                              */
    PK_EASE_COUNT
};

/* ---- what a beat does ------------------------------------------------------ */
enum {
    PK_BK_NONE = 0,
    PK_BK_FLIGHT,       /* a card (or a back) from one anchor to another         */
    PK_BK_FLIP,         /* a card turns over where it is                         */
    PK_BK_GATHER,       /* reshuffle: `parts` under-cards slide stack -> deck     */
    PK_BK_FATTEN,       /* reshuffle: the deck swells (.7 -> 1.14 -> 1)           */
    PK_BK_RIFFLE,       /* the deck's layers splay and snap back                 */
    PK_BK_HALO,         /* the pile's halo cross-fades to `suit`                  */
    PK_BK_BAND,         /* a wild's chosen-suit band slides up its foot          */
    PK_BK_STAMP,        /* LAST / Caught you! / Wrong call / OUT (sub) into a slot */
    PK_BK_SLASH,        /* a skipped fan's red bar wipes across, then goes       */
    PK_BK_DIM,          /* dims to amp% and back (a skipped badge, my hand)      */
    PK_BK_TURN_BAR,     /* the turn bar leaves seat from_i for seat to_i         */
    PK_BK_TURN,         /* the direction box rotates out and back in (sub: dir)  */
    PK_BK_FADE,         /* an anchor fades in (sub 1) or out (sub 0)             */
    PK_BK_SHAKE,        /* side to side, `parts` times, amp points               */
    PK_BK_SHRUG,        /* a passer's fan: scale down amp% and back              */
    PK_BK_PULSE,        /* scale up amp% and back (the strip's chip)             */
    PK_BK_RING,         /* a called fan presses in and rings brass               */
    PK_BK_POP,          /* suit tiles pop out of the card to their compass points */
    PK_BK_COLLAPSE,     /* ... and fall back into it                            */
    PK_BK_HOLD,         /* nothing moves: a rest the sequence waits out          */
    PK_BK_COUNT
};

/* STAMP subs */
enum { PK_STAMP_LAST = 1, PK_STAMP_CAUGHT, PK_STAMP_WRONG, PK_STAMP_OUT };

/* Beat flags: what the beat does to the board as it plays (pk_beats_frame). */
enum {
    PK_BF_FACE_MID   = 1,    /* a back that turns face up halfway (a seat's play) */
    PK_BF_ADDS       = 2,    /* lands as a new hand card at to_i, unseen until shown */
    PK_BF_SHOWS      = 4,    /* at its end, hand card to_i is seen                 */
    PK_BF_HIDES      = 8,    /* at its start, hand card to_i is unseen             */
    PK_BF_LEAVES     = 16,   /* at its start, hand card from_i is gone             */
    PK_BF_TOP        = 32,   /* at its end, `card` is the pile's top               */
    PK_BF_RETRACT    = 64,   /* foolish's red retraction ghost (a lost race)       */
    PK_BF_DEAL       = 128,  /* a dealt card: the fan it lands in is no longer empty */
};

#define PK_BEAT_NO_DECK 0xFF  /* deck_n: this beat does not change the count */
#define PK_BEAT_NO_EVENT 0xFFFF

typedef struct {
    uint32_t start_ms;   /* from the plan's start                                 */
    uint16_t dur_ms;     /* the whole envelope, every part included               */
    uint16_t part_ms;    /* one part's own run (dur_ms with a single part)        */
    uint16_t ev_i;       /* the plan event it plays, or PK_BEAT_NO_EVENT          */
    uint8_t  kind;       /* PK_BK_*                                               */
    uint8_t  ease;       /* PK_EASE_*                                             */
    uint8_t  from, from_i;
    uint8_t  to, to_i;
    uint8_t  card;       /* a card id, PK_CARD_HIDDEN (a back) or PK_CARD_NONE    */
    uint8_t  flags;      /* PK_BF_*                                               */
    uint8_t  parts;      /* 1, or the layers / ghosts / tiles / shakes            */
    uint8_t  stagger_ms; /* start to start of two parts                           */
    uint8_t  deck_n;     /* the count the deck shows from this beat on, or NO_DECK */
    uint8_t  bulge;      /* a flight's peak scale, in hundredths (115, 108, 105)  */
    int8_t   rot0, rot1; /* degrees at the start and at the end                   */
    int8_t   amp;        /* points or percent, by kind                            */
    uint8_t  seat;       /* the seat it is about, or PK_SEAT_NONE                 */
    uint8_t  sub;        /* STAMP kind, TURN's new dir, FADE in (1) / out (0)     */
    uint8_t  suit;       /* HALO / BAND: the suit                                 */
    uint8_t  ev_kind;    /* the PK_EV_* it plays, 0 for a host motion             */
    uint8_t  pad0;
} PkBeat;                /* 32 bytes */

#define PK_BEATS_MAX 1024

/* ---- the board as of a moment ------------------------------------------------
 *
 * What the host draws INSTEAD of its settled view while a plan plays: the
 * count, the pile, the direction, whose turn, my hand with the cards still in
 * the air unseen, and what is still held back. At the plan's end it is the
 * settled view again, except for what a staged draft holds until Send. */
enum {
    PK_HOLD_DIR      = 1,    /* the direction box has not faded in yet            */
    PK_HOLD_RESULTS  = 2,    /* the results plank has not faded in yet            */
    PK_HOLD_PENDING  = 4,    /* the picker's wild has not reached the pile yet    */
    PK_HOLD_BOARD    = 8,    /* the table has not faded in over the lobby yet     */
};

typedef struct {
    uint8_t  deck_n;
    uint8_t  top;                        /* PK_CARD_NONE for an empty pile         */
    uint8_t  suit;                       /* the halo's suit                        */
    uint8_t  dir;                        /* PK_DIR_*                               */
    uint8_t  turn;                       /* the turn bar's seat, or NONE           */
    uint8_t  stack_n;
    uint8_t  hold;                       /* PK_HOLD_*                              */
    uint8_t  stamp_hold;                 /* bit s: seat s's stamp has not landed   */
    uint8_t  fans_empty;                 /* bit s: the deal has not reached s yet  */
    uint8_t  buried_hold;                /* buried start cards not under the deck yet */
    uint8_t  revealing;                  /* the end reveal has begun               */
    uint8_t  done;                       /* every beat has run                     */
    uint8_t  reveal_shown[PK_MAX_SEATS]; /* per seat, backs turned face up so far  */
    uint8_t  my_n;
    uint8_t  pad0;
    uint8_t  my_hand[PK_HAND_CAP];
    uint8_t  my_unseen[PK_HAND_CAP];     /* 1: that card's slot is open, the card is in the air */
    uint32_t now_ms;
    uint32_t next_ms;                    /* the next moment the frame changes, or PK_BEAT_NEVER */
} PkBeatFrame;

#define PK_BEAT_NEVER 0xFFFFFFFFu

/* One beat sampled at a moment: where along its path, and the transform the
 * grid gives it there. `part` picks one layer / ghost / tile / reveal card. */
enum { PK_BS_PENDING = 0, PK_BS_ACTIVE = 1, PK_BS_DONE = 2 };
typedef struct {
    float   p;          /* eased progress along from -> to, 0..1 (may overshoot) */
    float   scale;      /* uniform scale about the centre                         */
    float   scale_x;    /* a flip's width, 0..1                                   */
    float   rot;        /* degrees (TURN: about Y)                                */
    float   dx, dy;     /* points                                                 */
    float   opacity;
    uint8_t state;      /* PK_BS_*                                                */
    uint8_t face;       /* 1: draw the card face, 0: its back                     */
    /* 1: the host applies this transform now. Always while active; before its
     * start for a beat that brings something IN (a pop, a stamp, a fade in:
     * CSS fill backwards), after its end for one that takes something OUT (a
     * fade out, a collapse: fill forwards). */
    uint8_t apply;
    uint8_t pad0;
} PkBeatSample;

/* ---- building ------------------------------------------------------------------ */

enum {
    PK_BEATS_OPEN = 0,   /* C and D: a bubble opened; lead 100ms                   */
    PK_BEATS_ARRIVAL,    /* E: a bubble landed while I look; lead 16ms             */
    PK_BEATS_STAGE,      /* A: my own tap, played at stage; settle of the last turn held */
    PK_BEATS_SEND,       /* B: at Send, what staging held                          */
    PK_BEATS_HOST,       /* an undo, the picker, a refusal, a lost race            */
};

/* Build flags */
enum {
    PK_BFL_WILD_PLACED = 1,  /* STAGE: my wild already sits on the pile (the picker put it there) */
    PK_BFL_PICKED      = 2,  /* STAGE (bridge): a suit tile was tapped first; suit in bits 8..15 */
};

typedef struct {
    uint16_t n;
    uint16_t held;       /* events HELD until Send: the end frame is not the settled view */
    uint32_t total_ms;   /* the last beat's end                                   */
    uint32_t settle_ms;  /* STAGE: when the drawer may collapse (250 + plan + 500) */
    uint32_t serial;     /* which build this is (the bridge counts; 0 here)       */
    uint8_t  mode, viewer, n_seats, pad0;
    PkBeatFrame start;   /* the board before the first beat                       */
    PkBeat   beat[PK_BEATS_MAX];
} PkBeats;

/* The board before a plan, from a masked view (NULL: the table before the
 * deal, `n_seats` empty seats and a full deck). */
void pk_beats_frame_of(const PkView *v, int n_seats, PkBeatFrame *out);

/* The longest event list a build takes (pk_api's PK_API_EVENTS). */
#define PK_BEATS_EVENTS 4096

/* Lay `n` plan events out. `start` is the board they begin from, `viewer` the
 * seat they were masked for (PK_SEAT_NONE for a spectator).
 *
 * STAGE: `prev` is the draft's plan as the previous tap left it. An event
 * found in it, in order, already played (DONE); a Last card! or a catch lands
 * at the FRONT of a draft (5.3.3), so "the events after index k" would be
 * wrong and the two lists are matched instead. Ignored in the other modes.
 *
 * Returns the beat count, or -1 if the plan does not fit PK_BEATS_MAX (a host
 * then snaps to the settled view). */
int pk_beats_build(const PkEvent *ev, int n, const PkBeatFrame *start, int viewer, int n_seats,
                   int mode, const PkEvent *prev, int prev_n, int flags, PkBeats *out);

/* ---- host motions, which no plan event describes -------------------------------- */
enum {
    PK_HM_UNDO = 1,      /* a: card, b: hand position it returns to               */
    PK_HM_REFUSED,       /* b: the newest drawn card's position (U23)             */
    PK_HM_PICKER_OPEN,   /* a: the wild, b: its hand position                     */
    PK_HM_PICKER_PICK,   /* a: the suit tapped                                    */
    PK_HM_PICKER_CANCEL, /* a: the wild, b: its hand position                     */
    PK_HM_RETRACT,       /* a: my staged card, b: its hand position (a lost race) */
    PK_HM_UNCALL,        /* a: the seat un-called                                 */
};
/* Append host motion `what` to `out` (after what is already there when
 * `append`, else as a new plan from `start`). The beat count, or -1. */
int pk_beats_host(int what, int a, int b, const PkBeatFrame *start, int viewer, int n_seats,
                  int append, PkBeats *out);

/* The board at the end of sealed bubble `from` (-1: before the deal), masked
 * for `viewer`: what a plan of (from, to] starts from. The game is replayed
 * to that bubble's edge, so nothing a host kept can disagree with it. 1, or 0
 * if the history does not replay. */
int pk_beats_pre(const PkGame *g, int viewer, int from, PkBeatFrame *out);

/* Shift every beat of `b` from index `first` on by `ms` (a lost race plays
 * the winner after the retraction). */
void pk_beats_delay(PkBeats *b, int first, uint32_t ms);

/* ---- sampling ------------------------------------------------------------------------ */

/* The board at `now_ms` from the plan's start. */
void pk_beats_frame(const PkBeats *b, uint32_t now_ms, PkBeatFrame *out);

/* One beat at `now_ms`; `part` in 0..parts-1. */
void pk_beat_sample(const PkBeat *b, uint32_t now_ms, int part, PkBeatSample *out);

/* A curve at t in 0..1 (the host needs none; the tests pin the curves). */
float pk_ease(int ease, float t);

/* U20's start-to-start for a deal of `cards` cards, and card i's start from
 * the first. */
int pk_beats_deal_step(int cards);
int pk_beats_deal_at(int i, int cards);

#endif
