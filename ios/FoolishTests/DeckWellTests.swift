// The deck well, 1.1(55) - the owner's two reports about the stock in the
// board's top-left corner, and the non-regressions the fixes for them must not
// break.
//
//  1. "on a bout ending good, if the flipped card would've been animated in the
//     resulting animation, it DOES NOT SHOW at first in the pile BEFORE the
//     deal animations play. The deck shows, but not the flipped card."
//
//     The stock is one thing drawn in two pieces - a pile of backs and the
//     trump tucked under it - and only the pile went through the count-freeze.
//     The trump was read straight off the committed board, which a refill
//     reaching past the deck has already emptied. So the freeze held the pile
//     at four and drew it standing on nothing.
//
//  2. "the deck visual does not change as it deals? seems to be an identical
//     amount of cards diagonally spaced no matter what the number says."
//
//     `min(deckCount, 6)`: a straight line for the last six cards of the game
//     and a flat line for the twenty-three before them.
//
// MUTATION-CHECKED - see the note above each test for what was broken and how
// many cases noticed.
import XCTest
@testable import FoolishKit

@MainActor
final class DeckWellTests: XCTestCase {

    // MARK: - 1. the trump belongs to the freeze
    //
    // The wire half of this - that `AnimPlan.pre` really carries the pre-move
    // trump, over real games at 2/3/4 players - lives in
    // MessageCountWindingTests, which already owns that sweep and whose header
    // already names the flipped trump as one of the shapes it exists for.
    // What is here is the BOARD half: which of the three sources the well
    // draws from, in which order.

    /// MUTATION-CHECKED: `shownTrump` reading the committed board first
    /// -> 1 failure; a held slot treated as a miss -> 2 failures; the ledger
    /// consulted after the freeze -> 2 failures.
    ///
    /// THE THREE SOURCES, IN ORDER, and that they are the same three the deck
    /// COUNT uses. This is the whole shape of the defect: one value on the well
    /// skipped all of them and read the committed board, which a bout end's
    /// refill has already emptied of its trump.
    func testTheShownTrumpPrefersTheLedgerThenTheFreezeThenTheBoard() {
        let ace = Card(s: 0, v: 13)
        let six = Card(s: 2, v: 6)
        typealias V = MessageTableView
        // The committed board says the trump is gone; the freeze says it is
        // still there. That IS the bug, stated as a value.
        XCTAssertEqual(V.shownTrump(held: nil, frozen: six, committed: .gone), .card(six),
                       "the freeze outranks the committed board")
        // …and a running sequence's ledger outranks the freeze.
        XCTAssertEqual(V.shownTrump(held: .card(ace), frozen: six, committed: .gone), .card(ace),
                       "the ledger outranks the freeze")
        // A held `.airborne` or `.gone` is an ANSWER, not an absence: the step
        // whose flight took the trump out from under the pile writes the first
        // and the landing writes the second, and falling through to the freeze
        // at either would put the card back under the pile.
        XCTAssertEqual(V.shownTrump(held: .airborne, frozen: six, committed: .card(six)), .airborne,
                       "a held airborne is the answer, not a miss")
        XCTAssertEqual(V.shownTrump(held: .gone, frozen: six, committed: .card(six)), .gone,
                       "a held gone is the answer, not a miss")
        // With nothing held, the board is the answer.
        XCTAssertEqual(V.shownTrump(held: nil, frozen: nil, committed: .card(ace)), .card(ace),
                       "at rest the well follows the kernel")
    }

    /// MUTATION-CHECKED: `.airborne` given a `card` -> 1 failure; `.airborne`
    /// made `exists == false` (the glyph up while the card flies) -> 1 failure;
    /// `TrumpSlot.of` dropping the `hasFlipped` gate -> 1 failure.
    ///
    /// THE GLYPH APPEARS AT ARRIVAL, THE PILE THINS AT DEPARTURE, and the slot
    /// is the one place that asymmetry lives. Owner, 1.1(55), reading a frame
    /// of my own before/after strip: "if there still is a flipped card on
    /// board, don't show Trump indicator yet! Only after flipped is gone do you
    /// show it." The trace had it too - in `after_cold.log` the glyph paint
    /// sits BETWEEN `flight START [... 2-5 ...]` and `flight LAND [1-5,2-5]`,
    /// with the trump provably in the air.
    ///
    /// What the well does with each state is asserted through the pair it
    /// actually renders from, because that pair is the API two other boards
    /// use as well.
    func testTheTrumpGlyphWaitsForTheCardToLand() {
        let six = Card(s: 2, v: 6)
        // Under the stock: the card is drawn, the glyph is not.
        XCTAssertEqual(TrumpSlot.card(six).card?.identity, six.identity)
        XCTAssertTrue(TrumpSlot.card(six).exists)
        // In the air: NEITHER. The flight layer is carrying it, so the well
        // must not hold a second copy - and the glyph would be that same card
        // drawn a third time, as its own absence.
        XCTAssertNil(TrumpSlot.airborne.card, "the well draws no card while it flies")
        XCTAssertTrue(TrumpSlot.airborne.exists, "…and no glyph either - it still exists")
        // Landed: the glyph, and only now.
        XCTAssertNil(TrumpSlot.gone.card)
        XCTAssertFalse(TrumpSlot.gone.exists)
    }

