// LayoutTests.swift - the Swift face of pk_lay.c: the hand overflow
// thresholds (O4, U7, U8) and the pill slots (U9) as the screens read them.
// The C side pins the same numbers in pk_api_smoke.c; these prove the
// bridge carries them (a wrong enum mapping, a lost slot) into what is drawn.

import CPickemup
import SwiftUI
import XCTest
@testable import PickemupKit

final class LayoutTests: XCTestCase {
    private let w: CGFloat = 358          // a 390pt phone's hand width

    // MUTATE: PkLayout.hand maps PK_LAY_OVERLAP to .flat -> "twenty-seven overlap".
    // MUTATE: PkLayout.hand maps PK_LAY_SCROLL to .overlap -> "forty-two scroll".
    func testTheHandOverflowsAsO4Says() {
        XCTAssertEqual(PkLayout.hand(count: 7, width: w, maxRows: 2).mode, .flat)
        XCTAssertEqual(PkLayout.hand(count: 7, width: w, maxRows: 2).rows, 1)
        XCTAssertEqual(PkLayout.hand(count: 10, width: w, maxRows: 2).rows, 2, "ten: two rows")
        XCTAssertEqual(PkLayout.hand(count: 10, width: w, maxRows: 2).boxHeight, 166)
        XCTAssertEqual(PkLayout.hand(count: 27, width: w, maxRows: 2).mode, .overlap, "twenty-seven overlap")
        XCTAssertEqual(PkLayout.hand(count: 42, width: w, maxRows: 2).mode, .scroll, "forty-two scroll")
        XCTAssertGreaterThan(PkLayout.hand(count: 42, width: w, maxRows: 2).contentWidth, w)
    }

    // MUTATE: PkLayout.maxRows returns 2 -> "the drawer keeps one row".
    func testTheDrawerKeepsOneRow() {
        XCTAssertEqual(PkLayout.maxRows(viewHeight: 340), 1, "the drawer keeps one row")
        XCTAssertEqual(PkLayout.maxRows(viewHeight: 718), 2)
        let h = PkLayout.hand(count: 14, width: w, maxRows: PkLayout.maxRows(viewHeight: 340))
        XCTAssertEqual(h.mode, .overlap)
        XCTAssertEqual(h.rows, 1)
        XCTAssertEqual(PkLayout.hand(count: 30, width: w, maxRows: 1).mode, .scroll)
    }

    // MUTATE: the slot loop stops one short (0..<n-1) -> "a slot per card".
    // MUTATE: Hand.thin becomes `cardW <= thinBelow` -> "an overlapped card keeps its face".
    func testEverySlotIsInsideTheRowAndFacesAreRight() {
        for n in [1, 5, 9, 13, 26, 40] {
            let h = PkLayout.hand(count: n, width: w, maxRows: 2)
            XCTAssertEqual(h.slots.count, n, "a slot per card")
            for s in h.slots {
                XCTAssertGreaterThanOrEqual(s.minX, 0, "n=\(n)")
                XCTAssertLessThanOrEqual(s.maxX, w + 0.01, "n=\(n)")
                XCTAssertLessThanOrEqual(s.maxY, h.boxHeight + 0.01, "n=\(n)")
            }
        }
        XCTAssertTrue(PkLayout.hand(count: 12, width: w, maxRows: 1).thin, "a flat card under 40 goes thin")
        XCTAssertFalse(PkLayout.hand(count: 30, width: w, maxRows: 2).thin, "an overlapped card keeps its face (U8)")
    }

    // MUTATE: PkLayout.pill maps PK_PILL_PASS to .undo -> "Pass beside Draw".
    func testThePillSlots() {
        let drew = PkLayout.pills(canDraw: true, myTurn: true, selected: false, canPass: true, canUndo: false)
        XCTAssertEqual(drew.trailing, .draw)
        XCTAssertEqual(drew.leading, .pass, "Pass beside Draw")
        let picked = PkLayout.pills(canDraw: true, myTurn: true, selected: true, canPass: true, canUndo: false)
        XCTAssertEqual(picked.leading, .play)
        let staged = PkLayout.pills(canDraw: false, myTurn: true, selected: false, canPass: false, canUndo: true)
        XCTAssertEqual(staged.trailing, .undo)
        XCTAssertEqual(staged.leading, .none)
    }

