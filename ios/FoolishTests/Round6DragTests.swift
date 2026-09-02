// Round6DragTests — the two round-6 drag findings, pinned as the pure functions
// the board actually calls (same approach as CoverTiltTests/Round5BoardTests: no
// simulator, no snapshot, just the arithmetic that decides what you see).
//
// Bug 5: "Action text preview on drag is broken, sometimes appearing quite far
// away from the cards. Just keep it right above the dragged card." The pill used
// to take its X from the dragged card and its Y from the FINGER, so any grab
// that wasn't dead-centre on the card left it floating a card-height off.
//
// Bug 13: "When we drag a card then let go, it should animate from where we let
// go to the table, not back from its original position in hand to the table."
// The play's source rect is now the release point for the card the finger was
// holding, and the resting hand slot for everything it wasn't.
import XCTest
@testable import FoolishKit

final class Round6DragTests: XCTestCase {

    // MARK: bug 5 — the verb hint rides the CARD, on both axes

    /// The bug itself: a finger 30pt below the card's centre (you grabbed the
    /// card near its top edge) used to drag the pill down with it. The pill must
    /// come off the card alone.
    func testTheHintIgnoresTheFingerWhenTheCardCentreIsKnown() {
        let card = CGPoint(x: 140, y: 500)
        let at = MessageTableView.dragHintPosition(cardCentre: card, restingCentre: nil,
                                                   finger: CGPoint(x: 190, y: 530), origin: .zero)
        XCTAssertEqual(at.x, card.x, accuracy: 0.001, "X comes off the card (round-5 finding 5)")
        XCTAssertEqual(at.y, card.y - MessageTableView.dragHintLift, accuracy: 0.001,
                       "Y must come off the card too, NOT the fingertip 30pt lower")
    }

    /// A 72pt hand card lifted 52pt from its centre clears its own top edge by
    /// 16pt: attached to the card, clear of the thumb. This is the number the
    /// owner asked for ("right above the dragged card") and the reason the
    /// pre-existing 52 was kept when the anchor moved from finger to card.
    func testTheLiftLeavesThePillJustAboveTheCardsTopEdge() {
        let cardTop = 500 - 36.0            // FHandFan card height 72 -> half is 36
        // The finger sits 30pt off the card's centre on purpose: this pins the
        // GAP TO THE CARD, so it cannot pass while the pill is measured off
        // anything else.
        let at = MessageTableView.dragHintPosition(cardCentre: CGPoint(x: 100, y: 500),
                                                   restingCentre: nil,
                                                   finger: CGPoint(x: 130, y: 530), origin: .zero)
        XCTAssertEqual(cardTop - at.y, 16, accuracy: 0.001)
        XCTAssertLessThan(at.y, cardTop, "the pill sits ABOVE the card, never on it")
    }

    /// The first frame of a drag: FHandFan has reported a drag point but not yet
    /// a card centre. The card has not moved at all in that frame, so its resting
    /// slot IS the card — the pill must not jump from the finger to the card a
    /// frame later.
    func testTheRestingSlotStandsInBeforeTheFirstReportedCentre() {
        let resting = CGPoint(x: 60, y: 620)
        let at = MessageTableView.dragHintPosition(cardCentre: nil, restingCentre: resting,
                                                   finger: CGPoint(x: 95, y: 655), origin: .zero)
        XCTAssertEqual(at.x, resting.x, accuracy: 0.001)
        XCTAssertEqual(at.y, resting.y - MessageTableView.dragHintLift, accuracy: 0.001)
    }

    /// Last resort only: no card centre and no published slot for it either.
    func testTheFingerIsTheLastResort() {
        let finger = CGPoint(x: 95, y: 655)
        let at = MessageTableView.dragHintPosition(cardCentre: nil, restingCentre: nil,
                                                   finger: finger, origin: .zero)
        XCTAssertEqual(at.x, finger.x, accuracy: 0.001)
        XCTAssertEqual(at.y, finger.y - MessageTableView.dragHintLift, accuracy: 0.001)
    }

    /// Dragging a card up to the deck/discard corners would push a 52pt lift off
    /// the top of the board. An unreadable verb is the same bug pointing the
    /// other way, so the pill flips BELOW the card - which (unlike clamping it
    /// to the ceiling, screenshotted mid-fix) also never parks it on top of the
    /// card it is describing.
    func testThePillFlipsBelowTheCardRatherThanClipOffTheTop() {
        let card = CGPoint(x: 100, y: 40)
        let at = MessageTableView.dragHintPosition(cardCentre: card, restingCentre: nil,
                                                   finger: card, origin: .zero)
        XCTAssertEqual(at.y, card.y + MessageTableView.dragHintLift, accuracy: 0.001)
        XCTAssertGreaterThan(at.y, MessageTableView.dragHintMinY, "…and stays on screen")
        XCTAssertGreaterThan(abs(at.y - card.y), 36,
                             "…clear of the 72pt card either way, never drawn over it")
    }

