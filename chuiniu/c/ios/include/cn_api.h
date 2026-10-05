/* Chui Niu - the Swift-visible face of the kernel (module CChuiniu). Flat
 * entry points, and no byte layout on the far side. THIS IS THE ONLY HEADER
 * THE HOST READS; every entry point is documented here.
 *
 * WHERE A STRUCT CROSSES, IT CROSSES AS A POINTER Swift reads through
 * generated code: cn_api_table, cn_api_view, cn_api_plan*, cn_api_beats* and
 * cn_api_beats_frame return `const void *` into the kernel's own storage, and
 * the readers are shared/tools/structgen's, generated from the kernel's
 * headers into chuiniu/ios/Generated/ChuiniuKernel.swift by
 * `make -C chuiniu/c ios-lib` (readCnApiTable, readCnView, readCnApiEvents,
 * readCnBeats, readCnBeatFrame). The pointer is valid until the next call
 * into this file; a host copies what it read (the readers return values) and
 * holds no pointer. The kernel's constants (CN_PHASE_*, CN_PH_*, CN_EV_*,
 * CN_BK_*, CN_BS_*, CN_LOBBY_*, CN_BY_*, CN_E*, CN_API_STAGED_*, CN_SEAT_NONE,
 * CN_MAX_SEATS, CN_START_DICE, ...) come with the readers.
 *
 * ONE RESIDENT MESSAGE, static rather than allocated: the roster and the game
 * together. IT IS ONE SLOT: never read it and stage on it across an await.
 * cn_api_read ADOPTS (a decode is an adoption; cn_api_check is the one read
 * that leaves the resident alone), and an adoption drops any staged move.
 *
 * ONE MOVE PER BUBBLE. On my turn I stage a raise (cn_api_raise) or a call
 * (cn_api_call); staging again replaces it, cn_api_cancel drops it. The
 * staged move is written into the link by cn_api_text and becomes part of
 * the resident only at cn_api_commit (the host calls it when Messages starts
 * sending). A STAGED CALL REVEALS NOTHING (DECISIONS K8): the view, the
 * staged plan, the staged beats and the staged caption show only "I call",
 * and the cups lift once it is committed.
 *
 * THE DICE ARE NEVER RANDOM HERE: every round's dice come from the game's
 * seed and the moves already sent (DECISIONS K2, K3), so cancelling and
 * staging again can never re-roll them. The only randomness the host
 * supplies is the 32-byte seed of a new game.
 *
 * THIS DEVICE'S IDENTITY is whatever bytes the host says it is (Messages'
 * localParticipantIdentifier). It is hashed with each game's seed into a
 * seat tag here and never leaves the kernel. THE NICKNAME is the App
 * Group's, typed once; it is this device's name on every roster it joins. */
#ifndef CN_API_H
#define CN_API_H

#include <stddef.h>
#include <stdint.h>

/* The layout the generated readers must have been generated for: equal to
 * their SG_LAYOUT_HASH, or the two are not a pair (refuse to start). 0 for a
 * build that was not stamped (ios-smoke), which matches no module. */
uint32_t cn_api_layout_hash(void);

/* ---- who I am ------------------------------------------------------------ */

/* This device's identity bytes (at most 64 are used). */
void cn_api_me(const uint8_t *id, int n);
/* My nickname, UTF-8. A name that fails cn_api_name_verdict clears it. */
void cn_api_nickname(const uint8_t *name, int n);
/* CN_NAME_OK 0, EMPTY 1, TOO_LONG 2 (16 characters, 48 bytes), BAD 3. */
int  cn_api_name_verdict(const uint8_t *name, int n);

/* THIS DEVICE'S SEAT RECORDS: fixed-layout bytes the host keeps in the App
 * Group and hands back unread. Load once at launch, save whenever dirty. At
 * most CN_API_REC_BYTES (17 bytes a game, the newest 256 games). */
#define CN_API_REC_BYTES 4352
void cn_api_seats_load(const uint8_t *bytes, int n);
int  cn_api_seats_dirty(void);
int  cn_api_seats_save(uint8_t *out, int cap);    /* length, or -1; clears dirty */

/* ---- the resident message ------------------------------------------------ */

/* The longest link any game writes, with its NUL: size the buffer for
 * cn_api_text with it. */
#define CN_API_TEXT_MAX 4392

