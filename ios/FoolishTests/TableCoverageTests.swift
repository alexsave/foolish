// TableCoverageTests.swift — round 16 #1: the table surface covers whatever it
// is given, at every size, in both materials and both schemes.
//
// The defect this pins: `TableWeave` drew ONE baked picture at ONE fixed
// magnification (592x1280 texels x 0.775 = 458.6 x 991.6pt) and callers laid it
// over a flat fallback colour, so a surface larger than that rectangle in
// either axis showed a slab of flat colour where the picture ran out —
// reported as "the wool/felt texture simply does not cover the entire screen,
// the bottom was just some flat solid color". A probe of the old code measured
// 108 uncovered rows at 402x1100 (1100 - 991.6) and 42 uncovered columns at
// 500x900 (500 - 458.6).
//
// Measured as TRANSPARENCY, which is what makes it exact rather than a
// judgement about how textured a pixel looks: `TableWeave` draws the picture
// and NOTHING else, so a pixel it does not cover is not "flat", it is empty.
// No threshold, no per-material tuning (the four bakes differ by an order of
// magnitude in how loud they are — the plaid wool against the near-smooth dark
// baize — so any texture-energy measure needs a threshold per bake and gets it
// wrong the moment a fifth material lands).
//
// The sizes run past anything that ships on purpose: the point of the fix is
// that the surface no longer depends on the bake being bigger than the screen,
// so a taller phone, an iPad, or a box that momentarily overshoots during a
// transition cannot bring the flat slab back.

import XCTest
import SwiftUI
@testable import FoolishKit

@MainActor
final class TableCoverageTests: XCTestCase {

    private func render<V: View>(_ v: V, _ size: CGSize, _ scheme: ColorScheme) -> UIImage? {
        let r = ImageRenderer(content: v.frame(width: size.width, height: size.height)
            .environment(\.colorScheme, scheme))
        r.scale = 1
        return r.uiImage
    }

    /// Pixels the weave did not paint, and where they are.
    private func uncovered(_ img: UIImage) throws -> (count: Int, rows: Int, cols: Int) {
        let cg = try XCTUnwrap(img.cgImage)
        let w = cg.width, h = cg.height
        var px = [UInt8](repeating: 0, count: w * h * 4)
        let ctx = try XCTUnwrap(CGContext(data: &px, width: w, height: h, bitsPerComponent: 8,
                                          bytesPerRow: w * 4, space: CGColorSpaceCreateDeviceRGB(),
                                          bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue))
        ctx.draw(cg, in: CGRect(x: 0, y: 0, width: w, height: h))

        // 250, not 255: the outermost pixel of a resampled image can carry a
        // hair of edge softness. A missing SLAB is thousands of pixels at
        // alpha 0, so the distinction is never close.
        var count = 0
        var rows = Set<Int>(), cols = Set<Int>()
        for y in 0..<h {
            for x in 0..<w where px[(y * w + x) * 4 + 3] < 250 {
                count += 1; rows.insert(y); cols.insert(x)
            }
        }
        return (count, rows.count, cols.count)
    }

    private static let sizes: [(String, CGSize)] = [
        ("SE 375x667",         CGSize(width: 375, height: 667)),
        ("iPhone 17 402x874",  CGSize(width: 402, height: 874)),
        ("Pro Max 440x956",    CGSize(width: 440, height: 956)),
        // Past the bake in one axis, then the other, then both.
        ("taller than the bake 402x1100", CGSize(width: 402, height: 1100)),
        ("wider than the bake 500x900",   CGSize(width: 500, height: 900)),
        ("iPad-ish 1024x1180",            CGSize(width: 1024, height: 1180)),
    ]

    func testTheWeaveCoversEverySurfaceInBothMaterials() throws {
        let saved = FPrefs.shared.table
        defer { FPrefs.shared.setTable(saved) }

        for material in TableSurface.allCases {
            FPrefs.shared.setTable(material)
            for scheme in [ColorScheme.light, .dark] {
                for (label, size) in Self.sizes {
                    let img = try XCTUnwrap(render(TableWeave(), size, scheme), "render \(label)")
                    let gap = try uncovered(img)
                    XCTAssertEqual(gap.count, 0,
                                   "\(material) \(scheme) \(label): \(gap.count) unpainted pixels " +
                                   "across \(gap.rows) rows and \(gap.cols) columns")
                }
            }
        }
    }

    /// The cover floor is a FLOOR: a surface the bake already covers is drawn at
    /// exactly `pointsPerTexel`, so the threads are the same size on the message
    /// bubble, the compact drawer and the full-screen board — round-6 #14,
    /// which is the thing the floor must not undo.
    ///
    /// Measured, not argued: two surfaces of very different sizes are rendered
    /// and the same patch of weave is compared pixel for pixel. If either one
    /// scaled, they diverge.
    func testSurfacesInsideTheBakeShareOneMagnification() throws {
        let saved = FPrefs.shared.table
        defer { FPrefs.shared.setTable(saved) }
        FPrefs.shared.setTable(.wool)

        let small = try XCTUnwrap(render(TableWeave(), CGSize(width: 300, height: 195), .light))
        let large = try XCTUnwrap(render(TableWeave(), CGSize(width: 402, height: 874), .light))

        // Both are windows onto the same picture, anchored to its bottom edge
        // and centred horizontally, so the same 200x150 bottom-CENTRE patch is
        // the same pixels. (Centre, not left: two differently sized windows
        // centred on one picture do not share a left edge.)
        func patch(_ img: UIImage) throws -> [UInt8] {
            let cg = try XCTUnwrap(img.cgImage)
            let rect = CGRect(x: (cg.width - 200) / 2, y: cg.height - 150, width: 200, height: 150)
            let cut = try XCTUnwrap(cg.cropping(to: rect))
            var px = [UInt8](repeating: 0, count: 200 * 150 * 4)
            let ctx = try XCTUnwrap(CGContext(data: &px, width: 200, height: 150, bitsPerComponent: 8,
                                              bytesPerRow: 200 * 4, space: CGColorSpaceCreateDeviceRGB(),
                                              bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue))
            ctx.draw(cut, in: CGRect(x: 0, y: 0, width: 200, height: 150))
            return px
        }
        let a = try patch(small), b = try patch(large)
        let diff = zip(a, b).map { abs(Double($0) - Double($1)) }
        let mean = diff.reduce(0, +) / Double(diff.count)
        print("PROBE magnification meanDiff=\(mean) max=\(diff.max() ?? 0)")
        XCTAssertLessThan(mean, 1.0,
                          "the same weave drawn at two surface sizes is no longer the same picture")
    }
}
