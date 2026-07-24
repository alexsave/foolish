// Round5BoardTests — the round-5 board findings that reduce to pure, static
// math, pinned the same way CoverTiltTests pins the tilt sequence: no
// simulator, no snapshot, just the functions the view actually calls.
//
// M6 ("a big hand compresses to slivers... maybe add a second row?") and its
// M5b sibling ("in the collapsed view, show only the top half of the cards")
// both live entirely in `FHandFan.rowCount`/`FHandFan.height` — the same
// arithmetic `body` uses to lay itself out and `MessageTableView` uses to lift
// the buttons that float above the hand. Nothing about drag gestures, taps, or
// SwiftUI state is exercised here (that needs a live view); what's pinned is
// the row-split THRESHOLD and the height it produces, so a future tweak to
// either can't silently stop agreeing with itself.
import XCTest
@testable import FoolishKit

final class Round5BoardTests: XCTestCase {

    /// `n` distinct cards — only the COUNT matters to `rowCount`/`height`, but
    /// distinct identities keep this honest against any future change that
    /// starts caring about which cards they are.
    private func hand(_ n: Int) -> [Card] {
        (0..<n).map { Card(s: $0 % 4, v: ($0 % 12) + 1) }
    }

    // MARK: M6 — the row-split threshold

    /// A small hand at an ordinary board width stays one row. 6 cards is a
    /// common mid-game hand size and must never wrap just because the hand
    /// grew past a fresh deal.
    func testSixCardsFitOneRow() {
        XCTAssertEqual(FHandFan.rowCount(cards: hand(6), availableWidth: 340), 1)
    }

    /// M6's own worst case: "Durak routinely leaves a defender holding 15-20
    /// cards after two pickups" — exactly the range the finding names, and
    /// exactly where the pre-M6 math put a card below Apple's 44pt minimum.
    func testFifteenToTwentyCardsSplitIntoTwoRows() {
        for n in 15...20 {
            XCTAssertEqual(FHandFan.rowCount(cards: hand(n), availableWidth: 340), 2,
                           "\(n) cards at an ordinary board width must split into two rows")
        }
    }

    /// The threshold is a per-card-width cutoff, not a card-count cutoff — so
    /// it must have an exact boundary at a fixed width. At 340pt available,
    /// 8 cards land a hair AT-OR-ABOVE the ~34pt floor (38pt) and 9 cards a
    /// hair below it (33.3pt); pinning both sides of that boundary is what
    /// keeps this a width-driven decision instead of a width-independent one
    /// that happens to look right at the counts the finding mentions.
    func testRowSplitThresholdIsAPerCardWidthBoundaryNotACardCount() {
        XCTAssertEqual(FHandFan.rowCount(cards: hand(8), availableWidth: 340), 1,
                       "8 cards at 340pt still clears the two-row threshold (38pt/card)")
        XCTAssertEqual(FHandFan.rowCount(cards: hand(9), availableWidth: 340), 2,
                       "9 cards at 340pt falls just under it (33.3pt/card)")
    }

    /// A 0- or 1-card hand is always one row, no matter how degenerate the
    /// width is — there is nothing to split, and `rowGroups` (body's actual
    /// layout) relies on `rowCount` agreeing with that or it would report a
    /// two-row height for a board that only ever renders one (empty) row.
    func testDegenerateHandsNeverSplit() {
        XCTAssertEqual(FHandFan.rowCount(cards: [], availableWidth: 0), 1)
        XCTAssertEqual(FHandFan.rowCount(cards: hand(1), availableWidth: 0), 1)
    }

    // MARK: M6 — the height MessageTableView reserves for the hand

    /// One row's height does not depend on how many cards are IN that row
    /// (only whether it's one row or two) — a 6-card hand and a 20-card hand
    /// that both happen to render one/two rows respectively at the SAME width
    /// must report exactly the two-vs-one relationship, not something that
    /// drifts with count.
    func testHeightDependsOnRowCountNotCardCount() {
        let sixCardHeight = FHandFan.height(cards: hand(6), availableWidth: 340)
        let fifteenCardHeight = FHandFan.height(cards: hand(15), availableWidth: 340)
        let twentyCardHeight = FHandFan.height(cards: hand(20), availableWidth: 340)
        XCTAssertEqual(fifteenCardHeight, twentyCardHeight,
                       "two hands that both split into two rows must reserve the same height")
        XCTAssertGreaterThan(fifteenCardHeight, sixCardHeight,
                             "a two-row hand must reserve more height than a one-row hand")
    }

    /// M5b's crop is a fixed fraction (roughly half) of a row's height,
    /// applied per row — so it must shrink a one-row hand AND still preserve
    /// the two-row relationship (a compact two-row hand is still taller than
    /// a compact one-row hand).
    func testTopHalfOnlyShrinksHeightButPreservesTheRowRelationship() {
        let full = FHandFan.height(cards: hand(6), availableWidth: 340, topHalfOnly: false)
        let compact = FHandFan.height(cards: hand(6), availableWidth: 340, topHalfOnly: true)
        XCTAssertLessThan(compact, full,
                          "the compact drawer's one-row hand must be shorter than the full board's")
        XCTAssertGreaterThan(compact, 0)

        let compactOneRow = FHandFan.height(cards: hand(6), availableWidth: 340, topHalfOnly: true)
        let compactTwoRow = FHandFan.height(cards: hand(15), availableWidth: 340, topHalfOnly: true)
        XCTAssertGreaterThan(compactTwoRow, compactOneRow,
                             "a two-row hand is still taller than a one-row hand once both are cropped")
    }

