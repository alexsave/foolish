// Round-16: dragging a card between the two rows of a split hand.
//
// The owner: "If there are two rows, you can't drag to rearrange between them.
// Fix. Yes this will mean if we drag a card from the bottom to the top row, it
// will bump the right most card in the top row to the bottom row. And if we
// move a card from the top to the bottom row, the left most card will be bumped
// to the top row."
//
// Round-5 clamped the reorder target into the dragged card's own row, so the
// gesture simply did nothing across the boundary. That clamp lived inside the
// view's drag handler, which is why the decision has been lifted out to
// `FHandFan.splice` and why these tests drive THAT: a test that re-implemented
// the arithmetic beside the view would pass just as happily against a view that
// still clamps.
//
// The bump is asserted as a bump - "which row is this card in, before and
// after" - rather than as an index, because an index test would also pass for a
// rearrangement that shuffled the whole hand. Exactly two cards may change row:
// yours, and the one it displaced.
import XCTest
@testable import FoolishKit

final class HandRowDragTests: XCTestCase {

    /// 15 DISTINCT cards - enough to split at 340pt into 7 up top and 8 below.
    private func hand(_ n: Int) -> [Card] { (0..<n).map { Card(s: $0 / 13, v: $0 % 13 + 1) } }
    private let width: CGFloat = 340

    private func slots(_ n: Int) -> [CGRect] {
        FHandFan.slotFrames(count: n, width: width, crop: 0)
    }
    private func centre(_ slot: CGRect) -> CGPoint { CGPoint(x: slot.midX, y: slot.midY) }

    /// Which row each id sits in, by the same cut the fan draws.
    private func rowOf(_ order: [String]) -> [String: Int] {
        let sizes = FHandFan.rowSizes(count: order.count, availableWidth: width)
        var out: [String: Int] = [:]
        var i = 0
        for (r, n) in sizes.enumerated() {
            for _ in 0..<n { out[order[i]] = r; i += 1 }
        }
        return out
    }

    // MARK: - the two rows

    /// A 15-card hand really is two rows, 7 over 8 - the premise everything
    /// below rests on. If this ever stops holding, the tests that follow would
    /// be quietly asserting one-row behavior and passing for the wrong reason.
    ///
    /// The odd card sits in the BOTTOM row (owner: "If we have 11 cards, do 5 up
    /// top and 6 below"), so the cut is at floor(n/2) and index 7 opens the
    /// second row rather than closing the first.
    func testTheFixtureHandActuallySplits() {
        XCTAssertEqual(FHandFan.rowSizes(count: 15, availableWidth: width), [7, 8])
        XCTAssertEqual(slots(15).count, 15)
        XCTAssertEqual(slots(15)[0].midY, slots(15)[6].midY, "row 0 is one row")
        XCTAssertGreaterThan(slots(15)[7].midY, slots(15)[6].midY, "row 1 sits below it")
    }

    /// THE OWNER'S OWN NUMBER, asserted directly rather than only through the
    /// 15-card fixture: eleven cards are five up top and six below. A test that
    /// checked only `[7, 8]` would pass just as well against a ceil cut applied
    /// to an even count somewhere else, so the odd case gets its own line.
    func testElevenCardsAreFiveOverSix() {
        XCTAssertEqual(FHandFan.rowSizes(count: 11, availableWidth: width), [5, 6])
        let s = slots(11)
        XCTAssertEqual(s[0].midY, s[4].midY, "five cards share the top row")
        XCTAssertGreaterThan(s[5].midY, s[4].midY, "the sixth opens the bottom row")
        XCTAssertEqual(s[5].midY, s[10].midY, "six cards share the bottom row")
        // ONE card width for both rows, sized off the FULLER row - the bottom
        // one now. Sized off row 0 (five) instead, the six-card row would be
        // wider than the container.
        XCTAssertEqual(s[0].width, s[10].width, accuracy: 0.01)
        XCTAssertLessThanOrEqual(s[10].maxX, width + 0.01, "the fuller row overflowed")
    }

    /// THE BUG. A bottom-row card dragged over the top row goes there. Before
    /// the fix `splice` had no say in it at all: the view clamped the target
    /// back into the row the card started in, so this returned the card to
    /// exactly where it was and nothing moved.
    func testABottomRowCardDraggedOverTheTopRowGoesThere() {
        let ids = hand(15).map(\.identity)
        let s = slots(15)
        let moved = try! XCTUnwrap(
            FHandFan.splice(order: ids, dragged: ids[9], centre: centre(s[1]), slots: s),
            "a bottom-row card hovering the top row asked for nothing")
        XCTAssertEqual(moved.to, 1)
        XCTAssertEqual(moved.order.firstIndex(of: ids[9]), 1)
        XCTAssertEqual(rowOf(moved.order)[ids[9]], 0, "it did not end up in the top row")
    }

