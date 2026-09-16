// BubbleBoardTests.swift - the 300x195 bubble at every player count.
//
// The bubble is rendered from a spectator view (viewer -1), so every fixture
// here is built the same way: no hand on any seat. That is the structural
// safety BubbleSnapshot relies on, and a fixture that broke it would be
// testing a picture the product can never draw.
//
// Two kinds of test. The GEOMETRY ones ask PublicBoardLayout, with no view in
// the loop, whether the pieces fit - they are the ones that would have gone
// red on the eight-seat bubble the owner filmed. The RENDER ones put the real
// picture through ImageRenderer and look at pixels, because a layout that
// fits on paper and a view that draws it are two different artifacts
// (docs/WINS_AND_LESSONS.md, lesson 1).

import XCTest
import SwiftUI
import UIKit
@testable import FoolishKit

/// A PUBLIC board fixture: `n` seats, `battles` attacks of which the first
/// `covered` are covered, and the rest of the state chosen so the board is a
/// legal-looking mid-game table. Deterministic so a dump is reproducible.
func publicBoardFixture(players n: Int, battles: Int, covered: Int,
                        defender: Int = 0, goodSeats: [Int] = [],
                        deck: Int = 12, discard: Int = 6,
                        counts: [Int]? = nil, out: [Int] = [],
                        gameOver: Int = -1) -> GameView {
    let hands = counts ?? (0..<n).map { 4 + ($0 * 3) % 5 }
    let players = (0..<n).map { s in
        PlayerView(seat: s, name: "", status: out.contains(s) ? 3 : 2,
                   handCount: out.contains(s) ? 0 : hands[s],
                   awaitingAttack: false, strategyKey: 0, hand: nil)
    }
    // Distinct ranks per attack so no two cards on the table read the same;
    // 13 is the ace (value 1 is a two), so six pairs top out at 11 and 13.
    let table = (0..<battles).map { i -> BattleView in
        let atk = Card(s: i % 4, v: 6 + i)
        let def: Card? = i < covered ? Card(s: (i + 1) % 4, v: 8 + i) : nil
        return BattleView(attack: atk, defense: def)
    }
    var mask = 0
    for s in goodSeats { mask |= 1 << s }
    let first = (defender + n - 1) % n
    return GameView(status: 1, numPlayers: n, powerSuit: 1, deckCount: deck,
                    discardCount: discard, hasFlipped: deck > 0,
                    firstAttacker: first, defender: defender, viewer: -1,
                    goodMask: mask, gameOver: gameOver,
                    flipped: deck > 0 ? Card(s: 1, v: 11) : nil,
                    battles: table, eliminationOrder: out, players: players)
}

/// The rig's cast (c/tests/msg_wire_test.c fixture_name), so a headless dump
/// and a simulator shot of the same seat say the same name.
let bubbleCast = ["Alex", "Mira", "Jonas", "Priya", "Tomas", "Nadia", "Felix", "Sana"]
func bubbleNames(_ n: Int) -> [Int: String] {
    Dictionary(uniqueKeysWithValues: (0..<n).map { ($0, bubbleCast[$0]) })
}

final class BubbleBoardTests: XCTestCase {

    /// The board the bubble lays out in: BubbleSnapshot.size less
    /// MessageBoardView's 8pt inset on every side.
    private let bubble = CGSize(width: BubbleSnapshot.size.width - 16,
                                height: BubbleSnapshot.size.height - 16)
    /// The spectator fallback in the drawer (MessagesRootView) is the other
    /// place this board draws; roomier, and it must fit there too.
    private let drawer = CGSize(width: 375 - 16, height: 340 - 16)

    // MARK: Geometry

    /// THE EIGHT-SEAT DEFECT, as arithmetic: every seat tag is inside the
    /// board and no two touch, at every count the game allows. The filmed
    /// bubble failed both halves - the top seat's name was outside the
    /// picture and the side seats lay on each other.
    func testSeatTagsFitAndNeverTouchAtEveryCount() {
        for size in [bubble, drawer] {
            let board = CGRect(origin: .zero, size: size)
            for n in 2...8 {
                let rects = PublicBoardLayout.seatRects(n: n, in: size)
                XCTAssertEqual(rects.count, n)
                for (i, r) in rects.enumerated() {
                    XCTAssertTrue(board.contains(r), "seat \(i) of \(n) leaves the board: \(r) in \(size)")
                    for (j, o) in rects.enumerated() where j > i {
                        XCTAssertFalse(r.intersects(o), "seats \(i) and \(j) of \(n) overlap in \(size)")
                    }
                }
            }
        }
    }

