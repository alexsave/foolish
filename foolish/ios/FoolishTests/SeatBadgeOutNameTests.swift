// ROUND 41: A PLAYER WHO IS OUT KEEPS THEIR NAME.
//
// The owner, off a screenshot of the iMessage board: "when a player gets out,
// their name rotates out along with the badge? Not ideal. Rotate only the
// badge, dim the name."
//
// Round 28 hung the out collapse on the WHOLE seat label - `.scaleEffect(x:)`
// on the outer VStack - so the name went edge-on with the hand and the role
// mark. Filmed on the rig (HARNESS_SCENARIO=arrival ARRIVE_KIND=gameover
// PLAYERS=4), the name "Boris" squeezes horizontally over about ten frames and
// is gone: a four-seat ring ends up showing two names and two empty spaces.
//
// MEASURED IN PIXELS, not asserted off the constants. A test that only asked
// `nameOpacity(collapsed: true) < 1` is green against the exact bug - the old
// code would have passed it while still turning the name away, because WHICH
// VIEW THE SCALE IS ATTACHED TO is the whole defect and no constant can see it.
// So these render the real `FSeatBadge` and count ink where the name is.
import XCTest
import SwiftUI
@testable import FoolishKit

@MainActor
final class SeatBadgeOutNameTests: XCTestCase {

    /// A badge on its own, at a fixed box, so the bands below are stable.
    ///
    /// `handCount` is not decoration. A seat holding NO cards draws no mini fan
    /// and no count chip, and with no role set it draws no mark either - so a
    /// zero-card badge is a name and nothing else, and the badge band below it
    /// is empty by construction (measured; `testTheBandsAreWhereTheContentIs`
    /// pins it). There would then be nothing for the collapse to take away and
    /// "did the badge turn" could not be asked at all. The board hands this the
    /// LAGGING `shownHandCount` (MessageTableView), which is exactly why a badge
    /// mid-collapse still has a hand to turn.
    private func render(handCount: Int = 5, collapsed: Bool,
                        scheme: ColorScheme = .dark) -> UIImage? {
        let badge = FSeatBadge(name: "Boris", handCount: handCount, isOut: true,
                               collapsed: collapsed)
            .frame(width: Self.box.width, height: Self.box.height)
            .environment(\.colorScheme, scheme)
        let r = ImageRenderer(content: badge)
        r.scale = 2
        return r.uiImage
    }

    private static let box = CGSize(width: 140, height: 110)

    /// Anything above this is ink the view drew; the ground is a hard 0.
    private static let alphaFloor: UInt8 = 40

    // MARK: - where the name ends and the badge begins

    // THE BANDS ARE READ OUT OF THE BITMAP, NOT WRITTEN DOWN HERE, and that is
    // the repair rather than a tidy-up.
    //
    // The first cut of this file hard-coded them as fractions of the box -
    // `0.0…0.22` for the name, `0.22…1.0` for the badge - measured on
    // 2026-09-07 against a 21x30 mini card. Two days later 5e700679 ("opponent
    // fans you can read") grew that card to 28x40 for the owner, the taller
    // inner stack pushed the fan UP inside the centred box, and row 48 - the
    // old 0.22 - stopped being the gap and became the seventh row of card
    // backs. Every measurement here then read name PLUS fan: the live name
    // "lost 60% of its ink" and "spans 58px against a live 136px" against a
    // product that had not changed at all, and the band guard caught it exactly
    // as its own docstring promised ("a layout change moving the clumps fails
    // loudly here instead of quietly measuring nothing"). It failed loudly, and
    // it was the test that was wrong.
    //
    // A number re-measured today would rot again the next time the owner asks
    // for a bigger card. So the split is DERIVED: the badge draws in two
    // separated clumps of rows, and the boundary is the first blank row after
    // the top one. Measured today, `handCount: 5`, a 140x110 box at scale 2
    // (280x220 px):
    //
    //     rows  11…33   the name ("Boris", 12pt semibold)     995 px
    //     rows  34…41   nothing                                 -
    //     rows  42…121  the mini fan + the count chip        10860 px
    //
    // so the split lands on row 34 wherever the clumps move to. The guards are
    // kept and sharpened: a layout that closes the gap, or that leaves either
    // clump empty, fails here with a sentence about the bitmap rather than
    // silently measuring nothing. `testTheBandsAreWhereTheContentIs` then
    // checks the split from the far side, against a render this derivation
    // never looked at.

    /// Ink per row: how many pixels in each row of `img` are not the
    /// (transparent) ground the badge was rendered against. Alpha, not colour:
    /// an ImageRenderer with no background paints nothing where the view drew
    /// nothing, so "did the name get drawn" is exact rather than a judgement
    /// about shades.
    private func rowInk(_ img: UIImage) throws -> [Int] {
        let (px, w, h) = try pixels(img)
        return (0..<h).map { y in
            (0..<w).reduce(0) { $0 + (px[(y * w + $1) * 4 + 3] > Self.alphaFloor ? 1 : 0) }
        }
    }