    /// …and the card it displaced comes down: the top row's RIGHTMOST becomes
    /// the bottom row's leftmost. One card moves, not the whole hand.
    func testDraggingUpBumpsTheTopRowsRightmostCardDown() {
        let ids = hand(15).map(\.identity)
        let s = slots(15)
        let before = rowOf(ids)
        let moved = try! XCTUnwrap(
            FHandFan.splice(order: ids, dragged: ids[9], centre: centre(s[1]), slots: s))
        let after = rowOf(moved.order)

        // ids[6] was the last card of the top row; it is now the first of the bottom.
        XCTAssertEqual(before[ids[6]], 0)
        XCTAssertEqual(after[ids[6]], 1, "the top row's rightmost card did not come down")
        XCTAssertEqual(moved.order.firstIndex(of: ids[6]), 7, "…and it is the head of the bottom row")

        let changed = ids.filter { before[$0] != after[$0] }
        XCTAssertEqual(Set(changed), Set([ids[9], ids[6]]),
                       "exactly the dragged card and the one it bumped may change row")
    }

    /// The mirror image: a top-row card dragged down bumps the bottom row's
    /// LEFTMOST up to the end of the top row.
    func testDraggingDownBumpsTheBottomRowsLeftmostCardUp() {
        let ids = hand(15).map(\.identity)
        let s = slots(15)
        let before = rowOf(ids)
        let moved = try! XCTUnwrap(
            FHandFan.splice(order: ids, dragged: ids[1], centre: centre(s[10]), slots: s))
        let after = rowOf(moved.order)

        XCTAssertEqual(after[ids[1]], 1, "the dragged card did not reach the bottom row")
        XCTAssertEqual(before[ids[7]], 1)
        XCTAssertEqual(after[ids[7]], 0, "the bottom row's leftmost card did not go up")
        XCTAssertEqual(moved.order.firstIndex(of: ids[7]), 6, "…to the tail of the top row")

        let changed = ids.filter { before[$0] != after[$0] }
        XCTAssertEqual(Set(changed), Set([ids[1], ids[7]]))
    }

    /// NO OSCILLATION. The card lands in the slot it asked for, so asking again
    /// from the same place answers "stay". Without this the fan would splice on
    /// every frame of a held finger and the whole hand would churn - which is
    /// the failure mode of hit-testing the FINGER (its offset inside the card
    /// never goes away) instead of the card's own centre.
    func testOnceItLandsItStopsAsking() {
        let ids = hand(15).map(\.identity)
        let s = slots(15)
        let target = centre(s[1])
        let moved = try! XCTUnwrap(
            FHandFan.splice(order: ids, dragged: ids[9], centre: target, slots: s))
        XCTAssertNil(FHandFan.splice(order: moved.order, dragged: ids[9],
                                     centre: target, slots: s),
                     "the card asked to move again from the slot it just took")
    }

    /// Rows are STICKY, sideways is not. Sliding along a row is as light as it
    /// ever was (half a slot, ~19pt), but changing row asks for a deliberate
    /// lift of about half a card - so a card dragged along the bottom row does
    /// not flick up into the top one on the way past.
    func testChangingRowCostsMoreThanSlidingSideways() {
        let s = slots(15)
        // The TOP row holds one card fewer (the odd card goes below), so it is
        // centred half a slot to the right of the bottom one - which is why this
        // measures the vertical pitch between the rows rather than assuming a
        // card sits directly below another.
        let top = s[3], bottom = s[11]
        XCTAssertEqual(FHandFan.slotIndex(at: centre(top), slots: s), 3)

        let pitch = bottom.midY - top.midY
        let justShort = CGPoint(x: top.midX, y: top.midY + pitch * 0.45)
        let justPast  = CGPoint(x: top.midX, y: top.midY + pitch * 0.55)
        XCTAssertEqual(FHandFan.slotIndex(at: justShort, slots: s), 3,
                       "a lift that never reached halfway changed row anyway")
        XCTAssertNotEqual(FHandFan.slotIndex(at: justPast, slots: s), 3,
                          "a lift past halfway did not change row")
        XCTAssertGreaterThan(pitch / 2, (s[4].midX - s[3].midX) / 2,
                             "changing row must cost more than sliding one slot along")
    }

    // MARK: - the single row, unchanged

    /// The one-row hand the owner is happy with. Its geometry is untouched:
    /// every slot on one line, evenly spaced, the row centred in the container.
    func testASingleRowHandLaysOutExactlyAsItDid() {
        let s = slots(6)
        XCTAssertEqual(FHandFan.rowSizes(count: 6, availableWidth: width), [6])
        XCTAssertEqual(Set(s.map(\.midY)).count, 1, "a one-row hand grew a second row")
        let steps = Set(zip(s, s.dropFirst()).map { ($1.minX - $0.minX).rounded() })
        XCTAssertEqual(steps.count, 1, "slots are evenly spaced")
        XCTAssertEqual((s.first!.minX + s.last!.maxX) / 2, width / 2, accuracy: 0.01,
                       "the row is centred")
    }

