// HandOrderFlightTests — ROUND 12: a deal must land in the slot the hand will
// actually put it in, not the slot an unsorted hand would have had.
//
// THE BUG. Round-8 #4 gave the message board a persisted per-game hand
// arrangement: drag your cards into an order and it survives closing and
// reopening the game. The FAN honoured it. The flight builders did not — they
// fed `handLandingSlot` the kernel's own hand order, and that function turns an
// array into slot rects BY INDEX, so it was describing a hand nobody was looking
// at. A card dealt into a rearranged hand flew to the slot it would have had in
// an unsorted one (the right-hand end) and then snapped into place a frame later:
// "the deal animation will give the rearranged card to the right regardless,
// then suddenly jump to the preferred order."
//
// WHY THE EXISTING DEBUG CHECK MISSED IT. `MessageTableView`'s SLOTCHECK already
// compared analytic slots against measured ones — but it computed its analytic
// slots from the DISPLAY order, while the flights used the kernel order. The
// check and the thing it was checking disagreed about which array they were
// describing, so they never disagreed with each other.
//
// So these assert the seam that was actually broken: `laidOut` must apply the
// saved arrangement. Asserting `FHandFan.displayOrder` instead would prove
// nothing — that function was always correct; it just was not being called.

import XCTest
@testable import FoolishKit

final class HandOrderFlightTests: XCTestCase {

    private func hand() -> [Card] {
        [Card(s: 0, v: 6), Card(s: 1, v: 9), Card(s: 2, v: 12), Card(s: 3, v: 7)]
    }

    /// With a saved arrangement, the laid-out hand IS that arrangement — which is
    /// what the fan renders, so it is what a landing slot must be computed from.
    func testLaidOutFollowsTheSavedArrangement() {
        let cards = hand()
        let reversed = cards.reversed().map(\.identity)
        let laid = MessageTableView.laidOut(hand: cards, deferred: [], order: reversed)
        XCTAssertEqual(laid.map(\.identity), reversed,
                       "the flight builders must lay the hand out the way the FAN does")
        XCTAssertNotEqual(laid.map(\.identity), cards.map(\.identity),
                          "…and this fixture's arrangement really does differ from kernel order, "
                          + "so the assertion above cannot pass by coincidence")
    }

    /// A freshly dealt card is not in the saved arrangement yet. It appends at the
    /// end — the same rule `FHandFan.displayOrder` uses for the fan — so both
    /// agree about where it lands.
    func testADealtCardAppendsInBothTheFanAndTheFlight() {
        let cards = hand()
        let dealt = Card(s: 1, v: 13)
        let order = cards.map(\.identity)          // the arrangement predates the deal
        let laid = MessageTableView.laidOut(hand: cards + [dealt], deferred: [], order: order)
        XCTAssertEqual(laid.map(\.identity), order + [dealt.identity])
        XCTAssertEqual(laid.map(\.identity),
                       FHandFan.displayOrder(cards: cards + [dealt], order: order).map(\.identity),
                       "the flight's layout and the fan's layout are the same function")
    }

    /// A deal whose flight has not started yet reserves no width (round-6 bug 10),
    /// and the arrangement still applies to what is left.
    func testDeferredCardsAreExcludedButTheOrderStillHolds() {
        let cards = hand()
        let reversed = cards.reversed().map(\.identity)
        let deferred: Set<String> = [cards[0].identity]
        let laid = MessageTableView.laidOut(hand: cards, deferred: deferred, order: reversed)
        XCTAssertEqual(laid.map(\.identity), reversed.filter { $0 != cards[0].identity })
    }

    /// No saved arrangement (the common case): kernel order, unchanged — so this
    /// fix cannot have moved anything for a player who never rearranged.
    func testNoArrangementIsKernelOrder() {
        let cards = hand()
        XCTAssertEqual(MessageTableView.laidOut(hand: cards, deferred: [], order: []).map(\.identity),
                       cards.map(\.identity))
    }
}
