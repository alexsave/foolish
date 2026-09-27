/* Tallybones - the Swift-visible face of the kernel. Flat entry points, and
 * no byte layout on the far side (pickemup/c/ios/include/pk_api.h's shape).
 *
 * WHERE A STRUCT CROSSES, IT CROSSES AS A POINTER Swift reads through
 * generated code: tb_api_table, tb_api_view, tb_api_plan*, tb_api_beats* and
 * tb_api_beats_frame / tb_api_beat_sample return `const void *` into the
 * kernel's own storage, and the readers are shared/tools/structgen's,
 * generated from the kernel's headers (tallybones/c/ios/layout.args, `make -C
 * tallybones/c structgen`). The pointer is valid until the next call into
 * this file; a host copies what it read (the generated readers return values)
 * and holds no pointer. The kernel's constants (TB_EV_*, TB_C_*, TB_LOBBY_*,
 * TB_PHASE_*, TB_BY_*, TB_BK_*, the TB_E* errors) come with the readers.
 *
 * ONE RESIDENT MESSAGE, static rather than allocated, and AT MOST ONE STAGED
 * MOVE on it: a turn is one to three bubbles and every bubble is one move
 * (DECISIONS.md T3, T11), so staging a keep or a score REPLACES whatever was
 * staged, and cancel drops it. tb_api_read ADOPTS (a decode is an adoption),
 * so never read, await and then stage.
 *
 * THE DICE (T11). tb_api_view shows the resident game with my staged move
 * applied AS A DRAFT: a staged KEEP's rerolling dice read 0, unknown, and a
 * staged SCORE's next roll reads 0. No entry point here returns dice values
 * for a staged move; the reroll exists once the bubble is SENT, which the
 * host reports with tb_api_mark_sent from didStartSending: the staged bubble
 * becomes the resident, the resident replay derives its roll, and the view
 * and tb_api_beats_now show it. A receiver gets the same dice by opening the
 * bubble (tb_api_adopt). tb_api_read refuses my own staged, unsent bubble
 * (TB_ESTAGED) so the host cannot adopt it by accident.
 *
 * THIS DEVICE'S IDENTITY is whatever bytes the host says it is (Messages'
 * localParticipantIdentifier), hashed with each game's seed into a seat tag
 * here, never sent. THE NICKNAME is the App Group's. */
#ifndef TB_API_H
#define TB_API_H

#include <stdint.h>

/* The layout the generated readers must have been generated for: equal to
 * their SG_LAYOUT_HASH, or the two are not a pair. 0 for a build that was not
 * stamped (ios-smoke), which matches no module. */
uint32_t tb_api_layout_hash(void);

/* ---- who I am ------------------------------------------------------------ */

void tb_api_me(const uint8_t *id, int n);
void tb_api_nickname(const uint8_t *name, int n);
/* TB_NAME_OK 0, EMPTY 1, TOO_LONG 2 (16 characters, 48 bytes), BAD 3. */
int  tb_api_name_verdict(const uint8_t *name, int n);

/* THIS DEVICE'S SEAT RECORDS: fixed-layout bytes the host keeps and hands
 * back unread (17 bytes a game, the newest 256). Load once, save when dirty. */
#define TB_API_REC_BYTES 4352
void tb_api_seats_load(const uint8_t *bytes, int n);
int  tb_api_seats_dirty(void);
int  tb_api_seats_save(uint8_t *out, int cap);    /* length, or -1; clears dirty */

/* ---- the resident message ------------------------------------------------ */

/* The longest link any game writes, with its NUL (asserted in tb_api.c). */
#define TB_API_TEXT_MAX 1280

/* A new lobby, me in seat 0 under my nickname, from 32 bytes of the host's
 * secure random (the game seed, T6). 0, or a negative TB_E*. */
int  tb_api_new(const uint8_t seed[32], int dm);

/* ADOPT the message in `text` (a whole URL string is fine): the resident
 * replay, every roll derived. Drops my staged move. 0, or a negative TB_E*
 * and nothing changes (TB_ESTAGED: it is my own staged, unsent bubble). */
int  tb_api_read(const char *text);
/* Whether `text` would read, WITHOUT adopting it. */
int  tb_api_check(const char *text);
/* THE SENDER FACT: `text` was sent by this device (i_sent 1) or not (0), in
 * a chat with exactly one other person (is_dm). NULL clears it. */
void tb_api_sender(const char *text, int is_dm, int i_sent);

/* The link for MSMessage.url: the resident message, with my staged move as
 * its newest bubble when there is one. Length, or a negative TB_E*. */
