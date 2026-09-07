// ROUND 41: A PLAYER WHO IS OUT KEEPS THEIR NAME.
//
// The owner, off a screenshot of the iMessage board: "when a player gets out,
// their name rotates out along with the badge? Not ideal. Rotate only the badge,
// dim the name."
//
// Round 28 hung the out collapse on the WHOLE seat label - `.scaleEffect(x:)` on
// the outer VStack - so the name went edge-on with the hand and the role mark.
// Filmed on the rig (HARNESS_SCENARIO=arrival ARRIVE_KIND=gameover PLAYERS=4),
// the name "Boris" squeezes horizontally over about ten frames and is gone: a
// four-seat ring ends up showing two names and two empty spaces.
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
    private func render(collapsed: Bool, scheme: ColorScheme = .dark) -> UIImage? {
        let badge = FSeatBadge(name: "Boris", handCount: 0, isOut: true,
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
        let cg = try XCTUnwrap(img.cgImage)
        let w = cg.width, h = cg.height
        var px = [UInt8](repeating: 0, count: w * h * 4)
        let ctx = try XCTUnwrap(CGContext(data: &px, width: w, height: h, bitsPerComponent: 8,
                                          bytesPerRow: w * 4, space: CGColorSpaceCreateDeviceRGB(),
                                          bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue))
        ctx.draw(cg, in: CGRect(x: 0, y: 0, width: w, height: h))
        let y0 = Int(Double(h) * band.lowerBound), y1 = Int(Double(h) * band.upperBound)
        var n = 0
        for y in max(0, y0)..<min(h, y1) {
            for x in 0..<w where px[(y * w + x) * 4 + 3] > 40 { n += 1 }
        }
        return n
    }

    /// The name sits in the TOP of the label (the fan and the role row are
    /// under it), so the top fifth is the name and nothing else.
    private static let nameBand = 0.0...0.2
    /// …and the rest is the badge: the mini fan, the count chip, the role mark.
    private static let badgeBand = 0.25...1.0

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
        // costs (0.001 of the width).
        XCTAssertGreaterThan(Double(outName) / Double(liveName), 0.5,
                             "the out name lost \(100 - 100 * outName / liveName)% of its ink - "
                             + "it is being squeezed, not dimmed")
        XCTAssertEqual(try extent(out, band: Self.nameBand).width,
                       try extent(live, band: Self.nameBand).width, accuracy: 2,
                       "the out name is not the same width as a live one - it turned")
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

    /// The horizontal extent of the ink in a band: where it starts, where it
    /// ends. A collapsed view leaves a sliver at the centre; an upright one
    /// spans the word.
    private func extent(_ img: UIImage, band: ClosedRange<Double>) throws -> CGRect {
        let cg = try XCTUnwrap(img.cgImage)
        let w = cg.width, h = cg.height
        var px = [UInt8](repeating: 0, count: w * h * 4)
        let ctx = try XCTUnwrap(CGContext(data: &px, width: w, height: h, bitsPerComponent: 8,
                                          bytesPerRow: w * 4, space: CGColorSpaceCreateDeviceRGB(),
                                          bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue))
        ctx.draw(cg, in: CGRect(x: 0, y: 0, width: w, height: h))
        let y0 = Int(Double(h) * band.lowerBound), y1 = Int(Double(h) * band.upperBound)
        var lo = w, hi = -1
        for y in max(0, y0)..<min(h, y1) {
            for x in 0..<w where px[(y * w + x) * 4 + 3] > 40 {
                lo = min(lo, x); hi = max(hi, x)
            }
        }
        guard hi >= lo else { return .zero }
        return CGRect(x: lo, y: y0, width: hi - lo + 1, height: y1 - y0)
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