    /// The exact numbers `MessageTableView`'s `handLift` depends on: pinned so
    /// a future tweak to `cardH`/`rowGap` is a deliberate, visible change here
    /// rather than a silent drift between this file and that one's padding math.
    func testExactHeightsAtAKnownWidth() {
        XCTAssertEqual(FHandFan.height(cards: hand(6), availableWidth: 340, topHalfOnly: false), 80)
        XCTAssertEqual(FHandFan.height(cards: hand(6), availableWidth: 340, topHalfOnly: true), 44)
        XCTAssertEqual(FHandFan.height(cards: hand(15), availableWidth: 340, topHalfOnly: false), 166)
        XCTAssertEqual(FHandFan.height(cards: hand(15), availableWidth: 340, topHalfOnly: true), 94)
    }

    /// `MessageTableView.boardContent` computes its `handLift` as the
    /// difference between the ACTUAL hand's height and an empty hand's height
    /// at the same width — this is the guarantee that subtraction relies on:
    /// an empty hand is always exactly one (uncropped-count) row.
    func testEmptyHandIsAlwaysOneRowBaseline() {
        XCTAssertEqual(FHandFan.height(cards: [], availableWidth: 340),
                       FHandFan.height(cards: hand(6), availableWidth: 340),
                       "an empty hand and any other one-row hand must reserve the same height")
        XCTAssertNotEqual(FHandFan.height(cards: [], availableWidth: 340),
                          FHandFan.height(cards: hand(15), availableWidth: 340),
                          "…but must be shorter than a hand that actually splits into two rows")
    }

    // MARK: slotRects — the analytical landing slot a flight targets (round-7)

    /// Two cards in a 300pt row, uncropped: centred, 52pt wide, 72 tall, sitting
    /// 4pt down (the 8pt of row padding, halved by the centred VStack). These are
    /// the exact numbers `body` renders, so a flight aimed at `slotRects` lands
    /// dead on the real card - which is the whole point (no mid-slide bunch).
    func testSlotRectsTwoCardsAreCentredAndExact() {
        let cards = hand(2)
        let r = FHandFan.slotRects(cards: cards, width: 300, crop: 0)
        XCTAssertEqual(r.count, 2)
        let a = r[cards[0].identity]!, b = r[cards[1].identity]!
        XCTAssertEqual(a, CGRect(x: 96, y: 4, width: 52, height: 72))
        XCTAssertEqual(b, CGRect(x: 152, y: 4, width: 52, height: 72))
        // Symmetric about the container centre.
        XCTAssertEqual((a.midX + b.midX) / 2, 150, accuracy: 0.01)
    }

    /// One card is centred horizontally in the container.
    func testSlotRectsSingleCardCentred() {
        let cards = hand(1)
        let r = FHandFan.slotRects(cards: cards, width: 300, crop: 0)[cards[0].identity]!
        XCTAssertEqual(r.midX, 150, accuracy: 0.01)
        XCTAssertEqual(r.height, 72)
    }

    /// A hand that splits into two rows: the FIRST row gets the ceil, both rows
    /// are vertically stacked (row 1 sits a full row+gap below row 0), and every
    /// card gets a slot. The container height matches `FHandFan.height` exactly.
    func testSlotRectsTwoRowsStackAndCoverEveryCard() {
        // 15 DISTINCT cards (hand(_) above repeats identities past 12, which the
        // slot dict would dedupe - fine for count-only tests, wrong here).
        let cards = (0..<15).map { Card(s: $0 / 13, v: $0 % 13 + 1) }
        let width: CGFloat = 340
        let r = FHandFan.slotRects(cards: cards, width: width, crop: 0)
        XCTAssertEqual(r.count, 15, "every card has a slot")
        // 15 cards -> 8 up top, 7 below (ceil on odd). The 9th card (first of the
        // bottom row) sits a full card-height + rowGap below the 1st.
        let topY = r[cards[0].identity]!.minY
        let botY = r[cards[8].identity]!.minY
        XCTAssertGreaterThan(botY - topY, 72, "the second row is a full row below the first")
        // Slots stay inside the reserved container height.
        let h = FHandFan.height(cards: cards, availableWidth: width, crop: 0)
        for c in cards {
            let rect = r[c.identity]!
            XCTAssertGreaterThanOrEqual(rect.minY, 0)
            XCTAssertLessThanOrEqual(rect.maxY, h + 0.01, "slot fits within the reserved height")
        }
    }

    /// Cropping (the compact drawer) shortens each slot the same way the fan
    /// crops its cards, so a flight into a collapsed hand still lands true.
    func testSlotRectsShrinkUnderCrop() {
        let cards = hand(3)
        let full = FHandFan.slotRects(cards: cards, width: 300, crop: 0)[cards[0].identity]!
        let crop = FHandFan.slotRects(cards: cards, width: 300, crop: 1)[cards[0].identity]!
        XCTAssertEqual(full.height, 72)
        XCTAssertEqual(crop.height, 36, "top-half crop halves the slot height")
    }
}