    /// Seat 0 is the bottom centre at every count (the live board's spectator
    /// convention), and the seats run clockwise - seat 1 is to its LEFT, the
    /// way the live board's ring runs, so a bubble and the board it opens
    /// into agree about who sits where.
    func testSeatZeroIsBottomCentreAndSeatsRunClockwise() {
        for n in 2...8 {
            let p0 = PublicBoardLayout.seatPoint(seat: 0, n: n, in: bubble)
            XCTAssertEqual(p0.x, bubble.width / 2, accuracy: 0.01, "seat 0 centred at \(n)")
            XCTAssertEqual(p0.y, bubble.height - PublicBoardLayout.tagSize.height / 2, accuracy: 0.01)
            if n > 2 {
                let p1 = PublicBoardLayout.seatPoint(seat: 1, n: n, in: bubble)
                XCTAssertLessThan(p1.x, p0.x, "seat 1 is left of seat 0 at \(n)")
                let last = PublicBoardLayout.seatPoint(seat: n - 1, n: n, in: bubble)
                XCTAssertGreaterThan(last.x, p0.x, "the last seat is right of seat 0 at \(n)")
            }
        }
        // Two seats face each other; four take the four sides; eight fill all.
        XCTAssertEqual(PublicBoardLayout.stations(n: 2), [.bottom, .top])
        XCTAssertEqual(PublicBoardLayout.stations(n: 4), [.bottom, .left, .top, .right])
        XCTAssertEqual(PublicBoardLayout.stations(n: 8), PublicBoardLayout.Station.allCases)
        // Every seat its own station, at every count.
        for n in 2...8 {
            XCTAssertEqual(Set(PublicBoardLayout.stations(n: n)).count, n, "distinct stations at \(n)")
        }
    }

    /// The deck well and the discard pile clear the seat tags at every count,
    /// at whatever size the layout picks for them - and it does not shrink
    /// them when it need not. Two seats leave them full size (the common
    /// case is drawn exactly as before); a top row beside the corners takes
    /// them to the floor.
    func testCornersClearTheSeatsAndShrinkOnlyWhenTheyMust() {
        for n in 2...8 {
            let s = PublicBoardLayout.cornerScale(n: n, in: bubble)
            let deck = PublicBoardLayout.deckRect(scale: s)
            let pile = PublicBoardLayout.discardRect(scale: s, in: bubble)
            for (i, r) in PublicBoardLayout.seatRects(n: n, in: bubble).enumerated() {
                XCTAssertFalse(r.intersects(deck), "deck well under seat \(i) at \(n) (scale \(s))")
                XCTAssertFalse(r.intersects(pile), "discard under seat \(i) at \(n) (scale \(s))")
            }
        }
        XCTAssertEqual(PublicBoardLayout.cornerScale(n: 2, in: bubble), 1, "two seats: full size")
        XCTAssertEqual(PublicBoardLayout.cornerScale(n: 8, in: bubble), PublicBoardLayout.scaleFloor,
                       accuracy: 0.001, "eight seats: the floor")
        XCTAssertEqual(PublicBoardLayout.cornerScale(n: 2, in: drawer), 1)
        // The owner asked for a bigger trump indicator. Three seats put two
        // tags on the top row, spread so the corners keep 67pt (the discard
        // pile's width is what stops at 0.85); four seats have side seats
        // below the middle (`sideY`), so the well is held only by its
        // height, and at 0.9 its flipped card is still wide enough for
        // FCard's full face and the short peek (FDeckWell.peek), so the
        // whole well fits above them. The first cut had every count from
        // three up at the 0.6 floor.
        XCTAssertEqual(PublicBoardLayout.cornerScale(n: 3, in: bubble), 0.85, accuracy: 0.001)
        XCTAssertEqual(PublicBoardLayout.cornerScale(n: 4, in: bubble), 0.9, accuracy: 0.001)
    }

