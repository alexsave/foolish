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

    /// A SCALED CARD ON THE PUBLIC BOARD IS A NORMAL CARD, JUST SMALLER.
    ///
    /// Owner, on pass 2: "I don't like that you used a skinny card for the
    /// flipped. It should be a normal card just scaled down." Laying the
    /// pieces out at their size (the edge fix) had pushed every card under
    /// 40pt wide onto FCard's THIN face - a rank over one pip, centred, no
    /// corner indices - which is a different card, not a smaller one. The
    /// full face is proportional to the width throughout, so drawn at 28pt
    /// it IS the 46pt card scaled, and its edge is still `restWidth`.
    ///
    /// The tell is a corner index: the full face has its rank in the corners
    /// and the thin face has nothing in a card's outer fifth. For the well's
    /// flipped trump, tucked under the stock, the visible index is the
    /// bottom-right one; for an upright battle card the top-left. Both must
    /// show ink. Written before the fix and RED on the pass-2 code (the thin
    /// face), which is the mutant for this change. (A first cut of the
    /// flipped check looked at the top-left corner over the card's whole
    /// height and stayed green: the stock's fern backs overlapping the
    /// card's top counted as ink.)
    @MainActor
    func testAScaledBubbleCardWearsTheFullFace() throws {
        let floor = PublicBoardLayout.scaleFloor
        // The flipped trump in a well at the floor: its card is 46 x floor
        // wide, at (flippedOrigin.x x floor, inset + peek) in the well, and
        // the stock's bottom card (46 x floor tall, laid landscape) covers
        // its top down to inset + 46 x floor.
        let well = FDeckWell(deckCount: 12, flipped: Card(s: 1, v: 12), hasFlipped: true,
                             trumpSuit: .hearts, scale: floor)
        let wellImage = try XCTUnwrap(Self.render(well.frame(width: 92 * floor, height: 108 * floor)))
        let flipped = CGRect(x: FDeckWell.flippedOrigin.x * floor, y: FSpace.s * floor + FDeckWell.peek(scale: floor),
                             width: 46 * floor, height: 66 * floor)
        let stockBottom = FSpace.s * floor + 46 * floor
        let trailingCorner = CGRect(x: flipped.maxX - flipped.width * 0.22, y: max(flipped.midY, stockBottom + 1),
                                    width: flipped.width * 0.22 - 1.5, height: 0)
            .union(CGRect(x: flipped.maxX - 1.5, y: flipped.maxY - 1.5, width: 0, height: 0))
        XCTAssertGreaterThan(Self.ink(in: trailingCorner, of: wellImage), 15,
                             "the flipped trump's rank is in its bottom-right corner: a full face, scaled")

        // A battle card at the two-row cluster scale (27.5 x 38.5): one
        // uncovered attack, bottom-aligned in its slot, upright.
        let scale: CGFloat = 0.55
        let grid = FBattleGrid(battles: [BattleView(attack: Card(s: 0, v: 7), defense: nil)],
                               trumpSuit: nil, scale: scale)
        let slot = CGSize(width: FBattleGrid.slotSize.width * scale, height: FBattleGrid.slotSize.height * scale)
        let gridImage = try XCTUnwrap(Self.render(grid.frame(width: slot.width, height: slot.height)))
        let card = CGRect(x: (slot.width - 50 * scale) / 2, y: slot.height - 70 * scale,
                          width: 50 * scale, height: 70 * scale)
        let leadingCorner = CGRect(x: card.minX + 1.5, y: card.minY + 1.5,
                                   width: card.width * 0.22 - 1.5, height: card.height / 2)
        XCTAssertGreaterThan(Self.ink(in: leadingCorner, of: gridImage), 15,
                             "a battle card's rank is in its top-left corner: a full face, scaled")
        XCTAssertEqual(FDeckWell.peek(scale: 1), 20, "the live board's peek is untouched")
    }

    /// ONE COUNT SIZE ON THE BUBBLE. Owner: "The fan card and discard card
    /// and deal card count numbers should be same font size in the bubble
    /// preview." Measured off the real 8-seat bubble: the digit height of
    /// seat 0's count on its tag, of the deck well's count and of the
    /// discard pile's count agree within a point. RED on pass 2, where the
    /// tag drew 15pt and the two piles floored at 13.
    @MainActor
    func testTheBubbleCountsShareOneSize() throws {
        let view = publicBoardFixture(players: 8, battles: 2, covered: 2, defender: 1, deck: 12, discard: 6)
        let img = try XCTUnwrap(BubbleSnapshot.render(publicView: view, names: bubbleNames(8)))
        let board = CGSize(width: BubbleSnapshot.size.width - 16, height: BubbleSnapshot.size.height - 16)
        let inset: CGFloat = 8
        let s = PublicBoardLayout.cornerScale(n: 8, in: board)
        let tag = PublicBoardLayout.seatRect(seat: 0, n: 8, in: board).offsetBy(dx: inset, dy: inset)
        let tagCard = CGRect(x: tag.minX, y: tag.minY + 15, width: FSeatTag.cardSize.height, height: tag.height - 15)
        // A 12-card stock is a full one (8 layers), so the chip is at the
        // top of its ride, where `deckCountCentre` models it.
        let deckC = PublicBoardLayout.deckCountCentre(scale: s)
        let deck = CGRect(x: deckC.x + inset - 14, y: deckC.y + inset - 9, width: 28, height: 18)
        let pileC = CGPoint(x: board.width - FDiscardPile.footprint.width * s / 2 + inset,
                            y: PublicBoardLayout.discardLift + FDiscardPile.footprint.height * s / 2 + inset)
        let pile = CGRect(x: pileC.x - 14, y: pileC.y - 7, width: 28, height: 14)
        let heights = [tagCard, deck, pile].map { Self.whiteRowExtent(in: $0, of: img) }
        XCTAssertGreaterThan(heights.min() ?? 0, 6, "digits were found in all three places: \(heights)")
        XCTAssertEqual(heights.max()! - heights.min()!, 0, accuracy: 1.0,
                       "tag, deck and discard counts are one size (digit heights \(heights) pt)")
    }

    /// Render a view at 2x on a plain white ground, light scheme.
    @MainActor
    private static func render<V: View>(_ view: V) -> UIImage? {
        let r = ImageRenderer(content: view.background(Color.white).environment(\.colorScheme, .light))
        r.scale = 2
        return r.uiImage
    }

    private static func pixels(_ image: UIImage) -> (bytes: [UInt8], w: Int, h: Int, scale: CGFloat)? {
        guard let cg = image.cgImage else { return nil }
        let w = cg.width, h = cg.height
        var bytes = [UInt8](repeating: 0, count: w * h * 4)
        guard let ctx = CGContext(data: &bytes, width: w, height: h, bitsPerComponent: 8,
                                  bytesPerRow: w * 4, space: CGColorSpaceCreateDeviceRGB(),
                                  bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)
        else { return nil }
        ctx.draw(cg, in: CGRect(x: 0, y: 0, width: w, height: h))
        return (bytes, w, h, CGFloat(w) / image.size.width)
    }

    /// Ink (anything that is not the white face) inside `rect`, in points.
    private static func ink(in rect: CGRect, of image: UIImage) -> Int {
        guard let p = pixels(image) else { return 0 }
        let x0 = max(0, Int(rect.minX * p.scale)), x1 = min(p.w, Int(rect.maxX * p.scale))
        let y0 = max(0, Int(rect.minY * p.scale)), y1 = min(p.h, Int(rect.maxY * p.scale))
        guard x1 > x0, y1 > y0 else { return 0 }
        var n = 0
        for y in y0..<y1 {
            for x in x0..<x1 {
                let i = (y * p.w + x) * 4
                if p.bytes[i] < 200 || p.bytes[i + 1] < 200 || p.bytes[i + 2] < 200 { n += 1 }
            }
        }
        return n
    }

    /// The height, in points, of the band of rows inside `rect` that hold
    /// any pure white pixel - a count chip's digits on a red-and-black back.
    private static func whiteRowExtent(in rect: CGRect, of image: UIImage) -> CGFloat {
        guard let p = pixels(image) else { return 0 }
        let x0 = max(0, Int(rect.minX * p.scale)), x1 = min(p.w, Int(rect.maxX * p.scale))
        let y0 = max(0, Int(rect.minY * p.scale)), y1 = min(p.h, Int(rect.maxY * p.scale))
        guard x1 > x0, y1 > y0 else { return 0 }
        var first = -1, last = -1
        for y in y0..<y1 {
            var any = false
            for x in x0..<x1 {
                let i = (y * p.w + x) * 4
                if p.bytes[i] > 235 && p.bytes[i + 1] > 235 && p.bytes[i + 2] > 235 { any = true; break }
            }
            if any { if first < 0 { first = y }; last = y }
        }
        return first < 0 ? 0 : CGFloat(last - first + 1) / p.scale
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
