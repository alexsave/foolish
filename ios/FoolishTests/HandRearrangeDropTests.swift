// HandRearrangeDropTests - ROUND 40: "a drag that ends INSIDE THE HAND must
// never resolve as a play, whatever the battle grid claims."
//
// Owner, real device, build 1.0(38): "In collapsed mode, now I can't rearrange
// cards. It seems to trigger attack." Sliding a card sideways within the hand to
// reorder it threw it on the table as an attack. Compact drawer only.
//
// THE MEASUREMENT these tests are built on is not invented. It was read off the
// rig (FoolishHarness, iPhone 16, a 20-card hand collapsed into a ~276pt drawer,
// so the fan is two rows deep) by printing `HandFrameKey` beside `BattleFramesKey`
// in `boardSpace`:
//
//     hand      = (16, 106) 343x166      rows centred at y=150 and y=228
//     battle[0] = (156, 101)  62x84      x 156...218, y 101...185
//     cancel band (MessageTableView.handDropFrame, hand grown 64 up / 24 down)
//               = (16, 42) 343x254
//
// The hand's entire TOP ROW lies inside the battle grid, and the band swallows
// battle[0] whole. Those four rects are the whole bug, so they are the fixture.
//
// These are pure-function tests on purpose (the same approach as Round6DragTests
// / Round5BoardTests): no simulator, no snapshot, just the arithmetic that
// decides what gets played. `HandRowDragTests` does NOT cover this - it drives
// `splice` at width 340 with crop 0 and never touches the boardSpace translation
// or the drop-target decision.
import XCTest
@testable import FoolishKit

final class HandRearrangeDropTests: XCTestCase {

    // The measured compact drawer, verbatim.
    private let hand = CGRect(x: 16, y: 106, width: 343, height: 166)
    private let battles = [0: CGRect(x: 156, y: 101, width: 62, height: 84)]
    /// What the board actually passes `BoardDrop.target` - round-7 #3's cancel
    /// band, NOT the hand (MessageTableView.handDropFrame: 64 up, 24 down).
    private var band: CGRect {
        CGRect(x: hand.minX, y: hand.minY - 64, width: hand.width, height: hand.height + 64 + 24)
    }
    /// The resting centre of hand slot 4 - the top row, dead inside battle[0].
    /// This is a point the player is holding one of their own cards at.
    private let onACardInTheTopRow = CGPoint(x: 170, y: 150)

    private func c(_ s: Int, _ v: Int) -> Card { Card(s: s, v: v) }

    // MARK: the bug, stated as geometry

    /// The precondition, and it stays TRUE after the fix: `BoardDrop` on its own
    /// still calls a point on the player's own card a battle. It has no way not
    /// to - it is handed the cancel band and never the hand (see
    /// `FHandFan.boardPoint`). Pinned so nobody "fixes" this by reordering
    /// `BoardDrop`, which would make the band beat battle[0] and leave every
    /// cover in the compact drawer unreachable.
    func testTheBattleGridClaimsAPointInsideTheHand() {
        XCTAssertTrue(hand.contains(onACardInTheTopRow), "the fixture point is on a hand card")
        XCTAssertEqual(BoardDrop.target(at: onACardInTheTopRow, battles: battles, handFrame: band),
                       .battle(0))
        // And it is not the 8pt slack doing it: the point is inside the battle's
        // RAW rect. Shrinking the slack would change nothing.
        XCTAssertTrue(battles[0]!.contains(onACardInTheTopRow))
    }

    // MARK: the invariant

    /// THE BUG. A release still on the cards must reach the board as `.hand`.
    /// Against 1.0(38) this is `.battle(0)` and the card is played.
    func testAReleaseOnTheCardsResolvesToTheHand() {
        let reported = FHandFan.boardPoint(onACardInTheTopRow, hand: hand)
        XCTAssertEqual(BoardDrop.target(at: reported, battles: battles, handFrame: band), .hand)
    }

    /// Every slot centre of the measured two-row hand, not just the one that
    /// happened to be over a battle - the rule is about the hand, not about slot 4.
    func testNoRestingSlotInTheCompactHandCanPlayACard() {
        let slots = FHandFan.slotFrames(count: 20, width: hand.width)
        XCTAssertEqual(FHandFan.rowCount(count: 20, availableWidth: hand.width), 2,
                       "the fixture is the TWO-ROW hand; a one-row hand never overlaps")
        for (i, s) in slots.enumerated() {
            let centre = CGPoint(x: s.midX + hand.minX, y: s.midY + hand.minY)
            let reported = FHandFan.boardPoint(centre, hand: hand)
            XCTAssertEqual(BoardDrop.target(at: reported, battles: battles, handFrame: band), .hand,
                           "slot \(i) at \(centre) must read as a rearrange")
        }
    }