    /// The name rows and the badge rows of a badge bitmap, split at the blank
    /// gap between the two clumps of ink.
    private func bands(_ img: UIImage) throws -> (name: Range<Int>, badge: Range<Int>) {
        let rows = try rowInk(img)
        let top = try XCTUnwrap(rows.firstIndex { $0 > 0 },
                                "the badge drew nothing at all")
        let split = try XCTUnwrap(rows[top...].firstIndex { $0 == 0 },
                                  "no blank row under the name - the name and the fan have "
                                  + "run together and these bands cannot separate them")
        // Both clumps have to be real, or a band is measuring nothing and every
        // ratio below is meaningless (an early cut of this file did exactly
        // that and read as a pass).
        XCTAssertGreaterThanOrEqual(split - top, 8,
                                    "the name clump is \(split - top) rows - too thin to be a name")
        let gapEnd = try XCTUnwrap(rows[split...].firstIndex { $0 > 0 },
                                   "nothing is drawn below the name - where did the fan go?")
        XCTAssertGreaterThanOrEqual(gapEnd - split, 2,
                                    "the gap between the name and the fan is \(gapEnd - split) "
                                    + "row(s) - too tight to split on safely")
        return (top..<split, split..<rows.count)
    }

    /// Pixels in `rows` that are not the ground.
    private func ink(_ img: UIImage, rows band: Range<Int>) throws -> Int {
        let profile = try rowInk(img)
        return band.clamped(to: 0..<profile.count).reduce(0) { $0 + profile[$1] }
    }

    /// THE REPORT. The name is still drawn - and drawn at its full WIDTH -
    /// after the seat goes out.
    ///
    /// Width, not merely presence, is what pins it: the collapse is a
    /// horizontal scale about the centre, so a name half way through the
    /// gesture is a narrow smear of ink in the middle of the band. Comparing
    /// the horizontal extent against the live badge's is the assertion the old
    /// code cannot satisfy at any point of the animation.
    func testAnOutSeatKeepsItsNameAtFullWidth() throws {
        let live = try XCTUnwrap(render(collapsed: false))
        let out  = try XCTUnwrap(render(collapsed: true))
        // The collapse is a SCALE, so it takes no layout room away: the name
        // sits on the same rows in both bitmaps and the live render's split
        // reads the out one too.
        let nameRows = try bands(live).name

        let liveName = try ink(live, rows: nameRows)
        let outName  = try ink(out,  rows: nameRows)
        XCTAssertGreaterThan(liveName, 200, "the live name did not render at all - bad band")
        XCTAssertGreaterThan(outName, 0, "the out player's name vanished from the board")

        // Dimmed, so fewer pixels clear the alpha floor is legitimate - but not
        // by much, and never by the order of magnitude a horizontal collapse
        // costs (0.001 of the width). Measured: 725/995 = 0.73.
        XCTAssertGreaterThan(Double(outName) / Double(liveName), 0.5,
                             "the out name lost \(100 - 100 * outName / liveName)% of its ink - "
                             + "it is being squeezed, not dimmed")

        // AND THE WIDTH, AS A RATIO - not an equality with a pixel tolerance.
        //
        // The dim is what makes an equality wrong here. At 0.6 opacity the
        // glyphs' anti-aliased outer columns drop under `alphaFloor`, so the
        // measured extent shrinks by a pixel or so at each end with the name
        // sitting perfectly still: measured 61px live against 58px out, which
        // an `accuracy: 2` equality fails on for no defect at all.
        //
        // A collapse is not that. `badgeTurn(collapsed:)` is 0.001, so a
        // collapsed name is one or two columns of ink at the centre - a ratio
        // near 0.02, two orders of magnitude below the 0.95 that dimming
        // leaves. 0.9 sits in that gap with room on both sides: wide enough
        // that anti-aliasing can never reach it, far enough above a collapse
        // that no part of the gesture can sneak past.
        let liveW = try extent(live, rows: nameRows).width
        let outW  = try extent(out,  rows: nameRows).width
        XCTAssertGreaterThan(liveW, 20, "the live name did not render at all - bad band")
        XCTAssertGreaterThan(outW / liveW, 0.9,
                             "the out name spans \(outW)px against a live \(liveW)px - "
                             + "it turned, it did not dim")
    }

    /// …and the BADGE under it did turn. Half the rule: a fix that simply
    /// stopped collapsing anything would pass the test above.
    func testTheBadgeItselfStillTurnsAway() throws {
        let live = try XCTUnwrap(render(collapsed: false))
        let out  = try XCTUnwrap(render(collapsed: true))
        let badgeRows = try bands(live).badge
        let liveBadge = try ink(live, rows: badgeRows)
        let outBadge  = try ink(out,  rows: badgeRows)
        XCTAssertGreaterThan(liveBadge, 200, "the live badge did not render - bad band")
        XCTAssertLessThan(Double(outBadge) / Double(liveBadge), 0.1,
                          "the badge did not turn edge-on when the seat went out")
    }

