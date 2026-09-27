/* msg_stage.h - getting a staged bubble into Messages' input field, decided
 * once for every iMessage product (INSERT_GATING.md beside this file).
 *
 * Three decisions, each one a device log's worth of evidence:
 *   ms_drawer_up       may an insert go now - is the extension view a drawer
 *                      yet, or still the window-sized first appearance?
 *   ms_insert_silence  what an insert that has gone unanswered means;
 *   ms_receive         whether a bubble handed to didReceive is an arrival or
 *                      this device's own bubble coming back (an echo).
 *
 * Header-only and static inline, so no build system has to compile a new
 * source: Swift reads it through the CMsgStage module (module.modulemap
 * beside this file, on SWIFT_INCLUDE_PATHS), C by including it by a relative
 * path. Product-free, no strings. */
#ifndef MSG_STAGE_H
#define MSG_STAGE_H

/* ------------------------------------------------------ is the drawer up */

/* A compact drawer is always well short of its window (309-343pt in 667-932). */
#define MS_DRAWER_MARGIN       40

/* IS THE DRAWER UP - may an insert go now? `window_h` is the window's height,
 * `view_h` the extension view's (at viewDidAppear or a layout pass),
 * `expanded` whether Messages says the presentation style is expanded.
 *
 * On a phone the + drawer's FIRST appearance is the whole window (430x932 in
 * a 932 window, device log 2026-09-23), a second before the compact drawer is
 * up, and an insert issued then is dropped without an answer. That appearance
 * never counts: a view as tall as its window is not a drawer.
 *
 * An EXPANDED drawer is a drawer at any height short of the window: on a
 * 667pt phone a tapped bubble opens it at 647 - the status bar is all it
 * leaves - which a "40 points short" test read as window-sized. A compact
 * drawer has to be well short of the window (MS_DRAWER_MARGIN). */
static inline int ms_drawer_up(float window_h, float view_h, int expanded)
{
    if (!(view_h > 0) || !(window_h > 0)) return 0;
    if (view_h >= window_h - 0.5f) return 0;
    if (expanded) return 1;
    return view_h < window_h - MS_DRAWER_MARGIN;
}

/* ---------------------------------------------------- a silent insert */

/* A REFUSED INSERT NEVER ANSWERS. ChatKit drops an insert that arrives before
 * the host counts the drawer as presenting and calls no completion at all -
 * not with an error, not ever (INSERT_GATING.md). So silence is the refusal:
 * an insert unanswered after MS_INSERT_SILENCE_MS is asked again, up to
 * MS_INSERT_ATTEMPTS in all (about five seconds), and then the human is
 * handed a door that inserts on a tap - by which time the drawer is
 * presenting and the gate passes. An accepted try answers at once, so a
 * refused try leaves nothing behind and a retry cannot double-stage. */
#define MS_INSERT_SILENCE_MS   500
#define MS_INSERT_ATTEMPTS     10

#define MS_INSERT_LISTEN       0   /* keep waiting; this silence is not a refusal */
#define MS_INSERT_RETRY        1   /* insert the same bubble again               */
#define MS_INSERT_DOOR         2   /* stop asking; offer the one-tap door        */

/* What an insert that has gone MS_INSERT_SILENCE_MS without an answer means,
 * on its `attempt`th try (1-based). ONLY THE COMPACT DRAWER'S SILENCE COUNTS:
 * expanded, the host deliberately parks an accepted insert's completion until
 * later, so a watchdog there would take a yes for a no - it listens and
 * counts nothing. */
static inline int ms_insert_silence(int attempt, int compact)
{
    if (!compact) return MS_INSERT_LISTEN;
    return attempt < MS_INSERT_ATTEMPTS ? MS_INSERT_RETRY : MS_INSERT_DOOR;
}

/* ------------------------------------------------ my own bubble, echoed */

#define MS_RECV_ARRIVAL        0   /* somebody else's move: fold it in        */
#define MS_RECV_ECHO           1   /* my own bubble back: ignore it           */
#define MS_RECV_ECHO_SENT      2   /* my own STAGED bubble back: it was sent  */

/* WHAT A BUBBLE HANDED TO didReceive IS. A drawer opened by tapping a bubble
 * is bound to that bubble's session, and Messages hands the sender's own
 * bubble back through didReceive the moment the send arrow is pressed - a
 * second BEFORE didStartSending on a simulator, and to a second device on the
 * same account for real. Threaded on as an arrival it rebuilds the screen and
 * replays the move the sender just made. So a bubble that is `mine` (the
 * product's own test: the draft, the staged bubble or the last one sent) is
 * never an arrival; and when it is the one `staged`, this IS the send
 * landing, so whatever the product plays at send plays now. */
static inline int ms_receive(int mine, int staged)
{
    if (!mine) return MS_RECV_ARRIVAL;
    return staged ? MS_RECV_ECHO_SENT : MS_RECV_ECHO;
}

#endif
