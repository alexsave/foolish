// THE DRAWER THAT OPENS ITSELF - what these tests can and cannot see.
//
// WHAT THEY CANNOT SEE, said first because an earlier round shipped this
// feature completely inert while eight tests passed: NOTHING here can tell
// whether Messages honours a requestPresentationStyle(.expanded). That happens
// in another process's sheet, there is no API that reports a dropped request,
// and no test on any machine can stage an MSMessagesAppViewController inside a
// real host. The suite that preceded this one asserted that our code was SHAPED
// right and could not notice that every request it described was being thrown
// away. These tests do not fix that; they cover the other half.
//
// WHAT THEY DO PIN: the decision msg_expand_note makes, driven through the exact
// event sequence filmed in the real Messages app on an iPhone 17 (iOS 26.3) -
// including the two events that make this a fix rather than a hope: the compact
// transition that arrives ~0.26s AFTER our request and is the only moment a
// request sticks, and the compact transition that arrives ~0.4s after the drawer
// has visibly expanded and must NOT be read as "collapse it again".
//
// THE MEASUREMENT ITSELF is in msg_expand.h, and it was made with a probe build,
// not with these: eight cold opens, 0/4 landed from SwiftUI's `onAppear`, 4/4
// landed from the host's compact-install callback.
//
// MUTATION-CHECKED. Every one of these was run against a deliberately broken
// kernel, and each mutation below is killed by the named test:
//
//   1. the COMPACT retry returns 0            -> the_filmed_cold_open
//   2. EXPANDED does not clear `pending`      -> the_late_compact_report
//   3. MSG_EXPAND_MAX_RETRIES = 999           -> abandoned_rather_than_retried
//   4. MSG_EXPAND_WINDOW_SEC = 1e9 (infinite) -> a_stale_request, window_boundary
//   5. WANTED returns 0 (no immediate ask)    -> mid_session_request, and 4 more
//
// Usage: msg_expand_test   (no arguments; it is a pure decision, not a fuzz)

#include "../src/msg_expand.h"
#include <stdio.h>

static int g_fails = 0;

#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_fails++; } \
} while (0)

// The filmed cold open, to the tenth of a second, from the flight log in
// msg_expand.h. Fed exactly what MessagesViewController feeds it, in exactly
// that order.
static void test_the_filmed_cold_open(void) {
    MsgExpand e;
    msg_expand_init(&e);

    // 0.029 - the host STATES the style it is about to present in. This pair
    // arrives before our view is in the drawer at all and before any name screen
    // exists, so there is nothing to ask for yet.
    CHECK(msg_expand_note(&e, MSG_EXPAND_COMPACT, 0.029) == 0,
          "the host's opening will/did pair is not an ask");
    CHECK(msg_expand_note(&e, MSG_EXPAND_COMPACT, 0.029) == 0,
          "and neither is its twin");
    CHECK(e.retries == 0, "nothing pending, nothing spent");

    // 0.170 - a name screen with an empty field appears and asks. We issue, and
    // on a cold open Messages discards this one silently.
    CHECK(msg_expand_note(&e, MSG_EXPAND_WANTED, 0.170) == 1,
          "the first ask must still be issued - it is the one that works mid-session");

    // 0.434 - the host installs the drawer. THIS is the fix: without a re-issue
    // here the feature is inert, which is what shipped before it.
    CHECK(msg_expand_note(&e, MSG_EXPAND_COMPACT, 0.434) == 1,
          "no retry at the compact install - the feature is inert again");

    // 0.560 - it landed.
    CHECK(msg_expand_note(&e, MSG_EXPAND_EXPANDED, 0.560) == 0,
          "an expanded transition asks for nothing more");
    CHECK(msg_expand_note(&e, MSG_EXPAND_EXPANDED, 0.560) == 0, "nor does its twin");
    CHECK(e.pending == 0, "an expanded transition is the answer we asked for");
}

