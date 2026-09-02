// TrumpGlyphTests.swift - round 16: "Top left corner Trump indicator is a bit
// low. The 'distance from top' and 'distance from left' should be equal. Play
// around with this for different suits as they can be funny."
//
// The indicator is FDeckWell's bare trump glyph - the mark that replaces the
// stock and the flipped card once the deck is drawn out. It was placed with a
// plain `.offset(x: inset, y: inset)`, which insets the glyph's TEXT BOX, not
// the glyph. A text box carries the font's whole ascent above the ink, so the
// mark sat far lower than it sat right; and because every suit's ink starts at
// a different height and side bearing in Georgia, each of the four was wrong by
// a DIFFERENT amount ("they can be funny").
//
// Measured off the rendered pixels rather than off font metrics on purpose: the
// metric arithmetic (ascent, leading, side bearing, synthetic vs real bold) is
// exactly the part that is easy to get plausibly wrong, so the assertion is the
// picture the owner is actually looking at. Ink is alpha >= 191, which is above
// anything the glyph's 0.6-opacity blurred shadow can reach and below the solid
// glyph body, so the shadow cannot widen the measured box.

import XCTest
import SwiftUI
@testable import FoolishKit

@MainActor
final class TrumpGlyphTests: XCTestCase {

    /// Sub-point precision: the whole question is a few points, so measure well
    /// above device scale and divide, rather than quantising the answer to the
    /// thing being measured. High rather than merely 3x because a solid-ink
    /// threshold clips the first pixel row or two of a POINTED extremity (the
    /// spade's spike loses more than the club's shoulder), and that constant
    /// pixel loss is worth a third of a point at 3x but a twentieth at 12x.
    private let scale: CGFloat = 12

    private func inkBox(_ suit: Suit, _ scheme: ColorScheme) throws -> CGRect {
        let well = FDeckWell(deckCount: 0, flipped: nil, hasFlipped: false, trumpSuit: suit)
        let r = ImageRenderer(content: well.environment(\.colorScheme, scheme))
        r.scale = scale
        let cg = try XCTUnwrap(r.uiImage?.cgImage, "the deck well rendered nothing")
        let w = cg.width, h = cg.height
        var px = [UInt8](repeating: 0, count: w * h * 4)
        let ctx = try XCTUnwrap(CGContext(data: &px, width: w, height: h, bitsPerComponent: 8,
                                          bytesPerRow: w * 4, space: CGColorSpaceCreateDeviceRGB(),
                                          bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue))
        ctx.draw(cg, in: CGRect(x: 0, y: 0, width: w, height: h))

        var minX = w, maxX = -1, minY = h, maxY = -1
        for y in 0..<h {
            for x in 0..<w where px[(y * w + x) * 4 + 3] >= 191 {
                minX = min(minX, x); maxX = max(maxX, x)
                minY = min(minY, y); maxY = max(maxY, y)
            }
        }
        XCTAssertGreaterThanOrEqual(maxX, 0, "no trump glyph was drawn for \(suit)")
        return CGRect(x: CGFloat(minX) / scale, y: CGFloat(minY) / scale,
                      width: CGFloat(maxX - minX + 1) / scale,
                      height: CGFloat(maxY - minY + 1) / scale)
    }

    /// The owner's rule, verbatim: distance from the top == distance from the
    /// left, for every suit. Half a point of tolerance - that is one pixel at
    /// the measuring scale, i.e. the finest answer the picture can give.
    func testEverySuitSitsAsFarFromTheTopAsFromTheLeft() throws {
        var report: [String] = []
        for suit in Suit.allCases {
            for scheme in [ColorScheme.dark, .light] {
                let ink = try inkBox(suit, scheme)
                report.append(String(format: "%@ %@ left=%.2f top=%.2f (%.1fx%.1f)",
                                     suit.glyph, scheme == .dark ? "dark" : "light",
                                     ink.minX, ink.minY, ink.width, ink.height))
                XCTAssertEqual(ink.minY, ink.minX, accuracy: 0.5,
                               "\(suit.glyph) sits \(ink.minY)pt from the top but \(ink.minX)pt from the left")
            }
        }
        print("TRUMP GLYPH INK:\n" + report.joined(separator: "\n"))
    }

    /// …and that shared distance is the deck well's own inset, the one every
    /// other state of this corner anchors to (`FDeckWell.bottomCardOrigin`), so
    /// the mark lines up with the stock it replaces instead of merely being
    /// square with itself somewhere else.
    func testThatDistanceIsTheSameInsetTheStockUses() throws {
        for suit in Suit.allCases {
            let ink = try inkBox(suit, .dark)
            XCTAssertEqual(ink.minX, FDeckWell.bottomCardOrigin.x, accuracy: 0.5,
                           "\(suit.glyph) is not flush with the stock's own inset")
        }
    }

    /// The mark must still FIT the well it lives in (92x108) - an ink-aligned
    /// offset that pushed a tall glyph off the bottom would trade one crop for
    /// another.
    func testTheMarkStaysInsideTheWell() throws {
        for suit in Suit.allCases {
            let ink = try inkBox(suit, .dark)
            XCTAssertLessThanOrEqual(ink.maxX, 92, "\(suit.glyph) overflows the well's width")
            XCTAssertLessThanOrEqual(ink.maxY, 108, "\(suit.glyph) overflows the well's height")
        }
    }
}