    /// MUTATION-CHECKED: `trumpAtDeparture` returning `.gone` (the glyph up as
    /// the card leaves) -> 1 failure; `trumpAtLanding` returning `.airborne`
    /// -> 1 failure.
    ///
    /// THE TWO BEATS, AND WHY THEY DIFFER. The pile thins at departure and the
    /// glyph appears at arrival; the same board read at the two moments must
    /// therefore give two different answers, and this is the only place that is
    /// written down as a value.
    func testTheTrumpLeavesTheWellAtDepartureAndBecomesTheGlyphAtLanding() {
        let six = Card(s: 2, v: 6)
        typealias V = MessageTableView
        let lost = Self.board(hasFlipped: false, flipped: six)   // the step that took it
        let kept = Self.board(hasFlipped: true, flipped: six)    // a step that did not

        XCTAssertEqual(V.trumpAtDeparture(lost), .airborne,
                       "departure takes the card out of the well but does not put the glyph up")
        XCTAssertEqual(V.trumpAtLanding(lost), .gone,
                       "…and the landing is what makes the glyph true")
        // A step that did not take the trump says the same thing at both beats.
        XCTAssertEqual(V.trumpAtDeparture(kept), .card(six))
        XCTAssertEqual(V.trumpAtLanding(kept), .card(six))
    }

    /// `TrumpSlot.of` is the gate every committed board is normalised through:
    /// a board whose trump has been drawn must answer `.gone` rather than the
    /// stale card the kernel still holds in the slot.
    func testAdrawnTrumpIsNotAFace() {
        let ace = Card(s: 0, v: 13)
        XCTAssertEqual(TrumpSlot.of(Self.board(hasFlipped: false, flipped: ace)), .gone,
                       "hasFlipped is the gate")
        XCTAssertEqual(TrumpSlot.of(Self.board(hasFlipped: true, flipped: Card(s: -1, v: -1))), .gone,
                       "a redacted card is not a face")
        XCTAssertEqual(TrumpSlot.of(Self.board(hasFlipped: true, flipped: ace)), .card(ace))
    }

    private static func board(hasFlipped: Bool, flipped: Card?) -> GameView {
        GameView(status: 1, numPlayers: 2, powerSuit: Suit.spades.rawValue,
                 deckCount: 4, discardCount: 0, hasFlipped: hasFlipped,
                 firstAttacker: 0, defender: 1, viewer: 0, goodMask: 0,
                 gameOver: -1, flipped: flipped, battles: [],
                 eliminationOrder: [], players: [])
    }

    // MARK: - 2. the pile's height tracks the count

    /// MUTATION-CHECKED, each on its own:
    ///   `layers(for:)` back to `min(max(n, 0), 6)` (the shipped bug) -> 4 failures
    ///   the exact range widened to 8 (`n <= 8`)                      -> 2 failures
    ///   the curve made non-monotonic (11 -> 8, 12 -> 7)              -> 5 failures
    ///
    /// `n <= 7` is NOT in that list: widening the exact range by exactly one
    /// produces the same function (7 maps to 7 either way), so it is an
    /// equivalent mutant rather than a case this misses.
    func testThePileHeightTracksTheCount() {
        // ONE CARD LOOKS LIKE ONE CARD. The end of the game is when a player
        // counts, so the last six are exact.
        for n in 0...6 {
            XCTAssertEqual(FDeckWell.layers(for: n), n,
                           "a stock of \(n) draws \(n) cards, exactly")
        }
        // A NEGATIVE COUNT IS NOT A NEGATIVE PILE. `shownDeckCount` can hand a
        // boardless step's forward derivation through (anim_plan_test pins a
        // step deriving to -1), and a well is not the place to notice.
        XCTAssertEqual(FDeckWell.layers(for: -3), 0)

        // A FULL STOCK LOOKS THICK, AND A HALF ONE IN BETWEEN. The report was
        // that 23 and 6 draw the same picture; they must not.
        XCTAssertGreaterThan(FDeckWell.layers(for: 23), FDeckWell.layers(for: 11))
        XCTAssertGreaterThan(FDeckWell.layers(for: 11), FDeckWell.layers(for: 6))

        // MONOTONIC over every count a real stock can hold, so the pile can
        // only ever thin as the deck drains - a picture that grew while the
        // number shrank would be the FSeatBadge defect one component over.
        for n in 0..<52 {
            XCTAssertLessThanOrEqual(FDeckWell.layers(for: n), FDeckWell.layers(for: n + 1),
                                     "the curve steps backwards at \(n)")
            XCTAssertLessThanOrEqual(FDeckWell.layers(for: n), FDeckWell.maxLayers)
        }

        // …AND NEVER MORE LAYERS THAN CARDS. The pile is an approximation of a
        // number and may be coarser than it; it may never claim MORE.
        for n in 0..<52 {
            XCTAssertLessThanOrEqual(FDeckWell.layers(for: n), max(n, 0),
                                     "the pile claims more cards than the stock holds at \(n)")
        }

        // WHERE THE STEPS FALL, pinned. The properties above are the rule, but
        // the owner has been handed the actual table - "7 to 11 all draw seven,
        // 12 and up all draw eight" - so it is a promise, not an accident, and
        // it must not drift without someone saying so. (Everything below 7 is
        // covered exactly by the loop at the top.)
        XCTAssertEqual((7...11).map(FDeckWell.layers(for:)), Array(repeating: 7, count: 5),
                       "7...11 share one thickness")
        XCTAssertEqual([12, 17, 23, 35].map(FDeckWell.layers(for:)), [8, 8, 8, 8],
                       "12 and up share the next, and the deepest stock in the game is 23")
    }

