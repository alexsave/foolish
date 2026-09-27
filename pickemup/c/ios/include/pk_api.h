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
#define PK_API_W_COUNT        16
int  pk_api_words(int what, int arg, char *out, int cap);

/* ---- two messages -------------------------------------------------------------- */

/* Which to show: <0 mine (the device's staged draft), >0 the tapped one, 0 the
 * same. An unreadable one always loses. */
int  pk_api_prefer(const char *mine, const char *tapped);
int  pk_api_same_game(const char *a, const char *b);
/* How many bubbles two chains of one game share (what to take back after a
 * lost race). -1 if either does not read. */
int  pk_api_common(const char *a, const char *b);

#endif