/* A new lobby, me in seat 0 under my nickname, from 32 bytes of the host's
 * secure random (SecRandomCopyBytes). `dm`: a chat with exactly one other
 * person (capacity 2, else 6). 0, or a negative CN_E*. */
int  cn_api_new(const uint8_t seed[32], int dm);

/* ADOPT the message in `text` (MSMessage.url's absoluteString; a whole URL
 * is fine). 0, or a negative CN_E* and nothing changes. Drops any staged
 * move. Any sender fact given for exactly this text (cn_api_sender) is used
 * to seat me. */
int  cn_api_read(const char *text);
/* Whether `text` would read, WITHOUT adopting it. 0 or a negative CN_E*. */
int  cn_api_check(const char *text);
/* THE SENDER FACT: `text` (the tapped bubble) was sent by this device
 * (i_sent 1) or not (0), in a chat with exactly one other person (is_dm).
 * About that one message only; NULL clears it. */
void cn_api_sender(const char *text, int is_dm, int i_sent);

/* ADOPT `text` (cn_api_read) AND LAY OUT WHAT IT BRINGS as the current beats
 * (cn_api_beats_now): of the same game and further on, every move after the
 * ones the old resident had; opened cold, the newest move (the start for a
 * start bubble); a lobby or the same bubble again, no motion. 0, or a
 * negative CN_E* and nothing changed. */
int  cn_api_adopt(const char *text);

/* The resident message as the link for MSMessage.url, with my staged move
 * in it if there is one (the resident itself keeps it staged). Length, or a
 * negative CN_E*. */
int  cn_api_text(char *out, int cap);
/* The staged move was sent: it joins the resident. 1, or 0 with none. */
int  cn_api_commit(void);

/* ---- the lobby ------------------------------------------------------------- */

/* Each returns 0 (join and join-start: my new seat) or a negative CN_E*.
 * After a success, cn_api_text is the bubble to send. */
int  cn_api_join(void);
int  cn_api_leave(void);
int  cn_api_start(void);
int  cn_api_join_start(void);

/* ---- my move (my seat, on my turn) --------------------------------------------
 *
 * 1 if staged, 0 if the rules refused it (not my turn, not a legal move).
 * Staging replaces any staged move. */
int  cn_api_raise(int quantity, int face);   /* face 2..6; quantity 1..dice on the table */
int  cn_api_call(void);                      /* never on a round's opening bid            */
int  cn_api_cancel(void);                    /* drop the staged move: 1, or 0 with none   */
/* Would cn_api_raise(quantity, face) be legal now? For dimming a picker. */
int  cn_api_can_raise(int quantity, int face);

/* ---- reading it ---------------------------------------------------------------- */

#define CN_API_ME        (-3)            /* the viewer is my resolved seat          */
#define CN_API_SPECTATOR (-1)
#define CN_API_ALL       (-2)            /* tests only: every die in CnView.all     */

/* CnApiTable: phase, roster, my seat, the lobby's offer, my staged move. */
const void *cn_api_table(void);
/* CnView of the COMMITTED game for `viewer`: my own dice (sorted), every
 * seat's count, the standing bid, the menu when it is my turn (can_raise,
 * can_call, the lowest raise min_q/min_f, max_q, and min_q_face[f], the
 * least legal quantity for each face 2..6 or 0: Raise is legal at (q, f)
 * exactly when min_q_face[f] != 0 and min_q_face[f] <= q <= max_q, so the
 * picker never ranks two bids), and the newest call's reveal (shown_n,
 * shown, shown_counts[i] = 1 for a die that counts toward the bid; revealed
 * while it is the news). A staged move never changes it. NULL before a game
 * has started. */
const void *cn_api_view(int viewer);
/* CnApiEvents of committed moves (from, to]; from = -1 includes the start.
 * NULL for a range that does not fit or a game that has not started. */
const void *cn_api_plan(int from, int to);
/* CnApiEvents of my staged move: a BID, or for a call the CALL alone (K8). */
const void *cn_api_plan_staged(void);

/* ---- the motion (cn_beats.h) ----------------------------------------------------
 *
 * Every animation is the kernel's timeline: a CnBeats of the plan events laid
 * out on a clock. ONE PLAN IS CURRENT: each build below replaces it. The
 * host samples cn_api_beats_frame(now_ms) every display tick, from 0 at the
 * moment it started the plan, and draws the CnBeatFrame (cups up, shaking,
 * the face being counted and how many are lit, the bid on the board, every
 * seat's count, each beat's state and eased progress) until `done`, then the
 * settled view. It holds no duration and no order of its own. Each returns
 * the new CnBeats, or NULL (then show the settled view, no motion). */

