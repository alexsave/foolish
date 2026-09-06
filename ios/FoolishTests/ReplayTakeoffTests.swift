// MY OWN ATTACK LEAVES MY OWN HAND - round 42.
//
// THE REPORT (owner, real device, 1.0(41)): "When I replay an attack of mine, it
// seems to animate from where my 'player card count' would be. This is
// absolutely fine for any OTHER player attack animation, however for my own
// attack animations, notice that there is no self player mini-hand visual. Thus
// what I'm seeing is that they spawn in like behind the cards in my hand, then
// fly in to their correct positions. All I need you to do, is when we replay
// OUR OWN attack, it should go FROM OUR HAND to the table. The end coordinates
// are fine, but not the start. Of course this will also mean we'll want the
// hand to rearrange as a result of the cards leaving."
//
// Two halves, and the second is not a nicety - it is what makes the first one
// true. An open replay renders the FINAL board, so my hand has already lost
// those cards and already re-centred before a single frame is drawn; a takeoff
// aimed at their old slots would launch from positions no card occupies. So the
// fan holds them (`handHoldback`) until their flight is built.
//
// These pin the pure surface that decision rests on. The flight builder itself
// is not reachable without a board - it reads @State frames - but everything it
// asks a question of is here, and each of these answers was wrong before.
//
// MUTATION-CHECKED, three mutants:
//   A  `fanCards` appends unconditionally (no `present` filter): fails
//      testAHoldbackNeverDoublesACardTheHandStillHolds - and on a device that
//      is two cards drawn into one slot.
//   B  `myPlacedCards` drops the seat filter: fails
//      testOnlyMyOwnPlacementsAreHeld.
//   C  `myPlacedCards` counts every event kind: fails
//      testADrawIsNotAPlacement and testEveryPlacementInTheStreamIsHeldInOrder.

import XCTest
@testable import FoolishKit

final class ReplayTakeoffTests: XCTestCase {

    private func c(_ s: Int, _ v: Int) -> Card { Card(s: s, v: v) }

    private func ev(_ kind: EventType, seat: Int, cards: [Card?]) -> GameEvent {
        GameEvent(type: kind.rawValue, seat: seat, msg: 0,
                  from: 0, to: 0, cards: cards, target: nil, battle: nil, state: nil)
    }

    // MARK: - what the fan is asked to lay out

    /// The ordinary board: no replay, no holdback, the hand is the hand. This is
    /// the case every other board in the app is in, so it is the one that must
    /// be provably free.
    func testWithNothingHeldTheFanGetsTheHandUnchanged() {
        // REAL cards: the fan is laid out by the kernel now (anim_fan_cards
        // over dense ids), and a value outside 1...13 is not a card. This read
        // `c(2, 14)` while the rule was Swift set algebra over identity strings,
        // which never asked whether the card could exist.
        let hand = [c(0, 6), c(1, 11), c(2, 13)]
        XCTAssertEqual(HandLayout.fanCards(hand, holding: []), hand)
    }

    /// THE FIRST HALF. The cards that left are back in the fan, so the layout
    /// the takeoff is computed against is the PRE-move one.
    func testHeldCardsRejoinTheFan() {
        let hand = [c(0, 6), c(1, 11)]
        let held = [c(2, 7), c(2, 8)]
        let laid = HandLayout.fanCards(hand, holding: held)
        XCTAssertEqual(laid.count, 4)
        XCTAssertEqual(Set(laid.map(\.identity)), Set((hand + held).map(\.identity)))
        XCTAssertEqual(Array(laid.prefix(2)), hand, "the present hand keeps its order")
    }

    /// A card the kernel hand ALREADY holds must not be added a second time.
    /// The fan places cards BY INDEX, so a duplicate identity is two cards
    /// stacked in one slot and every card after it shifted by one - the whole
    /// hand laid out wrong, from a holdback that merely lagged a frame behind
    /// an undo putting the card back.
    func testAHoldbackNeverDoublesACardTheHandStillHolds() {
        let seven = c(2, 7)
        let hand = [c(0, 6), seven]
        let laid = HandLayout.fanCards(hand, holding: [seven, c(2, 8)])
        XCTAssertEqual(laid.count, 3, "the 7 is in hand already - it is held ONCE")
        XCTAssertEqual(laid.map(\.identity).sorted(),
                       [c(0, 6), seven, c(2, 8)].map(\.identity).sorted())
        XCTAssertEqual(Set(laid.map(\.identity)).count, laid.count, "no identity twice")
    }