    /// MUTATION-CHECKED: `maxLayers` raised to 9 -> 1 failure; `leanY` raised
    /// from 2 to 3 -> 1 failure.
    ///
    /// THE PILE STAYS ON THE BOARD. `maxLayers` is a geometric bound, and this
    /// re-derives it from the geometry rather than restating the number: the
    /// board surface is a 30pt-radius rounded rectangle (measured off a
    /// rendered frame, twice), the stack sits 8pt in from its left edge and
    /// 14pt down from its top (`MessageTableView`'s board padding) plus the
    /// well's own 8pt inset, and each layer steps `FDeckWell.leanX/leanY`. A
    /// card fits while the centre of its own rounded corner stays inside the
    /// board's arc by that corner's radius - so changing the lean, the inset or
    /// the ceiling has to come back through here.
    func testTheTallestPileStaysInsideTheBoardsRoundedCorner() {
        let boardRadius: CGFloat = 30              // the surface's corner
        let cardRadius: CGFloat = min(5, 46 * 0.1) // FCard.radius at 46pt wide
        let arc = CGPoint(x: boardRadius, y: boardRadius)

        func cornerCentre(layer i: Int) -> CGPoint {
            // FSpace.s of board padding + FSpace.s of well inset, and 14pt of
            // board padding down; then the lean (read off the well, so changing
            // it has to change this bound too), then the card's own radius.
            CGPoint(x: FSpace.s + FSpace.s - CGFloat(i) * FDeckWell.leanX + cardRadius,
                    y: 14 + FSpace.s - CGFloat(i) * FDeckWell.leanY + cardRadius)
        }
        func clearance(layer i: Int) -> CGFloat {
            let c = cornerCentre(layer: i)
            return (boardRadius - cardRadius) - hypot(arc.x - c.x, arc.y - c.y)
        }

        XCTAssertGreaterThan(clearance(layer: FDeckWell.maxLayers - 1), 1.0,
                             "the top layer of a full pile is off the board's corner")
        XCTAssertLessThan(clearance(layer: FDeckWell.maxLayers), 1.0,
                          "maxLayers leaves a whole layer of unused room - it is "
                          + "supposed to be the ceiling, not a guess below it")
    }

    // MARK: - 3. the non-regressions

    /// Round 4 note 6, re-asserted here because this round changed the code
    /// around it: "the flipped card jumps slightly when the deck finishes; its
    /// position should be constant relative to the top left corner throughout
    /// the game." Neither anchor may take the count as an input, and the new
    /// curve must not have smuggled one in.
    ///
    /// MUTATION-CHECKED: `flippedOrigin` made a function of `stackLayers`
    /// -> does not compile (it is a `static let`), which is the point of it
    /// being one; the value check below is what fails if the constant moves.
    func testTheFlippedCardStillDoesNotHopWhenTheDeckEmpties() {
        XCTAssertEqual(FDeckWell.flippedOrigin.x, FSpace.s + 10, accuracy: 0.001)
        XCTAssertEqual(FDeckWell.flippedOrigin.y, FSpace.s + 20, accuracy: 0.001)
        XCTAssertEqual(FDeckWell.bottomCardOrigin.x, FSpace.s, accuracy: 0.001)
        XCTAssertEqual(FDeckWell.bottomCardOrigin.y, FSpace.s, accuracy: 0.001)
    }

}