    /// The deck's COUNT is never under the balloon's app icon, at any count.
    /// Messages paints that roundel over the bubble's top-left corner, and
    /// the first eight-seat frame shot on the simulator had a deck well whose
    /// "4" was entirely behind it. The chip's digits reach `countReach` from
    /// their centre - the half-diagonal of a two-digit count at the bubble's
    /// one count size - so the centre must clear the icon's edge by that.
    /// (Was a flat 7pt for 13pt digits; at 15pt the reach is 10.5, and this
    /// went red until the well slid further.)
    func testDeckCountClearsTheBalloonIcon() {
        for n in 2...8 {
            let s = PublicBoardLayout.cornerScale(n: n, in: bubble)
            let c = PublicBoardLayout.deckCountCentre(scale: s)
            let d = hypot(c.x - PublicBoardLayout.balloonIconCentre.x,
                          c.y - PublicBoardLayout.balloonIconCentre.y)
            XCTAssertGreaterThan(d, PublicBoardLayout.balloonIconRadius + PublicBoardLayout.countReach,
                                 "deck count at \(n) seats (scale \(s)) sits under the balloon icon: \(c)")
        }
        XCTAssertGreaterThanOrEqual(PublicBoardLayout.countReach, 10, "two 15pt digits reach past 10pt")
        // The slide is what buys it: without it the count at the floor would
        // be inside the roundel.
        XCTAssertEqual(PublicBoardLayout.deckSlide(scale: 1), .zero, "a full-size well does not move")
        XCTAssertGreaterThan(PublicBoardLayout.deckSlide(scale: PublicBoardLayout.scaleFloor).y, 0)
    }

    /// The battle cluster clears every seat tag and both corner pieces at
    /// every count and every table up to six pairs, and is drawn full size
    /// wherever it can be: one attack at two seats is the transcript's
    /// bread and butter and must not shrink, and two covered attacks at
    /// eight seats (--twocover 8, the filmed case) fit at full size once the
    /// seats are tags.
    func testBattlesFitBetweenTheSeatsAndTheCorners() {
        for n in 2...8 {
            let corner = PublicBoardLayout.cornerScale(n: n, in: bubble)
            let blockers = PublicBoardLayout.seatRects(n: n, in: bubble)
                + [PublicBoardLayout.deckRect(scale: corner),
                   PublicBoardLayout.discardRect(scale: corner, in: bubble)]
            for pairs in 1...6 {
                let s = PublicBoardLayout.gridScale(n: n, pairs: pairs, in: bubble)
                XCTAssertGreaterThanOrEqual(s, PublicBoardLayout.gridFloor)
                XCTAssertLessThanOrEqual(s, 1)
                let g = PublicBoardLayout.gridRect(pairs: pairs, scale: s, in: bubble)
                for (i, b) in blockers.enumerated() {
                    XCTAssertFalse(b.intersects(g), "\(pairs) pairs at \(n) seats (scale \(s)) run under piece \(i)")
                }
                XCTAssertTrue(CGRect(origin: .zero, size: bubble).contains(g),
                              "\(pairs) pairs at \(n) seats leave the board")
            }
        }
        XCTAssertEqual(PublicBoardLayout.gridScale(n: 2, pairs: 1, in: bubble), 1)
        XCTAssertEqual(PublicBoardLayout.gridScale(n: 8, pairs: 2, in: bubble), 1)
        // Two rows never fit the 99pt band between a top and a bottom tag at
        // full size; they are drawn at the largest size that does.
        XCTAssertLessThan(PublicBoardLayout.gridScale(n: 2, pairs: 4, in: bubble), 1)
        XCTAssertEqual(PublicBoardLayout.gridScale(n: 8, pairs: 6, in: bubble), 0.55, accuracy: 0.001)
    }

    /// The natural cluster size PublicBoardLayout scales is the grid's own
    /// arithmetic: `columns` across, the rest wrapping.
    func testBattleGridNaturalSizeWraps() {
        let slot = FBattleGrid.slotSize
        XCTAssertEqual(FBattleGrid.naturalSize(pairs: 0), .zero)
        XCTAssertEqual(FBattleGrid.naturalSize(pairs: 1), slot)
        XCTAssertEqual(FBattleGrid.naturalSize(pairs: 3).width,
                       3 * slot.width + 2 * FBattleGrid.columnGap)
        XCTAssertEqual(FBattleGrid.naturalSize(pairs: 4).height,
                       2 * slot.height + FBattleGrid.rowGap)
        XCTAssertEqual(FBattleGrid.naturalSize(pairs: 4).width,
                       FBattleGrid.naturalSize(pairs: 3).width, "a fourth pair wraps; the row is no wider")
    }