/* Committed moves (from, to], from the board as it stood after `from`. */
const void *cn_api_beats(int from, int to);
/* My staged move, from the committed board (a call: its stamp only). */
const void *cn_api_beats_staged(void);
/* The current plan (CnBeats), or NULL when the newest build laid nothing out. */
const void *cn_api_beats_now(void);
/* The current plan's board at `now_ms` (CnBeatFrame); NULL with no plan. */
const void *cn_api_beats_frame(uint32_t now_ms);

/* ---- the words ------------------------------------------------------------------
 *
 * Every line is the kernel's (cn_say.h); the host never composes one. Each
 * writes a NUL-terminated line and returns its length, "" (0) being a real
 * answer; -1 for a buffer too small, an argument out of range or nothing
 * resident. */
int  cn_api_string(int key, char *out, int cap);       /* one table entry by CN_K_* index */

#define CN_API_W_CAPTION         0   /* arg: move (0 the start). "Alex bid four 3s"        */
#define CN_API_W_STAGED_CAPTION  1   /* the caption of the bubble cn_api_text writes now:
                                        my staged move, else the newest move; in a lobby
                                        the invite, joined or left line of its sender     */
#define CN_API_W_HEADLINE        2   /* for me: "Your turn: raise or call", "Bo's turn",
                                        "Send to bid four 3s" while staged, "You win"      */
#define CN_API_W_SUBLINE         3   /* "Bid to beat: four 3s by Alex", "No bid yet"       */
#define CN_API_W_OUTCOME         4   /* the newest call, once sent: "Bo calls. Four 3s was
                                        true, Bo loses a die. Bo is out. Alex wins"; ""
                                        before the first call                              */
#define CN_API_W_SEAT            5   /* arg: seat. The name, or "Player 3"                 */
#define CN_API_W_BID             6   /* arg: quantity * 8 + face. "four 3s"                */
#define CN_API_W_DICE_N          7   /* arg: a count. "1 die", "4 dice"                    */
#define CN_API_W_TABLE           8   /* "14 dice on the table"                             */
#define CN_API_W_REVEAL_COUNT    9   /* the newest call's count: "There were five"         */
#define CN_API_W_LOBBY_ROW      10   /* arg: seat. "2. Bo", or "2. Bo (You)" for mine      */
#define CN_API_W_INVITE         11   /* arg: seat. "Alex wants a game of Chui Niu. Tap to join" */
#define CN_API_W_JOINED         12   /* arg: seat. "Bo joined"                             */
#define CN_API_W_LEFT           13   /* arg: seat. "Bo left" (before cn_api_leave)          */
#define CN_API_W_ERROR          14   /* arg: a negative CN_E*. Why a link did not read     */
#define CN_API_W_RULES_TITLE    15
#define CN_API_W_RULE           16   /* arg: 0..5                                          */
#define CN_API_W_COUNT          17
int  cn_api_words(int what, int arg, char *out, int cap);

/* ---- two messages -------------------------------------------------------------- */

/* Which to show: <0 mine (the device's own newest), >0 the tapped one, 0 the
 * same. An unreadable one always loses. */
int  cn_api_prefer(const char *mine, const char *tapped);
int  cn_api_same_game(const char *a, const char *b);
/* How many moves two chains of one game share. -1 if either does not read. */
int  cn_api_common(const char *a, const char *b);