    /// The projection keeps x, so it stays over the card the finger was on, and
    /// it lands INSIDE the hand (a `contains` test excludes maxY).
    func testTheProjectionStaysOverTheSameCardAndInsideTheHand() {
        let reported = FHandFan.boardPoint(onACardInTheTopRow, hand: hand)
        XCTAssertEqual(reported.x, onACardInTheTopRow.x, accuracy: 0.0001)
        XCTAssertTrue(hand.contains(reported))
        XCTAssertGreaterThan(reported.y, onACardInTheTopRow.y, "projected onto the hand's floor")
    }

    // MARK: what must NOT change

    /// ROUND-7 #3 IS UNTOUCHED. Its 64pt upward widening exists because a
    /// compact-drawer rearrange whose finger drifts off the thin cropped strip
    /// used to land on `.table` and be rejected with "move not allowed". A
    /// release 30pt above the hand's top edge is NOT inside the hand, so it is
    /// passed through unprojected and still falls through to the cancel band.
    func testTheCancelBandAboveTheHandStillCancels() {
        let justAbove = CGPoint(x: 60, y: hand.minY - 30)   // clear of battle[0] in x
        XCTAssertFalse(hand.contains(justAbove))
        XCTAssertEqual(FHandFan.boardPoint(justAbove, hand: hand), justAbove)
        XCTAssertEqual(BoardDrop.target(at: justAbove, battles: battles, handFrame: band), .hand)
    }

    /// A real cover is still reachable in the compact drawer: a drop on the
    /// battle ABOVE the hand's top edge is untouched by the projection and still
    /// beats the band. This is what the ordering cure would have broken.
    func testACoverDropOntoTheBattleStillLands() {
        let onTheBattle = CGPoint(x: 187, y: 104)           // battle[0], above hand.minY
        XCTAssertFalse(hand.contains(onTheBattle))
        XCTAssertEqual(BoardDrop.target(at: FHandFan.boardPoint(onTheBattle, hand: hand),
                                        battles: battles, handFrame: band), .battle(0))
    }

    /// A hand that has not published a frame yet (`.zero`) must not swallow
    /// anything - the very first drag of a board must still be able to play.
    func testAnUnpublishedHandFrameChangesNothing() {
        let p = CGPoint(x: 187, y: 104)
        XCTAssertEqual(FHandFan.boardPoint(p, hand: .zero), p)
    }

    // MARK: the resolver's own half

    /// The resolver used to answer `.hand` with an ATTACK for an attacker: the
    /// attacker branch reads only the cards and ignored `target` entirely. Only
    /// the boards' own `if target == .hand { return }` stood between a rearrange
    /// and a played card. The rule is the kernel's now (play_resolve).
    private func resolve(_ cards: [Card], _ target: PlayTarget, defender: Bool,
                         battles: [BattleView], legal: [Move]) -> Move? {
        PlayWire.probe(menu: MoveWire.encode(legal), battles: battles, powerSuit: 3,
                       isDefender: defender, selection: cards, target: target).move
    }

    func testAnAttackerToldTheHandPlaysNothing() {
        let six = c(0, 6)
        let legal = [Move(type: .attack, cards: [six])]
        XCTAssertNil(resolve([six], .hand, defender: false, battles: [], legal: legal))
        // …and the attack itself still resolves from the table, unchanged.
        XCTAssertEqual(resolve([six], .table, defender: false, battles: [], legal: legal)?.type,
                       .attack)
    }

    /// The defender's side of the same rule, which already held - pinned so the
    /// hoist out of the defender switch cannot quietly drop it.
    func testADefenderToldTheHandPlaysNothing() {
        let attack = c(0, 9), cover = c(0, 13)
        let bs = [BattleView(attack: attack, defense: nil)]
        let legal = [Move(type: .cover, cards: [cover], attackCards: [attack])]
        XCTAssertNil(resolve([cover], .hand, defender: true, battles: bs, legal: legal))
        XCTAssertEqual(resolve([cover], .battle(0), defender: true, battles: bs, legal: legal)?.type,
                       .cover)
    }
}