    // MARK: Render

    /// The bubble at eight seats renders, at the template size, from a
    /// no-hand view - the eight-seat twin of
    /// ComponentSnapshotTests.testBubbleSnapshotRendersPublicBoard - and the
    /// top seat's NAME IS IN THE PICTURE. That is the filmed defect: the top
    /// seat's badge hung 15pt above the bubble and "Tomas" was not on it.
    /// The name is bone ink (FColor.textPrimary) on the wool; the band where
    /// the top station's name line lies must hold some of it.
    @MainActor
    func testEightSeatBubbleShowsTheTopSeat() throws {
        let view = publicBoardFixture(players: 8, battles: 2, covered: 2, defender: 1, goodSeats: [2])
        let img = try XCTUnwrap(BubbleSnapshot.render(publicView: view, names: bubbleNames(8)))
        XCTAssertEqual(img.size, BubbleSnapshot.size)
        XCTAssertTrue(view.players.allSatisfy { $0.hand == nil }, "the source is a no-hand public view")

        // The top station's tag, in bubble points (board inset 8).
        let tag = PublicBoardLayout.seatRect(seat: 4, n: 8, in: bubble).offsetBy(dx: 8, dy: 8)
        // The name is the tag's first ~14pt.
        let nameBand = CGRect(x: tag.minX, y: tag.minY, width: tag.width, height: 14)
        let bone = Self.count(in: img, rect: nameBand, matching: Self.isBone)
        XCTAssertGreaterThan(bone, 40, "the top seat's name is drawn inside the bubble (\(bone) bone px)")
        // ...and the same band ABOVE the bubble's top edge is where the old
        // badge put it; there is nothing to look at there, which is the point.
        XCTAssertEqual(nameBand.minY, 8, accuracy: 0.01, "the name band starts at the board inset")
    }

    /// Every seat's count is on its tag: the digits are the only white on
    /// the CARD (a red-and-black fern back), so a card box with no white in
    /// it is a seat with no count.
    ///
    /// The card box, not the whole row: the sword and the shield beside the
    /// card are white too, and a first cut that sampled the row stayed green
    /// with the chip removed - the marks were answering for the digits.
    @MainActor
    func testEightSeatBubbleShowsEveryCount() throws {
        let view = publicBoardFixture(players: 8, battles: 2, covered: 2, defender: 1)
        let img = try XCTUnwrap(BubbleSnapshot.render(publicView: view, names: bubbleNames(8)))
        for seat in 0..<8 {
            let tag = PublicBoardLayout.seatRect(seat: seat, n: 8, in: bubble).offsetBy(dx: 8, dy: 8)
            // The tag's row is card (34 wide), 2pt, mark box; the row is the
            // tag's full width, so the card is the row's first 34pt.
            let card = CGRect(x: tag.minX, y: tag.minY + 15,
                              width: FSeatTag.cardSize.height, height: tag.height - 15)
            let white = Self.count(in: img, rect: card, matching: Self.isWhite)
            XCTAssertGreaterThan(white, 10, "seat \(seat)'s count is on its card (\(white) white px)")
        }
    }