/* ---- the stage: the table's pixels (cn_stage.h) ---------------------------------
 *
 * THE KERNEL DRAWS THE TABLE. The host hands over the texture pack's bytes
 * (cn_tex.pack, which must outlive the stage), begins a screen, and, only
 * when it is about to draw, one block of memory (the renderer's arena:
 * CN_STAGE_ARENA, 48 MB, cn_api_stage_attach); then it asks for frames.
 * A begin takes no arena, so an extension can lay a screen out and free
 * nothing it never took. Every place on it (the cups, the names, the plate, the shelf, my
 * cup's tap target, the camera's turn) comes back in a CnStageHud read through
 * the generated reader (readCnStageHud), and every frame's size in a
 * CnStageShot (readCnStageShot). ONE STAGE A PROCESS, static here.
 *
 * A FRAME ON SEVERAL THREADS: cn_api_stage_prepare, then for each pass 0 ..
 * CN_STAGE_PASSES - 1 in order, cn_api_stage_band(pass, i, CN_STAGE_BANDS) for
 * every i at once (DispatchQueue.concurrentPerform), then cn_api_stage_pixels:
 * the shot's w by h RGBA, straight alpha (or Core Animation's own form after
 * cn_api_stage_output(CN_API_STAGE_CA)), valid until the next prepare.
 *
 * A FRAME OFF THE HOST'S MAIN THREAD: the lift is the resident plan's, which
 * only the thread that adopts may read, so the host samples it with the clock
 * (cn_api_stage_lift) and draws with cn_api_stage_prepare_at anywhere else; no
 * other stage call (begin, attach, purge, another frame) may run meanwhile. The
 * picture goes at the shot's canvas (flat points, turned with the planks by the
 * HUD's ca); the plate and the shelf stay flat.
 *
 * THE CLOCK is the one cn_api_beats_frame is sampled on: a reveal's cups lift
 * with the current plan's LIFT beat, and a table begun with `roll` throws from
 * the plan's SHAKE beat (from 0 when the plan has none). Stage nothing before
 * the HUD's rest_ms (my dice at rest). */

/* The pack, no memory. 0, or a negative CN_TEX_E*. */
int  cn_api_stage_init(const uint8_t *pack, size_t pack_len);
/* The arena, before the first frame and again after a purge. 0, or
 * CN_STAGE_E_ARENA (none, too small, or no init yet). */
int  cn_api_stage_attach(void *arena, size_t bytes);
/* A memory warning: the stage lets go of the arena (free it after this);
 * frames draw nothing until cn_api_stage_attach gives one back, and then the
 * same bytes as before. */
void cn_api_stage_purge(void);
/* Begin a screen of the resident game for me: CN_STAGE_TABLE (the committed
 * round; `roll` 1 throws it), CN_STAGE_REVEAL (the newest call's dice where
 * that round's throw left them) or CN_STAGE_BUBBLE (300 by 195). The drawer is
 * w by h points, the device `scale` pixels a point (clamped: 1.5 while a throw
 * moves, 2 still). CnStageHud, or NULL (no game, no seat, no call to reveal, a
 * finished game's table). */
const void *cn_api_stage_begin(int kind, float w, float h, float scale, int roll);
/* The frame at now_ms with my cup tipped `peek` (0 shut .. 1 the HUD's
 * peek_target; ease it with cn_api_peek_ease over CN_PEEK_MS). 1, or 0 when
 * nothing can be drawn. */
int  cn_api_stage_prepare(uint32_t now_ms, float peek);
void cn_api_stage_band(int pass, int band, int nbands);
const uint8_t *cn_api_stage_pixels(void);
/* prepare, every band on this thread, the pixels */
const uint8_t *cn_api_stage_frame(uint32_t now_ms, float peek);
/* The reveal's cups' lift at now_ms (the current plan's LIFT beat; 1 with
 * none): what cn_api_stage_prepare reads from the resident. */
float cn_api_stage_lift(uint32_t now_ms);
/* cn_api_stage_prepare with the lift given: reads nothing of the resident.
 * cn_api_stage_prepare(t, p) is cn_api_stage_prepare_at(t, p, cn_api_stage_lift(t)). */
int  cn_api_stage_prepare_at(uint32_t now_ms, float peek, float lift);
/* The pixels' form from the next frame on: CN_API_STAGE_RGBA (R G B A,
 * straight alpha: the default) or CN_API_STAGE_CA (B G R A premultiplied,
 * alpha first and 32 bits little-endian: Core Animation's own, which it draws
 * as it is; any other form it redraws into an image of its own first, on the
 * main thread, every frame). */
#define CN_API_STAGE_RGBA 0
#define CN_API_STAGE_CA   2
void cn_api_stage_output(int form);
/* CnStageShot of the last frame. */
const void *cn_api_stage_shot(void);
/* Everything at rest at now_ms (every throw, the SHAKE beat)? */
int  cn_api_stage_done(uint32_t now_ms);
/* The peek's tween: the fraction of the tip at t (0..1 of CN_PEEK_MS). */
float cn_api_peek_ease(float t);

#endif