    /// THE SPLIT ITSELF, SEEN FROM THE FAR SIDE, since the two tests above are
    /// only as good as the rows they read - and the way this file failed the
    /// first time was a band that contained the fan, which reads as "the name
    /// was squeezed" just as well as a name that was squeezed.
    ///
    /// Dropping the hand to zero removes the fan and the chip and leaves the
    /// name alone. That render is not what the split was derived from, so it is
    /// an independent check on it: the name band must keep everything it had,
    /// and the badge band must go to exactly nothing. A split too low catches
    /// fan ink in the name band and the first assertion fails; a split too high
    /// leaves name ink in the badge band and the last one does.
    func testTheBandsAreWhereTheContentIs() throws {
        let withHand = try XCTUnwrap(render(handCount: 5, collapsed: false))
        let empty    = try XCTUnwrap(render(handCount: 0, collapsed: false))
        let (nameRows, badgeRows) = try bands(withHand)

        XCTAssertEqual(try ink(withHand, rows: nameRows),
                       try ink(empty, rows: nameRows),
                       "the name band moved with the hand count - it is reading the fan")
        XCTAssertGreaterThan(try ink(withHand, rows: badgeRows), 200,
                             "the badge band is empty on a seat holding five cards")
        XCTAssertEqual(try ink(empty, rows: badgeRows), 0,
                       "the badge band caught ink from a seat with no cards and no role - "
                       + "it is reading the name")
    }

    /// The horizontal extent of the ink in a band: where it starts, where it
    /// ends. A collapsed view leaves a sliver at the centre; an upright one
    /// spans the word.
    private func extent(_ img: UIImage, rows band: Range<Int>) throws -> CGRect {
        let (px, w, h) = try pixels(img)
        var lo = w, hi = -1
        for y in band.clamped(to: 0..<h) {
            for x in 0..<w where px[(y * w + x) * 4 + 3] > Self.alphaFloor {
                lo = min(lo, x); hi = max(hi, x)
            }
        }
        guard hi >= lo else { return .zero }
        return CGRect(x: lo, y: band.lowerBound, width: hi - lo + 1, height: band.count)
    }

    /// The bitmap behind both measurements, drawn once per call into a plain
    /// RGBA buffer so `ink` and `extent` cannot disagree about what a pixel is.
    private func pixels(_ img: UIImage) throws -> ([UInt8], Int, Int) {
        let cg = try XCTUnwrap(img.cgImage)
        let w = cg.width, h = cg.height
        var px = [UInt8](repeating: 0, count: w * h * 4)
        let ctx = try XCTUnwrap(CGContext(data: &px, width: w, height: h, bitsPerComponent: 8,
                                          bytesPerRow: w * 4, space: CGColorSpaceCreateDeviceRGB(),
                                          bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue))
        ctx.draw(cg, in: CGRect(x: 0, y: 0, width: w, height: h))
        return (px, w, h)
    }

    /// THE DIM IS A DIM, not the 0.45 that round 16 threw out. The out name
    /// already says "out" with its INK (`nameInk`, drawn at full strength);
    /// this only has to read as a step down, and a value that takes it back
    /// under the round-16 floor is the same defect returning.
    func testTheDimStaysAboveTheRoundSixteenFloor() {
        XCTAssertEqual(FSeatBadge.nameOpacity(collapsed: false), 1.0, accuracy: 0.0001,
                       "a live seat's name is not dimmed at all")
        XCTAssertLessThan(FSeatBadge.nameOpacity(collapsed: true), 1.0, "it must read as out")
        XCTAssertGreaterThan(FSeatBadge.nameOpacity(collapsed: true), 0.45,
                             "round 16: 0.45 is the opacity the owner could not see")
        // And the ink under it is still opaque - the dim must not have been
        // smuggled into the colour, where round 16's test would not see it.
        for scheme in [ColorScheme.light, .dark] {
            let a = UIColor(FSeatBadge.nameInk(isOut: true, onLight: false, scheme: scheme))
                .cgColor.alpha
            XCTAssertEqual(a, 1.0, accuracy: 0.001)
        }
    }

    /// The badge turn is a SCALE that keeps its reserved width - 0.001, not 0,
    /// for the reason round 28 wrote down: a view scaled to exactly zero can
    /// stop being laid out, taking the ring's spacing with it.
    func testTheTurnIsNeverExactlyZero() {
        XCTAssertEqual(FSeatBadge.badgeTurn(collapsed: false), 1.0, accuracy: 0.0001)
        XCTAssertGreaterThan(FSeatBadge.badgeTurn(collapsed: true), 0)
        XCTAssertLessThan(FSeatBadge.badgeTurn(collapsed: true), 0.01)
    }
}