int  tb_api_text(char *out, int cap);
/* THE SEND ECHO (didStartSending): my staged bubble was sent, so it becomes
 * the resident message and its roll is derived. Lays out its motion
 * (tb_api_beats_now). 1, or 0 with nothing staged. */
int  tb_api_mark_sent(void);

/* ADOPT `text` AND LAY OUT WHAT IT BRINGS: of the same game and further on,
 * the bubbles (on screen, new tip]; opened cold, the newest bubble (the
 * start for a start bubble); the same bubble again or a lobby, no motion.
 * `arrival`: it landed while the board was up (a 16ms lead), else it was
 * opened (100ms). 0, or a negative TB_E* and nothing changed. */
int  tb_api_adopt(const char *text, int arrival);

/* ---- the lobby: each changes the resident at once (Pick 'Em Up's) ---------
 *
 * 0 (join and join-start: my new seat) or a negative TB_E*. In a live game
 * tb_api_stage_leave STAGES a LEAVE bubble instead (T5): 1 staged, 0 refused. */
int  tb_api_stage_join(void);
int  tb_api_stage_leave(void);
int  tb_api_stage_start(void);
int  tb_api_stage_join_start(void);

/* ---- staging a move (my seat, the resident game) ---------------------------
 *
 * 1 if it is staged, 0 if the rules refused it (and the staged move, if any,
 * stays). Staging replaces the staged move. */
int  tb_api_stage_keep(int mask);        /* bit i: keep die i; 0..30 (31 is not a reroll) */
int  tb_api_stage_score(int cat);        /* TB_C_* */
int  tb_api_cancel(void);                /* drop the staged move: 1, or 0 with none */
int  tb_api_can_keep(int mask);          /* on the resident, as a staged move would be */
int  tb_api_can_score(int cat);
/* What `cat` scores for the turn seat with the dice as the view shows them:
 * the scorecard's preview. -1 while any die is unknown (a staged keep) or
 * the category is taken. */
int  tb_api_score_if(int cat);

/* ---- reading it ------------------------------------------------------------ */

const void *tb_api_table(void);                  /* TbApiTable                          */
const void *tb_api_view(void);                   /* TbView: the resident, my move staged */
/* Events of bubbles (from, to]; from = -1 includes the start. NULL for a
 * range off the game. */
const void *tb_api_plan(int from, int to);       /* TbApiEvents                         */
const void *tb_api_plan_draft(void);             /* TbApiEvents: my staged move         */
const void *tb_api_plan_lobby(const char *before);   /* TbApiEvents: roster changes     */

/* THE FINISHED TABLE'S ORDER, and the live one's: seats by total, highest
 * first, ties in seat order, seats that left last. Writes n, returns n. */
int  tb_api_ranks(uint8_t out[8]);

/* ---- the words --------------------------------------------------------------
 *
 * Every line is the kernel's (tb_say.h), in the table's language; the host
 * never composes one. Each writes a NUL-terminated line and returns its
 * length, "" (0) being a real answer; -1 for a buffer too small or an
 * argument out of range. */
int  tb_api_string(int key, char *out, int cap);    /* one table entry by TB_K_* key */

#define TB_API_W_CAPTION         0   /* arg: bubble (0 the start). One line, <= 36 columns */
#define TB_API_W_SUMMARY         1   /* arg: bubble. Every clause: MSMessage.summaryText   */
#define TB_API_W_STAGED_CAPTION  2   /* the bubble tb_api_text writes now                   */
#define TB_API_W_STAGED_SUMMARY  3
#define TB_API_W_HEADLINE        4   /* for me: "Your roll", "Waiting on Bo"                */
#define TB_API_W_SUBLINE         5   /* "Two rerolls left", "Send it to see the new dice"   */
#define TB_API_W_CAT             6   /* arg: TB_C_*. "Full House"                          */
#define TB_API_W_SEAT            7   /* arg: seat. The name, or "Player 3"                */
#define TB_API_W_INVITE          8   /* arg: seat. The lobby's captions; a leave is       */
#define TB_API_W_JOINED          9   /* captioned BEFORE tb_api_stage_leave, while the     */
#define TB_API_W_LEFT           10   /* leaver's row is still there                        */
#define TB_API_W_LOBBY_ROW      11   /* arg: seat. "2. Bo", or "2. Bo (You)" for mine      */
#define TB_API_W_PUBLIC_ROW     12   /* arg: seat. "2. Bo", for the bubble's picture       */
#define TB_API_W_RANK_ROW       13   /* arg: place 0..n-1 of tb_api_ranks. "1. Cy, 241"   */
#define TB_API_W_ERROR          14   /* arg: a negative TB_E*. Why a link did not read    */
#define TB_API_W_RULES_TITLE    15
#define TB_API_W_RULE           16   /* arg: 0..TB_RULES_N-1                               */
#define TB_API_W_SPOKEN_DIE     17   /* arg: die 0..4. VoiceOver: "Die 2, five, kept"       */
#define TB_API_W_ROW_LABEL      18   /* arg: TB_ROW_*. The scorecard's left column        */
#define TB_API_W_COUNT          19
int  tb_api_words(int what, int arg, char *out, int cap);