    /// The flip is a LAST resort: one point of room above and the pill still
    /// goes above, so it doesn't hop sides while you drag through the boundary
    /// any earlier than it must.
    func testThePillStaysAboveWhileThereIsAnyRoom() {
        let y = MessageTableView.dragHintMinY + MessageTableView.dragHintLift
        let at = MessageTableView.dragHintPosition(cardCentre: CGPoint(x: 100, y: y),
                                                   restingCentre: nil,
                                                   finger: CGPoint(x: 130, y: y + 30), origin: .zero)
        XCTAssertEqual(at.y, MessageTableView.dragHintMinY, accuracy: 0.001)
    }

    /// The anchor arrives in `boardSpace` while `.position` is local to the
    /// board's own GeometryReader. Both axes must be rebased, or the pill sits
    /// off by however far the reader is inset (14pt of top padding today).
    func testTheAnchorIsRebasedOutOfBoardSpaceOnBothAxes() {
        let origin = CGPoint(x: 8, y: 14)
        let at = MessageTableView.dragHintPosition(cardCentre: CGPoint(x: 140, y: 500),
                                                   restingCentre: nil,
                                                   finger: CGPoint(x: 140, y: 500), origin: origin)
        XCTAssertEqual(at.x, 140 - origin.x, accuracy: 0.001)
        XCTAssertEqual(at.y, 500 - origin.y - MessageTableView.dragHintLift, accuracy: 0.001)
    }

    // MARK: bug 13 — a played card leaves from where the finger let go

    private func card(_ s: Int, _ v: Int) -> Card { Card(s: s, v: v) }

    /// The bug: the dragged card's flight used to start at its hand slot. It must
    /// start centred on the release point instead — and the flight layer
    /// positions by the rect's CENTRE, so that is what this pins.
    func testTheDraggedCardLeavesFromTheReleasePoint() throws {
        let c = card(0, 10)
        let slot = CGRect(x: 40, y: 600, width: 44, height: 72)
        let release = CGPoint(x: 180, y: 300)
        let rects = MessageTableView.playSourceRects(cards: [c], handRects: [c.identity: slot],
                                                     released: (c.identity, release))
        let from = try XCTUnwrap(rects[c.identity])
        XCTAssertEqual(from.midX, release.x, accuracy: 0.001)
        XCTAssertEqual(from.midY, release.y, accuracy: 0.001)
        XCTAssertNotEqual(from.midY, slot.midY, "…and NOT from its resting slot in the hand")
    }

    /// A multi-card play: only the card the finger actually held moved. The rest
    /// of the selection is still sitting in the fan and must fly from there, or
    /// two cards would leave from the same point and read as one.
    func testOnlyTheHeldCardMovesTheRestKeepTheirSlots() throws {
        let held = card(1, 11), other = card(2, 11)
        let heldSlot = CGRect(x: 40, y: 600, width: 44, height: 72)
        let otherSlot = CGRect(x: 90, y: 600, width: 44, height: 72)
        let rects = MessageTableView.playSourceRects(
            cards: [held, other],
            handRects: [held.identity: heldSlot, other.identity: otherSlot],
            released: (held.identity, CGPoint(x: 180, y: 300)))
        XCTAssertEqual(try XCTUnwrap(rects[held.identity]).midY, 300, accuracy: 0.001)
        XCTAssertEqual(rects[other.identity], otherSlot)
    }

    /// A tap/button play never had a release point and must be exactly what it
    /// always was: every card out of its own hand slot.
    func testATapPlayStillLeavesFromTheHand() {
        let c = card(3, 12)
        let slot = CGRect(x: 40, y: 600, width: 44, height: 72)
        let rects = MessageTableView.playSourceRects(cards: [c], handRects: [c.identity: slot],
                                                     released: nil)
        XCTAssertEqual(rects, [c.identity: slot])
    }

    /// Cards outside this play are never carried along (the hand's frames are
    /// published for the WHOLE hand), and a release naming a card that isn't in
    /// the play cannot inject a rect for it.
    func testOnlyThePlayedCardsGetASourceRect() throws {
        let played = card(0, 6), idle = card(0, 7)
        let rects = MessageTableView.playSourceRects(
            cards: [played],
            handRects: [played.identity: .init(x: 0, y: 0, width: 44, height: 72),
                        idle.identity: .init(x: 50, y: 0, width: 44, height: 72)],
            released: (idle.identity, CGPoint(x: 999, y: 999)))
        XCTAssertEqual(Set(rects.keys), [played.identity])
        XCTAssertEqual(try XCTUnwrap(rects[played.identity]).midX, 22, accuracy: 0.001,
                       "the played card keeps its own slot when the release was some other card")
    }
}