    /// An empty hand is the genesis / just-went-out board, and it must still be
    /// able to hold a departure back.
    func testAnEmptyHandStillHolds() {
        XCTAssertEqual(HandLayout.fanCards([], holding: [c(0, 6)]).count, 1)
        XCTAssertTrue(HandLayout.fanCards([], holding: []).isEmpty)
    }

    // MARK: - what gets held

    /// The seed: only the cards I MYSELF put on the table. An opponent's attack
    /// never came out of my hand, and holding it would draw their card in my
    /// fan.
    func testOnlyMyOwnPlacementsAreHeld() {
        let mine = [c(0, 7), c(1, 7)]
        let theirs = [c(2, 9)]
        let events = [
            ev(.attackPass, seat: 3, cards: theirs.map { $0 }),
            ev(.attackPass, seat: 1, cards: mine.map { $0 }),
        ]
        XCTAssertEqual(HandLayout.myPlacedCards(events, mySeat: 1).map(\.identity),
                       mine.map(\.identity))
        XCTAssertEqual(HandLayout.myPlacedCards(events, mySeat: 3).map(\.identity),
                       theirs.map(\.identity))
        XCTAssertTrue(HandLayout.myPlacedCards(events, mySeat: 0).isEmpty)
    }

    /// A COVER is a placement too - it leaves my hand exactly like an attack,
    /// and the owner's report names the attack only because that is the one
    /// they happened to watch.
    func testACoverIsHeldLikeAnAttack() {
        let king = c(0, 13)
        let events = [ev(.cover, seat: 2, cards: [king])]
        XCTAssertEqual(HandLayout.myPlacedCards(events, mySeat: 2).map(\.identity),
                       [king.identity])
    }

    /// A DRAW IS NOT A PLACEMENT. It moves the other way - deck into my hand -
    /// and it already has a takeoff of its own (the deck). Holding a drawn card
    /// would put it in the fan before it was dealt and then fly it in on top of
    /// itself.
    func testADrawIsNotAPlacement() {
        let events = [
            ev(.refill, seat: 1, cards: [c(0, 6)]),
            ev(.deal, seat: 1, cards: [c(0, 7)]),
        ]
        XCTAssertTrue(HandLayout.myPlacedCards(events, mySeat: 1).isEmpty)
        XCTAssertFalse(HandLayout.isPlacement(.refill))
        XCTAssertFalse(HandLayout.isPlacement(.deal))
        XCTAssertTrue(HandLayout.isPlacement(.attackPass))
        XCTAssertTrue(HandLayout.isPlacement(.defenderMove))
        XCTAssertTrue(HandLayout.isPlacement(.cover))
    }

    /// The kernel REDACTS cards that are not the viewer's (`GameEvent.cards`
    /// carries nil for them). My own placement is never redacted, but a stream
    /// is walked whole, so a nil must drop out rather than crash or become a
    /// phantom slot.
    func testRedactedCardsAreSkipped() {
        let events = [ev(.attackPass, seat: 1, cards: [nil, c(0, 7), nil])]
        XCTAssertEqual(HandLayout.myPlacedCards(events, mySeat: 1).map(\.identity),
                       [c(0, 7).identity])
    }

    /// Several placements in one stream are held TOGETHER and in stream order -
    /// a throw-in after my own attack is still my card leaving my hand, and the
    /// fan lays them out in the order they were held.
    func testEveryPlacementInTheStreamIsHeldInOrder() {
        let a = c(0, 7), b = c(1, 7), d = c(2, 12)
        let events = [
            ev(.attackPass, seat: 0, cards: [a, b]),
            ev(.refill, seat: 0, cards: [c(3, 6)]),
            ev(.cover, seat: 0, cards: [d]),
        ]
        XCTAssertEqual(HandLayout.myPlacedCards(events, mySeat: 0).map(\.identity),
                       [a, b, d].map(\.identity))
    }
}