    /// The design loop. `TEST_RUNNER_FOOLISH_BUBBLE_DUMP=<dir>` on the
    /// xcodebuild line writes one PNG per case at device scale into <dir>, and
    /// the case list below is the set of tables a bubble has to read at. Not
    /// an assertion: a place to LOOK. Without the variable it is skipped.
    @MainActor
    func testDumpBubbleGallery() throws {
        guard let dir = ProcessInfo.processInfo.environment["FOOLISH_BUBBLE_DUMP"], !dir.isEmpty else {
            throw XCTSkip("set FOOLISH_BUBBLE_DUMP to write the gallery")
        }
        try FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
        let cases: [(String, GameView)] = [
            ("2p_1atk", publicBoardFixture(players: 2, battles: 1, covered: 0, defender: 0)),
            ("2p_2atk_1cov", publicBoardFixture(players: 2, battles: 2, covered: 1, defender: 1)),
            ("2p_3atk_2cov", publicBoardFixture(players: 2, battles: 3, covered: 2, defender: 1)),
            ("2p_6atk_5cov", publicBoardFixture(players: 2, battles: 6, covered: 5, defender: 1, deck: 0)),
            ("2p_over", publicBoardFixture(players: 2, battles: 0, covered: 0, deck: 0, counts: [3, 0], gameOver: 0)),
            ("3p_2atk_1cov_good", publicBoardFixture(players: 3, battles: 2, covered: 1, defender: 1, goodSeats: [2])),
            ("4p_2atk_1cov", publicBoardFixture(players: 4, battles: 2, covered: 1, defender: 2)),
            ("4p_4atk_3cov_good", publicBoardFixture(players: 4, battles: 4, covered: 3, defender: 0, goodSeats: [1])),
            ("5p_2atk_1cov_good", publicBoardFixture(players: 5, battles: 2, covered: 1, defender: 2, goodSeats: [3])),
            ("6p_2atk_1cov_good", publicBoardFixture(players: 6, battles: 2, covered: 1, defender: 3, goodSeats: [1])),
            ("6p_3atk_3cov_good", publicBoardFixture(players: 6, battles: 3, covered: 3, defender: 3, goodSeats: [1, 4])),
            ("7p_2atk_1cov_good", publicBoardFixture(players: 7, battles: 2, covered: 1, defender: 4, goodSeats: [2, 5])),
            ("8p_2atk_2cov_good", publicBoardFixture(players: 8, battles: 2, covered: 2, defender: 1, goodSeats: [2])),
            ("8p_4atk_2cov_good", publicBoardFixture(players: 8, battles: 4, covered: 2, defender: 4, goodSeats: [2, 5, 6])),
            ("8p_6atk_6cov_out", publicBoardFixture(players: 8, battles: 6, covered: 6, defender: 6, goodSeats: [0, 1], deck: 0, out: [3])),
        ]
        for (name, view) in cases {
            for scheme in [ColorScheme.light, .dark] {
                let img = try XCTUnwrap(BubbleSnapshot.render(publicView: view, names: bubbleNames(view.numPlayers), scheme: scheme))
                let data = try XCTUnwrap(img.pngData())
                let suffix = scheme == .dark ? "dark" : "light"
                try data.write(to: URL(fileURLWithPath: "\(dir)/\(name)_\(suffix).png"))
            }
        }
    }

    // MARK: Pixels

    /// Bone name ink (FColor.textPrimary 0xEDE9DF), with a little room for
    /// the antialiased edge - and NOT the count chip's pure white: bone is
    /// warm, its red 14 above its blue, and white is neither. A first cut
    /// took white too, and with the ring selected (`tags=0`) the old badge's
    /// fan put a white "6" in the band where the top seat's name should be,
    /// so the test that exists to notice the name was missing did not.
    private static func isBone(_ r: UInt8, _ g: UInt8, _ b: UInt8) -> Bool {
        r > 205 && g > 200 && b > 175 && Int(r) - Int(b) >= 8 && Int(r) - Int(b) <= 40
    }
    /// The count chip's white digits.
    private static func isWhite(_ r: UInt8, _ g: UInt8, _ b: UInt8) -> Bool {
        r > 235 && g > 235 && b > 235
    }

    /// Pixels in `rect` (image points) that satisfy `matching`.
    private static func count(in image: UIImage, rect: CGRect,
                              matching: (UInt8, UInt8, UInt8) -> Bool) -> Int {
        guard let cg = image.cgImage else { return 0 }
        let w = cg.width, h = cg.height
        var bytes = [UInt8](repeating: 0, count: w * h * 4)
        guard let ctx = CGContext(data: &bytes, width: w, height: h, bitsPerComponent: 8,
                                  bytesPerRow: w * 4, space: CGColorSpaceCreateDeviceRGB(),
                                  bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)
        else { return 0 }
        ctx.draw(cg, in: CGRect(x: 0, y: 0, width: w, height: h))
        let scale = CGFloat(w) / image.size.width
        let x0 = max(0, Int(rect.minX * scale)), x1 = min(w, Int(rect.maxX * scale))
        let y0 = max(0, Int(rect.minY * scale)), y1 = min(h, Int(rect.maxY * scale))
        // A band that lies outside the image (a tag placed above the bubble,
        // which is the defect this file exists for) has no pixels in it.
        guard x1 > x0, y1 > y0 else { return 0 }
        var n = 0
        for y in y0..<y1 {
            for x in x0..<x1 {
                let i = (y * w + x) * 4
                if matching(bytes[i], bytes[i + 1], bytes[i + 2]) { n += 1 }
            }
        }
        return n
    }
}