    // MUTATE: PkLayout.seat passes `seat` as `me` -> "the seat across is at the top".
    func testTheRingPutsMeAtTheBottom() {
        let board = CGSize(width: 374, height: 700)
        let mine = PkLayout.seat(2, me: 2, count: 4, board: board, collapse: 0)
        XCTAssertEqual(mine.x, 187, accuracy: 0.1)
        XCTAssertGreaterThan(mine.y, 350, "my seat is at the bottom")
        let across = PkLayout.seat(0, me: 2, count: 4, board: board, collapse: 0)
        XCTAssertLessThan(across.y, 350, "the seat across is at the top")
    }
}

final class CardFaceTests: XCTestCase {
    // MUTATE: CardFace reads pk_api_card_rank for the suit -> "circle one".
    func testFacesComeFromTheKernel() throws {
        let one = try XCTUnwrap(CardFace(0))
        XCTAssertEqual(one.suit, 0, "circle one")
        XCTAssertEqual(one.rank, 1)
        XCTAssertEqual(one.label, "1")
        let wild4 = try XCTUnwrap(CardFace(100))
        XCTAssertTrue(wild4.isWild)
        XCTAssertEqual(wild4.label, "+4")
        XCTAssertNil(CardFace(PK_CARD_HIDDEN), "a hidden card is a back")
    }
}

/// O6: an action card says the shape its corners print, through the same
/// `cornerSuit` that draws them, so colour is never the only way to its suit.
@MainActor
final class ActionCardCornerTests: XCTestCase {
    // MUTATE: CardFace.cornerSuit answers nil for Skip -> "a skip on squares carries the square".
    func testAnActionCardExposesItsSuitShape() throws {
        let faces = (0..<256).compactMap { CardFace($0) }
        let skip = try XCTUnwrap(faces.first { $0.rank == PK_R_SKIP && $0.suit == 2 })
        let three = try XCTUnwrap(faces.first { $0.rank == 3 && $0.suit == 2 })
        let square = Pk.string("SUIT_ONE_2")
        XCTAssertFalse(square.isEmpty, "the kernel names the shape")

        let skipEl = try XCTUnwrap(hostedCard(skip.id))
        XCTAssertEqual(skipEl.accessibilityValue, square, "a skip on squares carries the square")
        XCTAssertEqual(skipEl.accessibilityLabel, Pk.words(PK_API_W_CARD, skip.id), "the card by the kernel's word")

        let threeEl = try XCTUnwrap(hostedCard(three.id))
        XCTAssertEqual(threeEl.accessibilityValue ?? "", "", "a number card's shape is its centre glyph, no corner mark")
    }

    /// The one accessibility element a pile-sized card presents, hosted in a window.
    private func hostedCard(_ id: Int) -> NSObject? {
        AXTree.hosted(PkCard(card: id, size: CGSize(width: 82, height: 115), fullFace: true),
                      size: CGSize(width: 200, height: 200)) { AXTree.elements($0).first }
    }
}

@MainActor
final class RenderTests: XCTestCase {
    // MUTATE: BubbleSnapshot.size becomes 300 x 300 -> "the bubble is 300 x 195".
    func testTheBubbleRendersAt300By195() throws {
        XCTAssertNotNil(Phones.dmStartedByBo())
        let img = try XCTUnwrap(BubbleSnapshot.render(scheme: .light))
        XCTAssertEqual(img.size.width, 300, accuracy: 0.5, "the bubble is 300 x 195")
        XCTAssertEqual(img.size.height, 195, accuracy: 0.5, "the bubble is 300 x 195")
    }
}
