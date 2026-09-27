/* Pick 'Em Up - the Swift-visible face of the kernel. Flat entry points, and
 * no byte layout on the far side.
 *
 * WHERE A STRUCT CROSSES, IT CROSSES AS A POINTER Swift reads through generated
 * code: pk_api_table, pk_api_view, pk_api_plan and pk_api_since return
 * `const void *` into the kernel's own storage, and the readers are
 * shared/tools/structgen's, generated from the kernel's headers
 * (pickemup/c/ios/layout.args, `make -C pickemup/c structgen`). The pointer is
 * valid until the next call into this file; a host copies what it read (the
 * generated readers return values) and holds no pointer. The kernel's
 * constants (PK_EV_*, PK_LOBBY_*, PK_PHASE_*, PK_BY_*, the PK_E* errors) come
 * with the readers.
 *
 * ONE RESIDENT MESSAGE, static rather than allocated: the roster and the game
 * together, because a move is a move by somebody in a seat. THE RESIDENT GAME
 * IS ONE SLOT: never seal or read it across an await. pk_api_read ADOPTS (a
 * decode is an adoption, there is no decode that leaves the resident alone
 * except pk_api_check), so a host that reads, awaits, and then stages is
 * staging on whatever was adopted in between.
 *
 * THIS DEVICE'S IDENTITY is whatever bytes the host says it is (Messages'
 * localParticipantIdentifier). It is hashed with each game's seed into a seat
 * tag here and never leaves the kernel. THE NICKNAME is the App Group's, typed
 * once; it is this device's name on every roster it joins. */
#ifndef PK_API_H
#define PK_API_H

#include <stdint.h>

/* The layout the generated readers must have been generated for: equal to
 * their SG_LAYOUT_HASH, or the two are not a pair. 0 for a build that was not
 * stamped (ios-smoke), which matches no module. */
uint32_t pk_api_layout_hash(void);

/* ---- who I am ------------------------------------------------------------ */

void pk_api_me(const uint8_t *id, int n);
void pk_api_nickname(const uint8_t *name, int n);
/* PK_NAME_OK 0, EMPTY 1, TOO_LONG 2 (16 characters, 48 bytes), BAD 3. */
int  pk_api_name_verdict(const uint8_t *name, int n);

/* THIS DEVICE'S SEAT RECORDS: fixed-layout bytes the host keeps and hands
 * back unread. Load once, save whenever dirty. At most PK_API_REC_BYTES. */
#define PK_API_REC_BYTES (17 * 256)
void pk_api_seats_load(const uint8_t *bytes, int n);
int  pk_api_seats_dirty(void);
int  pk_api_seats_save(uint8_t *out, int cap);    /* length, or -1; clears dirty */

/* ---- the resident message ------------------------------------------------ */

/* The longest link any game writes, with its NUL. */
#define PK_API_TEXT_MAX 4724

/* A new lobby, me in seat 0 under my nickname, from 32 bytes of the host's
 * secure random. 0, or a negative PK_E*. */
int  pk_api_new(const uint8_t seed[32], int dm);

/* ADOPT the message in `text` (a whole URL string is fine). 0, or a negative
 * PK_E* and nothing changes. Any sender fact given for exactly this text
 * (pk_api_sender) is used to seat me. */
int  pk_api_read(const char *text);
/* Whether `text` would read, WITHOUT adopting it. */
int  pk_api_check(const char *text);
/* THE SENDER FACT: `text` (the tapped bubble) was sent by this device
 * (i_sent 1) or not (0), in a chat with exactly one other person (is_dm).
 * About that one message only; NULL clears it. Compared live, never stored. */
void pk_api_sender(const char *text, int is_dm, int i_sent);

/* The resident message as the link for MSMessage.url: a draft is sealed into
 * a copy first, and the resident keeps its draft (undo still works while it is
 * staged). Length, or a negative PK_E*. */
int  pk_api_text(char *out, int cap);
/* The staged draft was sent: seal it into the resident. 1, or 0 with no draft. */
int  pk_api_commit(void);

/* ---- the lobby ------------------------------------------------------------- */

