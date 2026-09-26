/* msg_stage.h - getting a staged bubble into Messages' input field, decided
 * once for every iMessage product (INSERT_GATING.md beside this file).
 *
 * Three decisions, each one a device log's worth of evidence:
 *   ms_drawer_up       may an insert go now - is the extension view a drawer
 *                      yet, or still the window-sized first appearance?
 *   ms_stage_*         one stage's insert loop: what silence, an error, the
 *                      drawer collapsing, a tap on the door and the timers
 *                      the host runs mean, with the budgets for each;
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

/* ------------------------------------------------ the insert loop */

/* A REFUSED INSERT NEVER ANSWERS. ChatKit drops an insert that arrives before
 * the host counts the drawer as presenting and calls no completion at all -
 * not with an error, not ever (INSERT_GATING.md). So silence is the refusal:
 * an insert unanswered after MS_INSERT_SILENCE_MS is asked again, up to
 * MS_INSERT_ATTEMPTS in all (about five seconds), and then the human is
 * handed a door that inserts on a tap - by which time the drawer is
 * presenting and the gate passes. An accepted try answers at once, so a
 * refused try leaves nothing behind and a retry cannot double-stage.
 *
 * AN INSERT ANSWERED WITH AN ERROR is a different vector with its own budget:
 * the gate passed and something downstream refused (a payload Messages would
 * not validate, the camera context). It is asked again a beat later, up to
 * MS_INSERT_ERRORS tries with an error in all, and the last one's error
 * reverts the draft, so the board never shows a move the field does not
 * hold. The two budgets never touch: ten silences do not spend an error, and
 * two errors do not spend a silence.
 *
 * ONLY THE COMPACT DRAWER'S SILENCE COUNTS. Expanded, the host parks an
 * accepted insert's completion until the drawer leaves full screen, so a
 * watchdog there would take a yes for a no. And the only way a human can
 * send is to collapse the drawer, which is exactly what releases a parked
 * yes or proves a drop. So a silence while the drawer is not compact PARKS
 * the stage: no timer, nothing counted, until the host reports the drawer
 * compact; then the same try's watchdog is armed once more, and its silence
 * is a refusal like any other. A parked stage costs nothing while it waits
 * and cannot wait past the collapse that has to come before a send. */
#define MS_INSERT_SILENCE_MS   500
#define MS_INSERT_ATTEMPTS     10
#define MS_INSERT_ERROR_MS     350
#define MS_INSERT_ERRORS       3

/* Where one stage's insert loop stands. */
#define MS_STAGE_IDLE          0   /* no try out; the host has not inserted yet */
#define MS_STAGE_WAITING       1   /* a try is out and its watchdog is armed     */
#define MS_STAGE_DELAY         2   /* an error; the next try goes after a beat   */
#define MS_STAGE_PARKED        3   /* unanswered while not compact; waiting for compact */
#define MS_STAGE_DOOR          4   /* out of silent tries; the door is up        */
#define MS_STAGE_LANDED        5   /* in the field                               */
#define MS_STAGE_DONE          6   /* reverted; nothing of this stage goes again */

/* What the host does next. Every timer it starts and every insert it issues
 * carries `try_no` as it stood when the action was handed over, and hands it
 * back with the event, so a timer of a try that has been overtaken changes
 * nothing. */
#define MS_ACT_NONE            0   /* nothing; the event was stale or moot        */
#define MS_ACT_INSERT          1   /* insert the bubble as try_no, arm its watchdog */
#define MS_ACT_INSERT_LATER    2   /* after MS_INSERT_ERROR_MS hand ms_stage_due   */
#define MS_ACT_PARK            3   /* no timer; hand ms_stage_compact when compact */
#define MS_ACT_ARM             4   /* arm try_no's watchdog once more             */
#define MS_ACT_DOOR            5   /* offer the one-tap door                      */
#define MS_ACT_LANDED          6   /* in the field: hide the door, start the hint */
#define MS_ACT_REVERT          7   /* give up: treat the draft as cancelled       */

