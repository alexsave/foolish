// ArrangeTests.swift - the hand's drag-to-reorder (ORCHESTRATION O9,
// IOS_DECISIONS I38): the model asks the kernel to move a card, and the next
// view reads the new order; a play and a draw after it land where O9 says.
//
// Each test names the mutation it must go red on (MUTATE:), listed with the
// rest in pickemup/ios/TESTS_MUTATED.md.

import CPickemup
import XCTest
@testable import PickemupKit

@MainActor
final class ArrangeTests: XCTestCase {

    /// The cards of the hand in the order they are drawn on screen.
    private func drawn(_ m: TableModel) -> [Int] {
        let s = m.shown(nil)
        var out = [Int](repeating: -1, count: s.hand.count)
        for (pos, card) in s.hand.enumerated() where pos < s.slot.count && s.slot[pos] < out.count {
            out[s.slot[pos]] = card
        }
        return out
    }

    // MUTATE: TableModel.arrange skips its refresh() -> "the next view reads the new order".
    // MUTATE: TableModel.shown sets s.slot from the hand's indices -> "slot 0's card is drawn last".
    func testAReorderMovesTheCardAndTheNextViewReadsTheNewOrder() {
        XCTAssertNotNil(Phones.dmStartedByBo())
        let m = TableModel()
        let hand = m.hand
        XCTAssertEqual(hand.count, 7)
        XCTAssertEqual(drawn(m), hand, "no drag yet: acquisition order")
        XCTAssertTrue(m.arrange(0, toSlot: 6), "the first card to the right end")
        var want = hand
        want.append(want.removeFirst())
        XCTAssertEqual(drawn(m), want, "the next view reads the new order")
        XCTAssertEqual(m.view?.mySlot.first, 6, "slot 0's card is drawn last")
        XCTAssertEqual(m.hand, hand, "the hand itself is still acquisition order")
        XCTAssertFalse(m.arrange(1, toSlot: m.view?.mySlot[1] ?? 0), "a drag to where it already is moves nothing")
        XCTAssertEqual(Pk.arrangedPos(6), 0, "the kernel maps the slot back to the position")
    }

    // MUTATE: TableModel.arrange passes `pos` as the from slot -> "the card at that slot plays".
    func testAPlayAfterAReorderPlaysTheRightCard() throws {
        var found = false
        for k in 0..<60 {
            Phones.dmStartedByBo(seed: k)
            if let p = Phones.plainPlayable(), p > 0 { found = true; break }
        }
        XCTAssertTrue(found, "a deal where Bo can play a plain card that is not the first")
        let m = TableModel()
        let pos = try XCTUnwrap(Phones.plainPlayable())
        let card = m.hand[pos]
        XCTAssertTrue(m.arrange(0, toSlot: m.hand.count - 1), "a drag first")
        XCTAssertTrue(m.arrange(pos, toSlot: 0), "the playable card to the left end")
        var before = drawn(m)
        XCTAssertEqual(before.first, card, "it is drawn first")
        let atSlot = try XCTUnwrap(Pk.arrangedPos(0))
        XCTAssertEqual(m.hand[atSlot], card, "the card at that slot plays")
        m.play(atSlot)
        XCTAssertEqual(m.view?.top, card, "the pile's top is the dragged card")
        before.removeFirst()
        XCTAssertEqual(drawn(m), before, "the gap closes and the rest keep the dragged order")
    }

    // MUTATE: pk_arr_sync appends a new card at the left (insert_at(a, 0, ...)),
    // library rebuilt -> "the drawn card lands on the right".
    func testADrawAfterAReorderLandsOnTheRight() throws {
        XCTAssertNotNil(Phones.dmStartedByBo())
        let m = TableModel()
        XCTAssertTrue(m.arrange(6, toSlot: 0), "the last card to the left end")
        XCTAssertTrue(m.arrange(2, toSlot: 4), "and another drag")
        let before = drawn(m)
        m.draw()
        let after = drawn(m)
        XCTAssertEqual(after.count, before.count + 1)
        XCTAssertEqual(after.last, m.hand.last, "the drawn card lands on the right")
        XCTAssertEqual(Array(after.dropLast()), before, "the rest keep the dragged order")
    }

    // MUTATE: PkLayout.drop maps PK_DROP_HAND to .pile -> "a release in the row rearranges".
    func testTheDropIsTheKernels() {
        let board = CGSize(width: 360, height: 420)
        let lay = PkLayout.hand(count: 7, width: board.width - PkLayout.handPadding * 2, maxRows: 2)
        let pile = PkLayout.zone(.pileDrop, board: board, collapse: 0, handBox: lay.boxHeight)
        XCTAssertEqual(PkLayout.drop(board: board, collapse: 0, handBox: lay.boxHeight,
                                     at: CGPoint(x: board.width / 2, y: board.height - lay.boxHeight / 2)),
                       .hand, "a release in the row rearranges")
        XCTAssertEqual(PkLayout.drop(board: board, collapse: 0, handBox: lay.boxHeight,
                                     at: CGPoint(x: pile.midX, y: pile.midY)), .pile, "a release on the pile plays")
        XCTAssertEqual(PkLayout.drop(board: board, collapse: 0, handBox: lay.boxHeight, at: CGPoint(x: 4, y: 40)),
                       .none, "the felt does nothing")
        let s3 = lay.slots[3]
        XCTAssertEqual(PkLayout.handNearest(count: 7, width: board.width - PkLayout.handPadding * 2, maxRows: 2,
                                            centre: CGPoint(x: s3.midX, y: s3.midY)), 3, "a card over slot 3 asks for it")
    }
}