/* ---- the motion (tb_beats.h) --------------------------------------------------
 *
 * Every animation is the kernel's timeline: a TbBeats of plan events laid out
 * on a clock, which the host samples each frame (tb_api_beats_frame for the
 * board, tb_api_beat_sample for one beat's transform). The host holds no
 * duration and no order. ONE PLAN IS CURRENT: each build replaces it. */

/* The events of bubbles (from, to] from the board at the end of `from`.
 * TB_BEATS_OPEN or TB_BEATS_ARRIVAL. NULL: show the settled view. */
const void *tb_api_beats(int from, int to, int mode);
/* The current plan (TbBeats), or NULL when the newest build laid nothing out. */
const void *tb_api_beats_now(void);
uint32_t    tb_api_beats_serial(void);
/* The current plan's board at `now_ms` (TbBeatFrame), and one beat of it
 * (TbBeatSample; `part` is a die 0..4 for a settle); NULL for an index off
 * the plan. */
const void *tb_api_beats_frame(uint32_t now_ms);
const void *tb_api_beat_sample(int i, int part, uint32_t now_ms);

/* ---- two messages ------------------------------------------------------------- */

/* Which to show: <0 mine, >0 the tapped one, 0 the same. An unreadable one
 * always loses. */
int  tb_api_prefer(const char *mine, const char *tapped);
int  tb_api_same_game(const char *a, const char *b);
/* How many bubbles two chains of one game share. -1 if either does not read. */
int  tb_api_common(const char *a, const char *b);

/* ---- the layout (tb_lay.c) --------------------------------------------------------
 *
 * Points, in the board's own coordinates (the extension's view less the
 * insets). Every function is pure: no resident, no game. */
#define TB_LAY_INSET_L    8.0f
#define TB_LAY_INSET_R    8.0f
#define TB_LAY_INSET_T   14.0f
#define TB_LAY_INSET_B    4.0f
#define TB_LAY_DIE_MAX   64.0f    /* a die's side, before the tray shrinks it   */
#define TB_LAY_DIE_MIN   40.0f
#define TB_LAY_DIE_GAP   10.0f    /* between two dice                           */
#define TB_LAY_KEEP_LIFT 12.0f    /* a die marked to keep sits this much lower  */
#define TB_LAY_PIP       0.18f    /* a pip's diameter as a fraction of the side */
#define TB_LAY_CORNER    0.18f    /* the rounded square's corner radius, too    */
#define TB_LAY_ROW_H     28.0f    /* one scorecard row                          */
#define TB_LAY_GAP_H     10.0f    /* between the card's two halves              */

/* How collapsed the drawer is, from the extension view's height: 0 expanded
 * (440 and up) to 1 compact (340 and below). */
float tb_lay_collapse(float view_h);

/* The dice tray: its rect (centred, above the scorecard in the expanded
 * view, alone in the drawer), and die i's top-left and side inside the
 * board. 0, or -1 for an i off 0..4. */
void  tb_lay_tray(float board_w, float board_h, float collapse, float *x, float *y, float *w, float *h);
int   tb_lay_die(int i, float board_w, float board_h, float collapse, int kept, float *x, float *y,
                 float *side);
/* Pip k (0..n-1) of face `face` (1..6) as a fraction 0..1 of the die's side:
 * the classic layouts. The pip count, or -1 for a face off 1..6. */
int   tb_lay_pip(int face, int k, float *fx, float *fy);

/* THE SCORECARD'S ROWS, top to bottom: the six numbers, the numbers half's
 * subtotal and its bonus, the seven combinations, the total. */
enum {
    TB_ROW_ONES = 0, TB_ROW_SIXES = 5, TB_ROW_UPPER = 6, TB_ROW_BONUS = 7,
    TB_ROW_THREE_ALIKE = 8, TB_ROW_ANY = 14, TB_ROW_TOTAL = 15, TB_ROW_N = 16
};
/* The category row `row` scores (TB_C_*), or -1 for the subtotal, the bonus
 * and the total. */
int   tb_lay_row_cat(int row);
/* Row `row`'s top in the card, and the card's whole height. -1 off the card. */
int   tb_lay_row_y(int row, float *y);
float tb_lay_card_h(void);

#endif
