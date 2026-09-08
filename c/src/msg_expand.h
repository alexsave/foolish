/* msg_expand.h - WHEN MESSAGES ACTUALLY HONOURS AN `.expanded` REQUEST.
 *
 * A name screen with an empty field wants the drawer open and the keyboard up
 * without a second tap. Deciding WHETHER to ask is the surface's job (only a
 * device that owes a nickname may take the screen over). Deciding WHEN the ask
 * is issued is this file's, and it is a kernel decision because the answer was
 * MEASURED rather than reasoned: the same request, issued 300ms apart, is
 * honoured every time or discarded every time.
 *
 * WHAT WAS MEASURED. A probe build in the real Messages app on an iPhone 17
 * (iOS 26.3), instrumented to issue exactly ONE requestPresentationStyle
 * (.expanded) per cold open and to report whether the host ever answered.
 * Eight cold opens, alternating between the two moments:
 *
 *   issued from SwiftUI `onAppear` (~0.17s in)                     0/4 landed
 *   issued in the host's compact-install `willTransition` (~0.47s)  4/4 landed
 *
 * The landing ones expanded 127ms after the request and the keyboard came up
 * 9ms after that; the dropped ones produced no transition at all. One session's
 * trace, times in seconds from the extension's launch:
 *
 *   0.029  willTransition -> compact,  didTransition -> compact   (see below)
 *   0.080  our view is laid out at 874pt - FULL SCREEN, not the drawer
 *   0.170  a name screen appears and asks to expand         <- DROPPED, silently
 *   0.433  the surface snaps 874 -> 854 -> 298pt: the drawer
 *   0.434  willTransition -> compact                        <- from here it sticks
 *   0.560  willTransition/didTransition -> expanded
 *   0.569  the field takes the keyboard
 *   0.960  didTransition -> compact  (the install's own, arriving late)
 *
 * THE MOMENT, NOT A DELAY. The pair at 0.029 arrives synchronously with
 * `willBecomeActive`, before Messages has installed our view in the drawer at
 * all - it is the host STATING the style it is about to present in, not a
 * transition it has performed. The real installation is the one at 0.434, and a
 * request made before it is discarded with no callback, no error and no second
 * chance. So the fix is not "sleep long enough": it is "ask again the first time
 * the host tells us it is moving the drawer". A delay would be a bet on a number
 * that was never the mechanism - the measurement says the mechanism is the
 * EVENT, because a request issued with ZERO delay from inside that callback
 * lands every time.
 *
 * THE TWO TRAPS this shape exists to avoid, both filmed:
 *
 *   1. The 0.029 pair must not be read as the install. Nothing is pending then,
 *      so nothing is spent on it.
 *   2. The host's own `presentationStyle` property kept answering "compact" for
 *      ~0.4s AFTER the drawer had visibly expanded. A pending request is
 *      therefore cleared by the transition CALLBACK and never by asking the
 *      sheet where it is; polling that property re-expands forever.
 *
 * Written the way the turn controller is (msg_wire.h): facts in, an answer out.
 * The three fields of MsgExpand are the whole memory, the caller owns them, the
 * caller passes the clock, and nothing here calls the host or the time.
 */
#ifndef MSG_EXPAND_H
#define MSG_EXPAND_H

/* What can change the answer. `WANTED` is a name screen asking; the other two
 * are the host's presentation-style callbacks. Only the STYLE matters, not the
 * phase - both `will` and `did` are fed in, because the filmed sequence proves
 * the useful one (the compact install) announces itself with a `will` whose
 * `did` does not arrive for another half second. */
#define MSG_EXPAND_WANTED   0   /* a name screen with an empty field asked */
#define MSG_EXPAND_COMPACT  1   /* the host reported a transition to compact */
#define MSG_EXPAND_EXPANDED 2   /* the host reported a transition to expanded */

/* ONE spare. The measurement says the FIRST compact transition after the
 * request is the one that lands (4/4), so a second re-issue is headroom for a
 * device that sequences its installation differently - not a poll. */
#define MSG_EXPAND_MAX_RETRIES 2

/* And a window, because a retry is only ever the tail of the SAME opening.
 * Filmed, the landing retry arrived 264-370ms after the request; two seconds is
 * more than five times the worst of those, and it is what stops a stale request
 * from turning a collapse the human asked for, minutes later, into an expand
 * nobody asked for. Inclusive: exactly two seconds is still the same opening. */
#define MSG_EXPAND_WINDOW_SEC 2.0

/* The whole memory of one opening's ask. Zero it (msg_expand_init) and hand it
 * back to every msg_expand_note call; there are no statics here, so two hosts -
 * or a test and a host - never share a budget. */
typedef struct {
    int    pending;    /* a request is out and the host has not answered it */
    int    retries;    /* re-issues spent on the current request */
    double wanted_at;  /* the caller's clock when the current request was made */
} MsgExpand;

void msg_expand_init(MsgExpand *st);

/* Note an event; 1 means "issue requestPresentationStyle(.expanded) NOW".
 *
 * `now` is any monotonic seconds clock - the extension passes
 * CACurrentMediaTime(), a test passes whatever it likes. A NULL state or an
 * unknown event answers 0.
 *
 * The host performs the effect and owns every callback; this only decides. */
int msg_expand_note(MsgExpand *st, int event, double now);

#endif /* MSG_EXPAND_H */
