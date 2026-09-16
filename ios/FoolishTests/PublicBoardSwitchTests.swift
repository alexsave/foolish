// PublicBoardSwitchTests.swift - the two switches on the public board and
// the live board's mark spacing, and the sizing that keeps a card's edge.
//
// The owner's rule for new work: a shipping default, a DEBUG knob that can
// select either path, the knob's default DERIVED from the shipping constant,
// and tests on both states. The collapse slide (build 71) is the model, and
// its lesson is the second test here - a literal beside the constant nearly
// shipped the product one way and every debug install the other.

import XCTest
import SwiftUI
import UIKit
@testable import FoolishKit

final class PublicBoardSwitchTests: XCTestCase {

    // MARK: The switches

    /// What ships: the seat tags on the bubble, and the tighter marks on the
    /// live board. Both ON - they are what the owner is reviewing, and a
    /// feature behind an OFF flag is never actually looked at.
    /// MUTANT: either constant `false` fails its line.
    func testTheShippingBoardIsTagsWithTightMarks() {
        XCTAssertTrue(PublicBoardLayout.tagsByDefault, "the bubble ships the seat tags")
        XCTAssertTrue(FSeatBadge.tightMarksByDefault, "the live board ships the tighter marks")
    }

    #if DEBUG || SOLO_TESTING
    /// A debug install with no `dev.board` file draws what the product
    /// draws. MUTANT: a literal `true`/`false` in BoardKnobs fails this the
    /// moment it disagrees with the constant it should be reading.
    func testADebugBuildDefaultsToTheShippingBoard() {
        XCTAssertEqual(MessageDevBoard.BoardKnobs().tags, PublicBoardLayout.tagsByDefault,
                       "a debug install must draw the board the product ships")
        XCTAssertEqual(MessageDevBoard.BoardKnobs().tightMarks, FSeatBadge.tightMarksByDefault,
                       "a debug install must space the marks as the product does")
    }
    #endif

    /// The ring the knob falls back to IS the live board's: a 35% ellipse,
    /// seat 0 at the bottom, seat 1 to its left. MUTANT: 0.35 -> 0.42 fails
    /// the side seats' x.
    func testTheRingIsTheLiveBoardsEllipse() {
        let size = CGSize(width: 284, height: 179)
        let p0 = PublicBoardLayout.ringPoint(seat: 0, n: 2, in: size)
        XCTAssertEqual(p0.x, 142, accuracy: 0.01)
        XCTAssertEqual(p0.y, 0.85 * 179, accuracy: 0.01, "seat 0 at the bottom")
        let p1 = PublicBoardLayout.ringPoint(seat: 1, n: 2, in: size)
        XCTAssertEqual(p1.y, 0.15 * 179, accuracy: 0.01, "seat 1 across the table")
        let left = PublicBoardLayout.ringPoint(seat: 1, n: 4, in: size)
        XCTAssertEqual(left.x, 0.15 * 284, accuracy: 0.01, "at four, seat 1 is the left side")
        XCTAssertEqual(left.y, 89.5, accuracy: 0.01)
        let right = PublicBoardLayout.ringPoint(seat: 3, n: 4, in: size)
        XCTAssertEqual(right.x, 0.85 * 284, accuracy: 0.01)
    }

    /// Both boards draw, from the same view, and they are different
    /// pictures - the switch selects one. MUTANT: `tagsBoard` for both
    /// branches of `MessageBoardView.body` makes the two images identical.
    @MainActor
    func testTheRingIsOneSwitchAway() throws {
        let view = publicBoardFixture(players: 8, battles: 2, covered: 2, defender: 1)
        func render(tags: Bool) -> Data? {
            let content = ZStack {
                FColor.fallback
                MessageBoardView(view: view, names: bubbleNames(8), tags: tags)
            }
            .frame(width: BubbleSnapshot.size.width, height: BubbleSnapshot.size.height)
            .environment(\.colorScheme, .light)
            let r = ImageRenderer(content: content)
            r.scale = 2
            return r.uiImage?.pngData()
        }
        let tags = try XCTUnwrap(render(tags: true), "the tags board renders")
        let ring = try XCTUnwrap(render(tags: false), "the ring board renders")
        XCTAssertNotEqual(tags, ring, "the switch selects a different picture")
    }

    // MARK: The marks

    /// The nudge is one number, applied both ways: the seats' rows come up
    /// by it, my own mark comes down by it, and OFF is exactly what shipped
    /// (a 4pt gap under the fan, a 6pt lift over my hand).
    /// MUTANT: `selfMarkLift` adding instead of subtracting fails the last.
    func testTightMarksAreOneNudgeBothWays() {
        XCTAssertEqual(FSeatBadge.fanMarkGap(tight: false), FSpace.xs, "OFF is the old row gap")
        XCTAssertEqual(FSeatBadge.selfMarkLift(tight: false), 6, "OFF is the old lift")
        XCTAssertEqual(FSeatBadge.fanMarkGap(tight: true),
                       FSeatBadge.fanMarkGap(tight: false) - FSeatBadge.markTightening,
                       "the seats' marks come UP by the nudge")
        XCTAssertEqual(FSeatBadge.selfMarkLift(tight: true),
                       FSeatBadge.selfMarkLift(tight: false) - FSeatBadge.markTightening,
                       "my own mark comes DOWN by the nudge")
        XCTAssertGreaterThan(FSeatBadge.markTightening, 0)
        XCTAssertLessThanOrEqual(FSeatBadge.markTightening, 8, "slightly, the owner said")
    }