    /// …and it reorders the way it always has: the card goes to the slot it is
    /// over, everything between shuffles up by one, nothing else moves.
    func testASingleRowHandStillReordersAlongItself() {
        let ids = hand(6).map(\.identity)
        let s = slots(6)
        let moved = try! XCTUnwrap(
            FHandFan.splice(order: ids, dragged: ids[0], centre: centre(s[3]), slots: s))
        XCTAssertEqual(moved.to, 3)
        XCTAssertEqual(moved.order, [ids[1], ids[2], ids[3], ids[0], ids[4], ids[5]])

        // Dragging back the other way is the same rule mirrored.
        let back = try! XCTUnwrap(
            FHandFan.splice(order: ids, dragged: ids[4], centre: centre(s[1]), slots: s))
        XCTAssertEqual(back.order, [ids[0], ids[4], ids[1], ids[2], ids[3], ids[5]])
    }

    /// A one-row hand cannot be dragged into a row that isn't there, no matter
    /// how far up or down the finger goes - the y that now matters for a split
    /// hand must be inert for a clean one. This is the "single row works the
    /// same" guarantee stated as a property rather than a hope.
    func testInASingleRowVerticalMovementChangesNothing() {
        let ids = hand(6).map(\.identity)
        let s = slots(6)
        for dy in [-400.0, -40.0, 0.0, 40.0, 400.0] as [CGFloat] {
            let p = CGPoint(x: s[2].midX, y: s[2].midY + dy)
            XCTAssertEqual(FHandFan.slotIndex(at: p, slots: s), 2,
                           "a one-row hand answered differently \(dy)pt off its line")
            XCTAssertNil(FHandFan.splice(order: ids, dragged: ids[2], centre: p, slots: s),
                         "a card sitting still moved because the finger drifted vertically")
        }
    }

    /// Half a slot is still the trigger along a row - the same feel as before,
    /// and independent of where inside the card you grabbed it (the reorder
    /// tracks the card, not the fingertip).
    func testASlideSwapsAtTheHalfwayPoint() {
        let ids = hand(6).map(\.identity)
        let s = slots(6)
        let step = s[3].midX - s[2].midX
        let justShort = CGPoint(x: s[2].midX + step * 0.45, y: s[2].midY)
        let justPast  = CGPoint(x: s[2].midX + step * 0.55, y: s[2].midY)
        XCTAssertNil(FHandFan.splice(order: ids, dragged: ids[2], centre: justShort, slots: s))
        XCTAssertEqual(try! XCTUnwrap(
            FHandFan.splice(order: ids, dragged: ids[2], centre: justPast, slots: s)).to, 3)
    }

    // MARK: - edges

    /// A card that holds no slot yet (round-6 bug 10: a deal whose flight has
    /// not started) is invisible to the drag and keeps its place in the order.
    /// It has nowhere on screen to be dropped, so a reorder must not be able to
    /// move it - and must not miscount slots because of it either.
    /// The deferred card sits in the MIDDLE of the order here, not at the end.
    /// A freshly dealt card usually is last, and a fixture that put it there
    /// would pass just as well against an implementation that dropped the
    /// deferred ids and re-appended them - which is a different rule that
    /// happens to agree in the easy case.
    func testADealStillInFlightKeepsItsPlace() {
        let ids = hand(7).map(\.identity)          // 6 laid out + 1 deferred
        let s = slots(6)
        let moved = try! XCTUnwrap(
            FHandFan.splice(order: ids, deferred: [ids[3]], dragged: ids[0],
                            centre: centre(s[2]), slots: s))
        XCTAssertEqual(moved.order.count, 7)
        XCTAssertEqual(moved.order.firstIndex(of: ids[3]), 3,
                       "the in-flight card was shifted out of its place")
        XCTAssertEqual(moved.order, [ids[1], ids[2], ids[0], ids[3], ids[4], ids[5], ids[6]])
    }

    /// Nonsense in, nothing out: a slot list that does not match the hand (a
    /// frame caught mid-deal) must decline rather than splice against the wrong
    /// geometry, and a card that is not in the hand at all cannot move it.
    func testAMismatchedHandDeclines() {
        let ids = hand(15).map(\.identity)
        XCTAssertNil(FHandFan.splice(order: ids, dragged: ids[0],
                                     centre: .zero, slots: slots(14)))
        XCTAssertNil(FHandFan.splice(order: ids, dragged: "not-a-card",
                                     centre: .zero, slots: slots(15)))
        XCTAssertNil(FHandFan.slotIndex(at: .zero, slots: []))
    }
}