/* Each returns 0 (join and join-start: my new seat) or a negative PK_E*. */
int  pk_api_join(void);
int  pk_api_leave(void);
int  pk_api_start(void);
int  pk_api_join_start(void);

/* ---- staging a bubble (my seat, the resident game) --------------------------
 *
 * 1 if it was applied, 0 if the rules refused it. */
int  pk_api_draw(void);
int  pk_api_play(int pos, int suit);     /* suit 0..3 for a wild that is not the last card, else 4 */
int  pk_api_say_it(void);
int  pk_api_catch(int seat);
int  pk_api_pass(void);
int  pk_api_undo(void);
int  pk_api_unsay(void);
int  pk_api_uncall(void);
int  pk_api_cancel(void);                /* back to the draft's floor (D9) */
int  pk_api_can_play(int pos);           /* with some suit */
/* What a card id IS, so a face is drawn without knowing the id order (3.2):
 * its suit 0..3 (PK_NO_SUIT for a wild) and its rank 1..9 or PK_R_*; -1 for
 * an id off the deck, PK_CARD_HIDDEN included. */
int  pk_api_card_suit(int card);
int  pk_api_card_rank(int card);
int  pk_api_is_wild(int pos);            /* a play at pos needs a suit */

/* ---- reading it ---------------------------------------------------------------- */

#define PK_API_ME        (-3)            /* the viewer is my resolved seat */
#define PK_API_SPECTATOR (-1)
#define PK_API_ALL       (-2)            /* tests and the finished board only */

const void *pk_api_table(void);                          /* PkApiTable                   */
const void *pk_api_view(int viewer);                     /* PkView                       */
/* Events of bubbles (from, to], masked for `viewer`; from = -1 includes the
 * deal. NULL for a range that does not fit or a game that does not replay. */
const void *pk_api_plan(int viewer, int from, int to);   /* PkApiEvents                  */
const void *pk_api_plan_draft(int viewer);               /* PkApiEvents: my open bubble  */
const void *pk_api_since(int from, int to);              /* PkSince                      */
/* The roster's events from the lobby in `before` to the resident one. */
const void *pk_api_plan_lobby(const char *before);       /* PkApiEvents                  */

/* ---- the words ------------------------------------------------------------------
 *
 * Every line is the kernel's (pk_say.h), in the table's language; the host
 * never composes one. Each writes a NUL-terminated line and returns its
 * length, "" (0) being a real answer; -1 for a buffer too small or an
 * argument out of range. */
int  pk_api_string(int key, char *out, int cap);       /* one table entry by PK_K_* key */

#define PK_API_W_CAPTION       0   /* arg: bubble (0 the deal)            */
#define PK_API_W_HEADLINE      1   /* for me                               */
#define PK_API_W_SUBLINE       2
#define PK_API_W_DECK_LEFT     3
#define PK_API_W_DIR           4
#define PK_API_W_CARD          5   /* arg: card id                         */
#define PK_API_W_SEAT          6   /* arg: seat                            */
#define PK_API_W_SPOKEN_CARD   7   /* arg: my hand position                */
#define PK_API_W_SPOKEN_FAN    8   /* arg: seat                            */
#define PK_API_W_SPOKEN_DECK   9
#define PK_API_W_SPOKEN_STACK 10
#define PK_API_W_RULES_TITLE  11
#define PK_API_W_RULE         12   /* arg: 0..PK_RULES_N-1                 */
#define PK_API_W_INVITE       13   /* the lobby's captions; arg: the seat they name. */
#define PK_API_W_JOINED       14   /* A leave is captioned by the leaver BEFORE      */
#define PK_API_W_LEFT         15   /* pk_api_leave, while their row is still there.  */
/* The caption of the bubble pk_api_text writes now: my draft sealed into a
 * copy, else the newest sealed bubble (0, the deal, right after Start). ""
 * while WAITING: a lobby bubble is captioned by W_INVITE / JOINED / LEFT. */
