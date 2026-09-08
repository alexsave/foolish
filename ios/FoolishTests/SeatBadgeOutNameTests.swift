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

    /// Pixels in `band` that are not the (transparent) ground the badge was
    /// rendered against. Alpha, not colour: an ImageRenderer with no background
    /// paints nothing where the view drew nothing, so "did the name get drawn"
    /// is exact rather than a judgement about shades.
    private func ink(_ img: UIImage, band: ClosedRange<Double>) throws -> Int {
        let (px, w, h) = try pixels(img)
        let y0 = Int(Double(h) * band.lowerBound), y1 = Int(Double(h) * band.upperBound)
        var n = 0
        for y in max(0, y0)..<min(h, y1) {
            for x in 0..<w where px[(y * w + x) * 4 + 3] > Self.alphaFloor { n += 1 }
        }
        return n
    }

    /// Anything above this is ink the view drew; the ground is a hard 0.
    private static let alphaFloor: UInt8 = 40

    // THE BANDS, MEASURED RATHER THAN GUESSED.
    //
    // A 140x110 box at scale 2 is a 280x220 bitmap, and the badge draws in two
    // separated clumps of rows (per-row ink counts, `handCount: 5`):
    //
    //     rows  18…40   the name  ("Boris", 12pt semibold)   ~995 px
    //     rows  41…54   nothing
    //     rows  55…114  the mini fan + the count chip        ~5390 px
    //
    // So 0.22 (row 48) is the gap between them, with six rows of slack either
    // side. These fractions are not free-hand: an earlier cut of this file
    // guessed `0.25…1.0` for the badge AND rendered a zero-card seat, which
    // made the badge band empty and the test's own ratio a NaN. Both live
    // guards below (`> 200`) exist so that a layout change moving the clumps
    // fails loudly here instead of quietly measuring nothing.
    private static let nameBand = 0.0...0.22
    private static let badgeBand = 0.22...1.0

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

        let liveName = try ink(live, band: Self.nameBand)
        let outName  = try ink(out,  band: Self.nameBand)
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
        let liveW = try extent(live, band: Self.nameBand).width
        let outW  = try extent(out,  band: Self.nameBand).width
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
        let liveBadge = try ink(live, band: Self.badgeBand)
        let outBadge  = try ink(out,  band: Self.badgeBand)
        XCTAssertGreaterThan(liveBadge, 200, "the live badge did not render - bad band")
        XCTAssertLessThan(Double(outBadge) / Double(liveBadge), 0.1,
                          "the badge did not turn edge-on when the seat went out")
    }

    /// THE BANDS THEMSELVES, since the two tests above are only as good as the
    /// rows they read - and the way this file failed the first time was a band
    /// that contained nothing, which reads as "the badge turned" just as well
    /// as a badge that turned.
    ///
    /// Dropping the hand to zero removes the fan and the chip and leaves the
    /// name alone, so it separates the two bands from the far side: the name
    /// band must keep everything it had, and the badge band must go to exactly
    /// nothing. Anything else means the split row is in the wrong place.
    func testTheBandsAreWhereTheContentIs() throws {
        let withHand = try XCTUnwrap(render(handCount: 5, collapsed: false))
        let empty    = try XCTUnwrap(render(handCount: 0, collapsed: false))

        XCTAssertEqual(try ink(withHand, band: Self.nameBand),
                       try ink(empty, band: Self.nameBand),
                       "the name band moved with the hand count - it is reading the fan")
        XCTAssertGreaterThan(try ink(withHand, band: Self.badgeBand), 200,
                             "the badge band is empty on a seat holding five cards")
        XCTAssertEqual(try ink(empty, band: Self.badgeBand), 0,
                       "the badge band caught ink from a seat with no cards and no role - "
                       + "it is reading the name")
    }

    /// The horizontal extent of the ink in a band: where it starts, where it
    /// ends. A collapsed view leaves a sliver at the centre; an upright one
    /// spans the word.
    private func extent(_ img: UIImage, band: ClosedRange<Double>) throws -> CGRect {
        let (px, w, h) = try pixels(img)
        let y0 = Int(Double(h) * band.lowerBound), y1 = Int(Double(h) * band.upperBound)
        var lo = w, hi = -1
        for y in max(0, y0)..<min(h, y1) {
            for x in 0..<w where px[(y * w + x) * 4 + 3] > Self.alphaFloor {
                lo = min(lo, x); hi = max(hi, x)
            }
        }
        guard hi >= lo else { return .zero }
        return CGRect(x: lo, y: y0, width: hi - lo + 1, height: y1 - y0)
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