    /// The badge really is shorter by the nudge when tight - measured off the
    /// laid-out view, not read off the constant, because the row's padding
    /// is what moves the mark and a constant nobody applies moves nothing.
    /// MUTANT: dropping the `.padding(.top, fanMarkGap)` off `roleRow` makes
    /// the two heights equal.
    @MainActor
    func testATightBadgeIsShorterByTheNudge() {
        func height(tight: Bool) -> CGFloat {
            let badge = FSeatBadge(name: "Vera", handCount: 6, isDefender: true, tightMarks: tight)
            let host = UIHostingController(rootView: badge)
            return host.sizeThatFits(in: CGSize(width: 400, height: 400)).height
        }
        let loose = height(tight: false), tight = height(tight: true)
        XCTAssertEqual(loose - tight, FSeatBadge.markTightening, accuracy: 0.5,
                       "the role row rides up into the fan box by the nudge (\(loose) -> \(tight))")
    }

    /// A small well tucks its flipped trump LESS. Under 40pt wide FCard draws
    /// its thin face - rank over suit, centred - and a card tucked 40% under
    /// the stock showed a bare spade where the K of spades was (the first
    /// eight-seat frame). A thin flipped card leaves 4pt under the stock; a
    /// full-face one keeps the 20pt peek, scaled. The well's ink follows.
    /// MUTANT: `peek(scale:)` returning `20 * scale` for every width fails
    /// the first line.
    func testAThinFlippedTrumpPeeksOutToShowItsRank() {
        let thinWidth = 46 * PublicBoardLayout.scaleFloor
        XCTAssertLessThan(thinWidth, 40, "the floor's flipped card is a thin card")
        XCTAssertEqual(thinWidth - FDeckWell.peek(scale: PublicBoardLayout.scaleFloor), 4, accuracy: 0.001,
                       "4pt of a thin flipped card under the stock, no more")
        XCTAssertEqual(FDeckWell.peek(scale: 1), 20, "the live board's peek is untouched")
        XCTAssertEqual(FDeckWell.peek(scale: 0.9), 18, accuracy: 0.001, "a 41pt card still wears the full face")
        let ink = FDeckWell.inkFootprint(scale: 1)
        XCTAssertEqual(ink.width, 74, accuracy: 0.001)
        XCTAssertEqual(ink.height, 94, accuracy: 0.001, "inset + peek + a card")
        XCTAssertEqual(FDeckWell.inkFootprint(scale: PublicBoardLayout.scaleFloor).height,
                       FSpace.s * 0.6 + FDeckWell.peek(scale: 0.6) + 66 * 0.6, accuracy: 0.001)
    }

    // MARK: Sized, not transformed

    /// The public board's pieces are laid out at their scale - the cards, the
    /// gaps, the frame - rather than drawn full size and transformed, which
    /// is what keeps FCard's 1pt edge at every size ("card edges look too
    /// thin, keep them whatever they are in the game"). Measured off the
    /// laid-out views. MUTANT: leaving `columnGap` unscaled in FBattleGrid
    /// fails the first width; leaving the frame unscaled in FDeckWell fails
    /// the second.
    @MainActor
    func testPublicBoardPiecesLayOutAtTheirScale() {
        let fit = CGSize(width: 1000, height: 1000)
        let pairs = publicBoardFixture(players: 2, battles: 4, covered: 2).battles
        let grid = UIHostingController(rootView: FBattleGrid(battles: pairs, trumpSuit: nil, scale: 0.5))
            .sizeThatFits(in: fit)
        let natural = FBattleGrid.naturalSize(pairs: 4)
        XCTAssertEqual(grid.width, natural.width * 0.5, accuracy: 0.5, "four pairs at half size")
        XCTAssertEqual(grid.height, natural.height * 0.5, accuracy: 0.5)

        let well = UIHostingController(rootView: FDeckWell(deckCount: 12, flipped: Card(s: 1, v: 11),
                                                           hasFlipped: true, trumpSuit: .hearts, scale: 0.6))
            .sizeThatFits(in: fit)
        XCTAssertEqual(well.width, 92 * 0.6, accuracy: 0.5, "the well's frame at 0.6")
        XCTAssertEqual(well.height, 108 * 0.6, accuracy: 0.5)

        let pile = UIHostingController(rootView: FDiscardPile(count: 6, scale: 0.6)).sizeThatFits(in: fit)
        XCTAssertEqual(pile.width, FDiscardPile.footprint.width * 0.6, accuracy: 0.5)
        XCTAssertEqual(pile.height, FDiscardPile.footprint.height * 0.6, accuracy: 0.5)
    }
}