// The trailing compact report - filmed at 0.960, a third of a second after the
// drawer had visibly expanded - is the host finishing the transition it began at
// 0.434, not a request to shut. Re-expanding on it would fight the human who
// dragged the drawer down a moment later, too.
static void test_the_late_compact_report(void) {
    MsgExpand e;
    msg_expand_init(&e);
    msg_expand_note(&e, MSG_EXPAND_WANTED, 0.170);
    msg_expand_note(&e, MSG_EXPAND_COMPACT, 0.434);
    msg_expand_note(&e, MSG_EXPAND_EXPANDED, 0.560);

    CHECK(msg_expand_note(&e, MSG_EXPAND_COMPACT, 0.960) == 0,
          "the install's own late report must not become a second expand");
    // And the human, seconds later.
    CHECK(msg_expand_note(&e, MSG_EXPAND_COMPACT, 8.0) == 0,
          "a collapse the human asked for must not be undone");
}

// Every path except the cold open reaches a name screen with the drawer already
// installed, and there the FIRST request is the one that works - no transition
// is coming to hang a retry on.
static void test_mid_session_request_issues_immediately(void) {
    MsgExpand e;
    msg_expand_init(&e);
    // A session that has been running: the install is long past.
    msg_expand_note(&e, MSG_EXPAND_COMPACT, 0.4);
    msg_expand_note(&e, MSG_EXPAND_EXPANDED, 2.0);
    msg_expand_note(&e, MSG_EXPAND_COMPACT, 9.0);

    CHECK(msg_expand_note(&e, MSG_EXPAND_WANTED, 30.0) == 1,
          "waiting for a transition that is not coming is worse than doing nothing");
}

// Two name screens inside one opening (the setup card, then the lobby's join
// row) share the one budget - the drawer is being asked for once.
static void test_second_screen_shares_the_budget(void) {
    MsgExpand e;
    msg_expand_init(&e);
    CHECK(msg_expand_note(&e, MSG_EXPAND_WANTED, 0.170) == 1, "the first screen asks");
    CHECK(msg_expand_note(&e, MSG_EXPAND_WANTED, 0.200) == 0,
          "the second screen inside the same opening does not ask again");
    CHECK(e.retries == 0, "and it does not refresh the budget either");
    // Nor does it move `wanted_at` forward: a late second screen must not
    // silently extend the window past the opening that owns it. Asked at 1.500,
    // a refreshed clock would still call 3.000 fresh; the first ask's does not.
    CHECK(msg_expand_note(&e, MSG_EXPAND_WANTED, 1.500) == 0, "still the one ask");
    CHECK(msg_expand_note(&e, MSG_EXPAND_COMPACT, 3.000) == 0,
          "a second ask must not extend the first ask's window");
}

// Bounded, not a poll. The measurement says the first compact transition after
// the request is the one that lands; one spare is headroom, and then the ask is
// abandoned rather than repeated at every collapse forever.
static void test_abandoned_rather_than_retried_forever(void) {
    MsgExpand e;
    msg_expand_init(&e);
    CHECK(msg_expand_note(&e, MSG_EXPAND_WANTED, 0.1) == 1, "the ask");
    CHECK(msg_expand_note(&e, MSG_EXPAND_COMPACT, 0.4) == 1, "the retry that lands");
    CHECK(msg_expand_note(&e, MSG_EXPAND_COMPACT, 0.7) == 1, "the one spare");
    CHECK(msg_expand_note(&e, MSG_EXPAND_COMPACT, 1.0) == 0,
          "a third re-issue is a poll, not a retry");
    CHECK(e.pending == 0, "and the ask is dropped, not left armed");
    CHECK(msg_expand_note(&e, MSG_EXPAND_COMPACT, 1.3) == 0, "still dropped");
}

// A retry only ever belongs to the opening that asked. Filmed, the landing retry
// arrived 264ms after the request; a compact transition two seconds later is a
// different event in the user's life.
static void test_a_stale_request_does_not_expand(void) {
    MsgExpand e;
    msg_expand_init(&e);
    CHECK(msg_expand_note(&e, MSG_EXPAND_WANTED, 100.0) == 1, "the ask");
    CHECK(msg_expand_note(&e, MSG_EXPAND_COMPACT, 102.5) == 0,
          "an expand nobody asked for, minutes after they asked");
    CHECK(e.pending == 0, "and the stale ask is dropped rather than left armed");
}