typedef struct ms_stage {
    int state;    /* MS_STAGE_* */
    int try_no;   /* tries issued so far, 1-based; 0 before the first */
    int silent;   /* compact silences since the first try or the last door tap */
    int errors;   /* errors since the same */
} ms_stage;

/* A new stage, a send or a cancel: nothing of the old loop goes again. */
static inline void ms_stage_reset(ms_stage *s)
{
    s->state = MS_STAGE_IDLE;
    s->try_no = 0;
    s->silent = 0;
    s->errors = 0;
}

static inline int ms_stage_issue(ms_stage *s)
{
    s->try_no++;
    s->state = MS_STAGE_WAITING;
    return MS_ACT_INSERT;
}

/* The bubble is painted and the drawer is where the stage wants it: the
 * first try. Always MS_ACT_INSERT, as try 1. */
static inline int ms_stage_first(ms_stage *s)
{
    ms_stage_reset(s);
    return ms_stage_issue(s);
}

/* Try `try_no` has gone MS_INSERT_SILENCE_MS without an answer; `compact` is
 * whether the drawer is compact NOW, not when the try went out. */
static inline int ms_stage_silence(ms_stage *s, int try_no, int compact)
{
    if (s->state != MS_STAGE_WAITING || try_no != s->try_no) return MS_ACT_NONE;
    if (!compact) {
        s->state = MS_STAGE_PARKED;
        return MS_ACT_PARK;
    }
    s->silent++;
    if (s->silent < MS_INSERT_ATTEMPTS) return ms_stage_issue(s);
    s->state = MS_STAGE_DOOR;
    return MS_ACT_DOOR;
}

/* Try `try_no` answered: `ok` is a nil error. A YES FROM ANY TRY LANDS, a
 * late one, a parked one, one behind a door - the bubble is in the field,
 * whichever try put it there. An error counts only for the try that is
 * current and still waiting: an overtaken try's error says nothing the newer
 * try will not say for itself, and a door already up is the human's path. */
static inline int ms_stage_answer(ms_stage *s, int try_no, int ok)
{
    if (s->state == MS_STAGE_DONE || s->state == MS_STAGE_IDLE) return MS_ACT_NONE;
    if (ok) {
        if (s->state == MS_STAGE_LANDED) return MS_ACT_NONE;
        s->state = MS_STAGE_LANDED;
        return MS_ACT_LANDED;
    }
    if (try_no != s->try_no) return MS_ACT_NONE;
    if (s->state != MS_STAGE_WAITING && s->state != MS_STAGE_PARKED) return MS_ACT_NONE;
    s->errors++;
    if (s->errors < MS_INSERT_ERRORS) {
        s->state = MS_STAGE_DELAY;
        return MS_ACT_INSERT_LATER;
    }
    s->state = MS_STAGE_DONE;
    return MS_ACT_REVERT;
}

/* The beat after try `try_no`'s error has passed. */
static inline int ms_stage_due(ms_stage *s, int try_no)
{
    if (s->state != MS_STAGE_DELAY || try_no != s->try_no) return MS_ACT_NONE;
    return ms_stage_issue(s);
}

/* The host reports the drawer compact (didTransition). Only a parked stage
 * has anything to do: its try's watchdog goes up again. */
static inline int ms_stage_compact(ms_stage *s)
{
    if (s->state != MS_STAGE_PARKED) return MS_ACT_NONE;
    s->state = MS_STAGE_WAITING;
    return MS_ACT_ARM;
}

/* The door was tapped: the same bubble again, from a drawer that is by now
 * certainly presenting, with both budgets whole. Tries keep counting up, so
 * an answer to a try from before the door is still told apart. */
static inline int ms_stage_door_tap(ms_stage *s)
{
    if (s->state != MS_STAGE_DOOR) return MS_ACT_NONE;
    s->silent = 0;
    s->errors = 0;
    return ms_stage_issue(s);
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