#define PK_API_W_STAGED_CAPTION 16
#define PK_API_W_LOBBY_ROW    17   /* arg: seat. "2. Bo", or "2. Bo (You)" for mine */
#define PK_API_W_LOBBY_DEALER 18   /* "Alex deals": seat 0 deals (4.6.3)           */
#define PK_API_W_ERROR        19   /* arg: a negative PK_E*. Why a link did not read */
#define PK_API_W_RANK_ROW     20   /* arg: a place 0..n-1 of pk_api_ranks, "1. Cy"  */
#define PK_API_W_PUBLIC_ROW   21   /* arg: seat. "2. Bo" for the bubble's picture: no "(You)" */
#define PK_API_W_COUNT        22
int  pk_api_words(int what, int arg, char *out, int cap);

/* THE BURIED START CARDS STILL UNDER THE DECK (D14, U16): the non-numbers
 * the deal turned up and sent face up to the deck's bottom, for as long as
 * they are still there (drawn down to, or gone in a reshuffle, they are not).
 * Public, so every viewer gets the same answer. Writes and returns the count. */
int  pk_api_buried(uint8_t out[8]);

/* THE FINISHED TABLE'S ORDER: the winner first, then every other seat by
 * fewest cards left, ties in seat order. Writes n seats and returns n, or 0
 * while the game is not over (a live count is never ranked, D22). */
int  pk_api_ranks(uint8_t out[8]);

/* ---- the layout (pk_lay.c) ------------------------------------------------------
 *
 * UI.html's numbers, so a host derives none of them: the hand row (O4, U7,
 * U8), the seat ring, the fan, the deck's layers, the pile's lift and which
 * pills stand in which slot (U9). Points throughout, in the board's own
 * coordinates (the extension's view less foolish's 8/8/14/4 inset). Every
 * function is pure: no resident, no game. */

#define PK_LAY_CARD_H   72.0f     /* a hand card, whatever its width           */
#define PK_LAY_ROW_H    80.0f     /* one hand row's box                        */
#define PK_LAY_FACE_W   40.0f     /* an overlapped card keeps a full face (U8) */
#define PK_LAY_THIN_W   40.0f     /* a flat card narrower than this goes thin  */

/* How a hand of n lays out: one or two flat rows, overlapped rows, or rows
 * that scroll (O4; the drawer keeps one row, U7). */
enum { PK_LAY_FLAT = 1, PK_LAY_OVERLAP = 2, PK_LAY_SCROLL = 3 };

/* How collapsed the drawer is, from the extension view's height: 0 expanded
 * (440 and up) to 1 compact (340 and below), foolish's anchors. */
float pk_lay_collapse(float view_h);
/* One hand row, or two? The drawer (collapse 1/2 and over) keeps one. */
int   pk_lay_max_rows(float view_h);

/* A hand of `n` cards in `width` (the hand's width inside its 8pt side
 * padding), at most `max_rows` rows. Returns the PK_LAY_* mode and writes the
 * card width, the step from one card's left edge to the next's, the rows, how
 * many cards the TOP row takes (the smaller half), the content width (wider
 * than `width` only when it scrolls) and the box height. */
int   pk_lay_hand(int n, float width, int max_rows, float *card_w, float *step, int *rows,
                  int *top_n, float *content_w, float *box_h);
/* Card i's top-left in the hand's box (content coordinates when it scrolls).
 * 0, or -1 for an i out of 0..n-1. */
int   pk_lay_hand_slot(int n, float width, int max_rows, int i, float *x, float *y);

/* Where seat `seat` sits on the ring (its badge's centre), with my seat at
 * the bottom (`me` = -1: a spectator sees seat 0 there). */
void  pk_lay_seat(int seat, int me, int n, float board_w, float board_h, float collapse,
                  float *x, float *y);

/* The step between two backs of a `backs`-card fan: foolish's 10pt,
 * compressing (never under 3pt) to keep the fan within 96pt (U6). */
float pk_lay_fan_step(int backs);
#define PK_LAY_FAN_CARD_W 28.0f
#define PK_LAY_FAN_CARD_H 40.0f

/* The deck's drawn layers for a count: one per card to 6, 7 to 11, then 8. */
int   pk_lay_deck_layers(int deck_n);