// And the boundary either side of it, so the window is a decision rather than an
// accident of comparison.
static void test_the_window_boundary(void) {
    MsgExpand inside;
    msg_expand_init(&inside);
    msg_expand_note(&inside, MSG_EXPAND_WANTED, 0.0);
    CHECK(msg_expand_note(&inside, MSG_EXPAND_COMPACT, 2.0) == 1,
          "exactly two seconds is still the same opening");

    MsgExpand outside;
    msg_expand_init(&outside);
    msg_expand_note(&outside, MSG_EXPAND_WANTED, 0.0);
    CHECK(msg_expand_note(&outside, MSG_EXPAND_COMPACT, 2.001) == 0,
          "a millisecond past it is not");
}

// Nothing is issued before a name screen asks. The extension opens compact on
// every conversation in the world; only the ones that owe a name may take the
// screen over, and that gate is `needsNameEntry` in MessagesRootView - this is
// the second half of it, that no transition on its own is an ask.
static void test_transitions_alone_never_issue(void) {
    MsgExpand e;
    msg_expand_init(&e);
    for (int i = 0; i <= 20; i++) {
        const double t = i * 0.25;
        CHECK(msg_expand_note(&e, MSG_EXPAND_COMPACT, t) == 0,
              "a compact transition with nothing pending is not an ask (t=%.2f)", t);
        CHECK(msg_expand_note(&e, MSG_EXPAND_EXPANDED, t) == 0,
              "and neither is an expanded one (t=%.2f)", t);
    }
    CHECK(e.pending == 0, "and none of them armed anything");
}

// A second opening, after the first was answered, gets a full budget of its own:
// the human collapsed the drawer, came back to a name screen, and is owed the
// same behaviour they got the first time.
static void test_a_later_opening_gets_a_fresh_budget(void) {
    MsgExpand e;
    msg_expand_init(&e);
    msg_expand_note(&e, MSG_EXPAND_WANTED, 0.1);
    msg_expand_note(&e, MSG_EXPAND_COMPACT, 0.4);
    msg_expand_note(&e, MSG_EXPAND_EXPANDED, 0.6);
    // ... the human drags it shut, minutes pass, another name screen appears.
    CHECK(msg_expand_note(&e, MSG_EXPAND_COMPACT, 60.0) == 0, "the drag itself asks nothing");
    CHECK(msg_expand_note(&e, MSG_EXPAND_WANTED, 90.0) == 1, "the new ask is issued");
    CHECK(msg_expand_note(&e, MSG_EXPAND_COMPACT, 90.3) == 1,
          "and it has its retries back");
}

// Defensive, because this crosses a C ABI: a NULL state answers 0 rather than
// dereferencing, and an event code the host never sends changes nothing.
static void test_hostile_inputs(void) {
    CHECK(msg_expand_note(0, MSG_EXPAND_WANTED, 1.0) == 0, "a NULL state asks nothing");
    msg_expand_init(0);   // must not crash

    MsgExpand e;
    msg_expand_init(&e);
    CHECK(e.pending == 0 && e.retries == 0 && e.wanted_at == 0.0,
          "init zeroes the whole budget");
    CHECK(msg_expand_note(&e, 99, 1.0) == 0, "an unknown event is not an ask");
    CHECK(msg_expand_note(&e, -1, 1.0) == 0, "nor is a negative one");
    CHECK(e.pending == 0, "and neither of them armed anything");
}

int main(void) {
    test_the_filmed_cold_open();
    test_the_late_compact_report();
    test_mid_session_request_issues_immediately();
    test_second_screen_shares_the_budget();
    test_abandoned_rather_than_retried_forever();
    test_a_stale_request_does_not_expand();
    test_the_window_boundary();
    test_transitions_alone_never_issue();
    test_a_later_opening_gets_a_fresh_budget();
    test_hostile_inputs();

    if (g_fails) { printf("msg_expand_test: %d FAILURES\n", g_fails); return 1; }
    printf("msg_expand_test: OK\n");
    return 0;
}
