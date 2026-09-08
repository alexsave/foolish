// THE DRAWER THAT OPENS ITSELF - what these tests can and cannot see.
//
// WHAT THEY CANNOT SEE, said first because round 46 shipped a feature that was
// completely inert while eight tests passed: NOTHING here can tell whether
// Messages honours a `requestPresentationStyle(.expanded)`. That happens in
// another process's sheet, there is no API that reports a dropped request, and
// a unit test cannot stage an MSMessagesAppViewController inside a real host.
// The round-46 suite (NameFieldKeyboardTests) asserted that our code was SHAPED
// right and could not notice that every request it described was being thrown
// away. These tests do not fix that; they cover the other half.
//
// WHAT THEY DO PIN: the decision NameEntryExpand makes, driven through the exact
// event sequence filmed in the real Messages app on an iPhone 17 (iOS 26.3) -
// including the two events that make this a fix rather than a hope: the compact
// `willTransition` that arrives ~0.26s AFTER our request and is the only moment
// a request sticks, and the compact `didTransition` that arrives ~0.4s after
// the drawer has visibly expanded and must NOT be read as "collapse it again".
// Delete the retry and `testTheFilmedColdOpenIssuesTheRetry` fails; delete the
// clear-on-expanded and `testTheLateCompactReportDoesNotReExpand` fails. Both
// were checked by mutation.
//
// THE MEASUREMENT ITSELF is in NameEntryExpand's own header, and it was made
// with a probe build, not with these: eight cold opens, 0/4 landed from
// SwiftUI's `onAppear`, 4/4 landed from the host's compact-install callback.
import XCTest
@testable import FoolishKit

final class NameEntryExpandTests: XCTestCase {

    /// The filmed cold open, to the tenth of a second, from the flight log in
    /// NameEntryExpand's header. `note` is fed exactly what
    /// MessagesViewController feeds it, in exactly that order.
    func testTheFilmedColdOpenIssuesTheRetry() {
        var e = NameEntryExpand()

        // 0.029 - the host states the style it is about to present in. This
        // pair arrives before our view is in the drawer at all and before any
        // name screen exists, so there is nothing to ask for yet.
        XCTAssertFalse(e.note(.transition(toCompact: true), now: 0.029))
        XCTAssertFalse(e.note(.transition(toCompact: true), now: 0.029))

        // 0.170 - a name screen with an empty field appears and asks. We issue,
        // and on a cold open Messages discards this one silently.
        XCTAssertTrue(e.note(.wanted, now: 0.170),
                      "the first ask must still be issued - it is the one that works mid-session")

        // 0.434 - the host installs the drawer. THIS is the fix: without a
        // re-issue here the feature is inert, which is what shipped in 46.
        XCTAssertTrue(e.note(.transition(toCompact: true), now: 0.434),
                      "no retry at the compact install - the feature is inert again")

        // 0.560 - it landed.
        XCTAssertFalse(e.note(.transition(toCompact: false), now: 0.560))
        XCTAssertFalse(e.note(.transition(toCompact: false), now: 0.560))
        XCTAssertFalse(e.pending, "an expanded transition is the answer we asked for")
    }

    /// The trailing compact report - filmed at 0.960, a third of a second after
    /// the drawer had visibly expanded - is the host finishing the transition it
    /// began at 0.434, not a request to shut. Re-expanding on it would fight the
    /// human who dragged the drawer down a moment later, too.
    func testTheLateCompactReportDoesNotReExpand() {
        var e = NameEntryExpand()
        _ = e.note(.wanted, now: 0.170)
        _ = e.note(.transition(toCompact: true), now: 0.434)
        _ = e.note(.transition(toCompact: false), now: 0.560)

        XCTAssertFalse(e.note(.transition(toCompact: true), now: 0.960),
                       "the install's own late report must not become a second expand")
        // And the human, seconds later.
        XCTAssertFalse(e.note(.transition(toCompact: true), now: 8.0),
                       "a collapse the human asked for must not be undone")
    }

    /// Every path except the cold open reaches a name screen with the drawer
    /// already installed, and there the FIRST request is the one that works -
    /// no transition is coming to hang a retry on.
    func testAMidSessionRequestIssuesImmediately() {
        var e = NameEntryExpand()
        // A session that has been running: the install is long past.
        _ = e.note(.transition(toCompact: true), now: 0.4)
        _ = e.note(.transition(toCompact: false), now: 2.0)
        _ = e.note(.transition(toCompact: true), now: 9.0)

        XCTAssertTrue(e.note(.wanted, now: 30.0),
                      "waiting for a transition that is not coming is worse than 46")
    }

    /// Two name screens inside one opening (the setup card, then the lobby's
    /// join row) share the one budget - the drawer is being asked for once.
    func testASecondScreenInTheSameOpeningDoesNotGetItsOwnBudget() {
        var e = NameEntryExpand()
        XCTAssertTrue(e.note(.wanted, now: 0.170))
        XCTAssertFalse(e.note(.wanted, now: 0.200))
        XCTAssertEqual(e.retries, 0)
    }

    /// Bounded, not a poll. The measurement says the first compact transition
    /// after the request is the one that lands; one spare is headroom, and then
    /// the ask is abandoned rather than repeated at every collapse forever.
    func testTheRequestIsAbandonedRatherThanRetriedForever() {
        var e = NameEntryExpand()
        XCTAssertTrue(e.note(.wanted, now: 0.1))
        XCTAssertTrue(e.note(.transition(toCompact: true), now: 0.4))
        XCTAssertTrue(e.note(.transition(toCompact: true), now: 0.7))
        XCTAssertFalse(e.note(.transition(toCompact: true), now: 1.0),
                       "a third re-issue is a poll, not a retry")
        XCTAssertFalse(e.pending)
        XCTAssertFalse(e.note(.transition(toCompact: true), now: 1.3))
    }

    /// A retry only ever belongs to the opening that asked. Filmed, the landing
    /// retry arrived 264ms after the request; a compact transition two seconds
    /// later is a different event in the user's life.
    func testAStaleRequestDoesNotTurnALaterCollapseIntoAnExpand() {
        var e = NameEntryExpand()
        XCTAssertTrue(e.note(.wanted, now: 100.0))
        XCTAssertFalse(e.note(.transition(toCompact: true), now: 102.5),
                       "an expand nobody asked for, minutes after they asked")
        XCTAssertFalse(e.pending)
    }

    /// And the boundary either side of it, so the window is a decision rather
    /// than an accident of comparison.
    func testTheRetryWindowIsInclusiveOfTwoSeconds() {
        var inside = NameEntryExpand()
        _ = inside.note(.wanted, now: 0)
        XCTAssertTrue(inside.note(.transition(toCompact: true), now: 2.0))

        var outside = NameEntryExpand()
        _ = outside.note(.wanted, now: 0)
        XCTAssertFalse(outside.note(.transition(toCompact: true), now: 2.001))
    }

    /// Nothing is issued before a name screen asks. The extension opens compact
    /// on every conversation in the world; only the ones that owe a name may
    /// take the screen over, and that gate is `needsNameEntry` in
    /// MessagesRootView (pinned by NameFieldKeyboardTests) - this is the second
    /// half of it, that no transition on its own is an ask.
    func testTransitionsAloneNeverIssueAnything() {
        var e = NameEntryExpand()
        for t in stride(from: 0.0, through: 5.0, by: 0.25) {
            XCTAssertFalse(e.note(.transition(toCompact: true), now: t))
            XCTAssertFalse(e.note(.transition(toCompact: false), now: t))
        }
        XCTAssertFalse(e.pending)
    }
}