/* The pile's centre, lifted clear of the pill row in the drawer (U2), and the
 * deck's top-left beside it (U3). */
void  pk_lay_pile(float board_w, float board_h, float collapse, float *cx, float *cy);
void  pk_lay_deck(float board_w, float board_h, float collapse, float *x, float *y);
#define PK_LAY_PILE_W 82.0f
#define PK_LAY_PILE_H 115.0f
#define PK_LAY_DECK_W 50.0f
#define PK_LAY_DECK_H 70.0f

/* The pill row (U9): what stands in the TRAILING slot and in the one to its
 * left. Draw holds the trailing slot whenever it is legal; beside it, Play
 * (a card selected on my turn), else Pass, else Undo; with no Draw the first
 * of those takes the trailing slot. */
enum { PK_PILL_NONE = 0, PK_PILL_DRAW, PK_PILL_PLAY, PK_PILL_PASS, PK_PILL_UNDO };
void  pk_lay_pills(int can_draw, int my_turn, int selected, int can_pass, int can_undo,
                   int *trailing, int *leading);

/* The suit picker (U14): tile `tile` (0 circles north, 1 triangles east, 2
 * squares south, 3 diamonds west, 4 the x) centred about the pile's centre. */
void  pk_lay_picker(int tile, float cx, float cy, float *x, float *y);
#define PK_LAY_PICKER_TILE 60.0f
#define PK_LAY_PICKER_X    30.0f

/* ---- the motion (pk_beats.h) --------------------------------------------------------
 *
 * Every animation is the kernel's timeline: a PkBeats of the plan events laid
 * out on a clock, which the host samples each frame (pk_api_beats_frame for
 * the board, pk_api_beat_sample for one beat's transform) and tweens between
 * the anchors its views report. The host holds no duration and no order.
 *
 * ONE PLAN IS CURRENT: each call below replaces it (a superseding arrival
 * clears the one playing; nothing is reverted). Each returns the new plan
 * (PkBeats), or NULL for a range the kernel cannot lay out, and the host then
 * shows the settled view with no motion. */

/* Channels C, D and E: the events of bubbles (from, to], from the board as it
 * stood at the end of bubble `from`. PK_BEATS_OPEN or PK_BEATS_ARRIVAL. */
const void *pk_api_beats(int viewer, int from, int to, int mode);
/* Channel A: what my newest tap did, against the draft as the previous call
 * left it. flags: PK_BFL_*; with PK_BFL_PICKED the tapped tile's ring and the
 * tiles' collapse go first, and the suit is (flags >> 8) & 3. */
const void *pk_api_beats_stage(int flags);
/* Channel B, after pk_api_commit: what staging held back. */
const void *pk_api_beats_send(void);
/* A motion no event describes (PK_HM_*), from the resident as it is now. */
const void *pk_api_beats_host(int what, int a, int b);
/* A lost race (4.8): my staged `card` flies home to `pos` as a retraction
 * ghost, then the winning chain (from, to] plays forward. */
const void *pk_api_beats_conflict(int card, int pos, int from, int to);
/* Remember the draft as it is now, for the next pk_api_beats_stage (after a
 * change that moves nothing: an un-say, a cancel). */
void        pk_api_beats_mark(void);
/* Which build is current (PkBeats.serial of the newest). */
uint32_t    pk_api_beats_serial(void);
/* The current plan's board at `now_ms` (PkBeatFrame), and one beat of it
 * (PkBeatSample); NULL for an index off the plan. */
const void *pk_api_beats_frame(uint32_t now_ms);
const void *pk_api_beat_sample(int i, int part, uint32_t now_ms);

/* ---- two messages -------------------------------------------------------------- */

/* Which to show: <0 mine (the device's staged draft), >0 the tapped one, 0 the
 * same. An unreadable one always loses. */
int  pk_api_prefer(const char *mine, const char *tapped);
int  pk_api_same_game(const char *a, const char *b);
/* How many bubbles two chains of one game share (what to take back after a
 * lost race). -1 if either does not read. */
int  pk_api_common(const char *a, const char *b);

#endif
