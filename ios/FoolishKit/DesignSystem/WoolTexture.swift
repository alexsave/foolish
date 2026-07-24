// WoolTexture.swift — the woven-wool table surface (§IOS_PHONE_LAYOUT §4:
// "the phone app should lean on what the website does — wool/wood/fern").
// A faithful CPU port of the web's WoolBackground generator (the CPU-fallback
// path, which writes a flat RGBA buffer — the same shape as our UInt8 buffer):
// a brown base woven by a horizontal fiber phase then a vertical one, with a
// tan-XOR plaid modulating the colour. The math is ported, not the WebGL.
//
// THIS FILE NO LONGER RUNS IN THE SHIPPING APP. It is the SOURCE OF TRUTH for
// what the wool looks like, and it is executed at BUILD time by
// ios/Tools/GenerateTextures.swift, which bakes one image per entry in `bakes`
// into FoolishKit/Resources/ (wool-classic.jpg, wool-dark-green.jpg,
// wool-dark-navy.jpg). The extension then loads whichever one the colour scheme
// calls for (FTextures) and generates zero procedural pixels on launch.
//
// Why: a 1920x1080 weave is ~2.4M brush iterations, each writing up to a 5x5
// span — tens of millions of blends and an 8.3MB scratch buffer — on the first
// launch of a process that iOS memory- and watchdog-caps far below an app. It
// was a live suspect for the round-5 "the extension comes up as a dark, empty
// panel on a real phone" report, and a disk cache does not help the launch that
// pays for it. Build-time rendering removes the cost as a CLASS: there is no
// first launch that renders.
//
// TO CHANGE THE LOOK: edit `render` / `Palette` here, then run
//   ios/Tools/regenerate_textures.sh
// and commit the regenerated images. Nothing else in the app reads this code.
//
// Deliberately UIKit-free (CoreGraphics only) so the macOS build-time tool can
// compile this exact file — one generator, no port to drift.

import CoreGraphics
import Foundation

public enum WoolTexture {

    // MARK: - Palette (the ONE place wool colour lives)

    /// Every colour the weave uses. A dark-mode wool is a second `Palette` plus
    /// a second output file (see `bakes` and FTextures.Variant) — NOT a
    /// second copy of the generator, and never a tint applied at draw time.
    public struct Palette {
        /// The brown showing between fibres (web BASE_R/G/B).
        public let baseR, baseG, baseB: Double
        /// Colour centre of the horizontal (weft) pass.
        public let weftR, weftG, weftB: Double
        /// Colour centre of the vertical (warp) pass.
        public let warpR, warpG, warpB: Double
        /// How far a fibre's colour swings with its phase, per channel.
        public let swingR, swingG, swingB: Double
        /// The tan-XOR plaid shift — what makes the ~1cm chequer blocks read —
        /// as a signed PER-CHANNEL delta added to the fibre colour inside a
        /// block and nothing outside it.
        ///
        /// It used to be a single `plaid: Double` applied as (+p, -p, -p/2),
        /// i.e. hard-wired to swing the blocks toward RED. That is the web's
        /// look and it is why the light wool's chequer is pink, but it makes
        /// "the wool red colour becomes green or navy" impossible to express:
        /// the direction of the shift was a constant of the generator rather
        /// than a property of the palette. Three signed numbers say the same
        /// thing with the hue included — `classic` below is the old
        /// (+100, -100, -50) written out, so the light bake is unchanged to
        /// the byte.
        public let plaidR, plaidG, plaidB: Double
        /// The flat colour a wool surface sits on before the weave is drawn
        /// (and all it shows if the resource is ever missing). Same contract as
        /// `WoodTexture.Palette.fallbackHex`: close to the weave's own average
        /// so a failure is dull, not wrong. NOT a generator input.
        public let fallbackHex: UInt32

        public init(baseR: Double, baseG: Double, baseB: Double,
                    weftR: Double, weftG: Double, weftB: Double,
                    warpR: Double, warpG: Double, warpB: Double,
                    swingR: Double, swingG: Double, swingB: Double,
                    plaidR: Double, plaidG: Double, plaidB: Double,
                    fallbackHex: UInt32) {
            self.baseR = baseR; self.baseG = baseG; self.baseB = baseB
            self.weftR = weftR; self.weftG = weftG; self.weftB = weftB
            self.warpR = warpR; self.warpG = warpG; self.warpB = warpB
            self.swingR = swingR; self.swingG = swingG; self.swingB = swingB
            self.plaidR = plaidR; self.plaidG = plaidG; self.plaidB = plaidB
            self.fallbackHex = fallbackHex
        }

        /// The shipped light wool — the web's numbers, unchanged.
        public static let classic = Palette(
            baseR: 113, baseG: 65,  baseB: 27,
            weftR: 209, weftG: 208, weftB: 183,
            warpR: 189, warpG: 188, warpB: 163,
            swingR: 46, swingG: 45,  swingB: 53,
            plaidR: 100, plaidG: -100, plaidB: -50,
            fallbackHex: 0xF5E6C8)

        // MARK: dark mode
        //
        // The look landed over four revisions. Round-6: "beige -> brown". Round-7
        // first: "dark grey ... navy chequer". Round-7 second: "dark gray and
        // light gray, both with very slight brown tint" - a two-tone grey. That
        // read "too grayscale", so round-7 FINAL (the shipped one) puts the HUE
        // back, dark: "dark brown for the beige color, and the deep red for the
        // bright red color" - the same two roles the LIGHT wool has (a brown
        // fibre field, a red plaid block), only dimmed for a dark table.
        //
        // The FIELD is a dark brown fibre (~92,60,36) between near-black brown
        // gaps (~28,18,11); the PLAID BLOCKS shift to the deep red (~0x8B1A1A =
        // 139,26,26) via a signed plaid delta off the fibre, mirroring how the
        // light palette's plaid pushes its beige toward red - here it is a
        // darker red on a darker brown so the chequer reads without glowing. The
        // swing is warm (R>G>B) and small so the weave has grain, not noise.
        public static let dark = Palette(
            baseR: 28, baseG: 18, baseB: 11,
            weftR: 92, weftG: 60, weftB: 36,
            warpR: 82, warpG: 53, warpB: 32,
            swingR: 20, swingG: 14, swingB: 9,
            plaidR: 47, plaidG: -34, plaidB: -10,
            fallbackHex: 0x3D2818)
    }

    // MARK: - The dark palette

    /// The dark-mode wool. One palette now: the owner settled on a two-tone
    /// grey plaid (dark grey field, light grey blocks, both faintly warm), so
    /// the earlier green-vs-navy accent choice is gone.
    public static var darkPalette: Palette { .dark }

    // MARK: - The shipped swatch

    /// THE canvas the web generates wool on (src/components/WoolBackground.tsx:
    /// both the WebGL path and the CPU fallback, and the fallback says 1920x1080
    /// literally). Landscape, and that matters: the generator's fibre phases and
    /// its iteration budget are both written in pixels of THIS shape, so a
    /// portrait render of the same code is a different weave, not the same weave
    /// rotated.
    ///
    /// We render 1312 rows rather than the web's 1080 for one reason: the phone
    /// only ever SEES a portrait sliver of this (see `shippedCrop`), and at the
    /// one fixed magnification below, the tallest iPhone needs more than 1080
    /// rows of weave to reach its own bottom edge. Row count is the only thing
    /// that changes; every constant in `render` is absolute pixels, so the
    /// weave itself is the same weave.
    public static let renderCanvas = (w: 1920, h: 1312)

    /// The sub-rectangle of `renderCanvas` that actually ships.
    ///
    /// A phone shows about 590 of the canvas's 1920 columns (a portrait window
    /// on a landscape weave — the web crops the same way), so shipping the full
    /// canvas would be ~3x the bytes and ~3x the decoded memory for pixels no
    /// device can display. The 64/16 origin skips the canvas edges, where the
    /// fibre passes start mid-stroke and cover thinly.
    ///
    /// Sized from `pointsPerTexel`: 592 x 1280 texels is 458.6 x 991.6pt, which
    /// covers the largest iPhone stage (440 x 956pt, safe areas included) with
    /// margin to spare.
    ///
    /// PORTRAIT ONLY, deliberately. The host app is portrait-locked
    /// (FoolishApp/Info.plist) and the board layout is portrait-tuned (8-seat
    /// arc, two-row hand, full-width plank), so a landscape stage - which the
    /// Messages host can still hand the extension - would show the beige
    /// fallback beside a 458pt-wide weave. Covering landscape too means a
    /// 1248 x 1280 crop: measured, that is 1277 KB shipped and 6 MB decoded
    /// against this one's 620 KB and 3 MB, and halving the extension's texture
    /// memory is the whole point of round-6 #16. If landscape ever matters,
    /// widen `w` to 1248 here and re-bake - nothing else changes.
    public static let shippedCrop = (x: 64, y: 16, w: 592, h: 1280)

    /// Base name of the baked LIGHT image in FoolishKit's bundle.
    public static let classicResourceName = "wool-classic"

    /// Base name of the baked DARK image. One dark weave now (the two-tone grey
    /// plaid), so this is a plain constant, not an accent-derived name.
    public static let darkResourceName = "wool-dark"

    /// Every weave the build-time tool bakes, as (file base name, palette).
    ///
    /// The list lives HERE and not in the tool so that adding a look is one
    /// entry beside the palette it names — and so the tool stays UIKit-free and
    /// knows nothing about which of these the app then chooses (that is
    /// `darkAccent` above and `FTextures.Variant`, both of which need SwiftUI
    /// and so cannot be seen from the macOS generator).
    public static let bakes: [(name: String, palette: Palette)] = [
        (classicResourceName, .classic),
        (darkResourceName, .dark),
    ]

    /// THE magnification, and the only one: how many POINTS one wool texel
    /// occupies, on every surface that shows wool (live board, compact drawer,
    /// message-bubble preview, app screens).
    ///
    /// Round-6 #14: "keep the threads the same size visually no matter the view
    /// - the block sizes of the wool should be that roughly 1cm x 1cm size on
    /// every screen". The weave's plaid block is `blockPx` = 80 texels, so
    ///     80 texels x 0.775 pt/texel = 62pt per block
    /// and a point is 1/163 inch on a typical phone, i.e. 62pt ~= 0.97cm. On the
    /// iPhone 16's actual 153.3 pt/inch it is 1.03cm. Either way: ~1cm.
    ///
    /// It is a CONSTANT, not a screen-derived fit, because a fit is exactly how
    /// the surfaces drifted apart: the board pinned its scale to the screen
    /// (so a 375pt-wide phone got 49pt blocks where a 393pt one got 63pt),
    /// while BubbleSnapshot aspect-filled the whole 1920x1080 canvas into a
    /// 300x195 balloon (0.181 pt/texel - 14.5pt blocks, "too zoomed out").
    /// One constant makes disagreement impossible: a smaller surface simply
    /// shows LESS weave, never smaller weave.
    public static let pointsPerTexel: CGFloat = 0.775

    /// The plaid block size in texels — the web's literal `/80`, in pixels.
    ///
    /// Three passes got this wrong before it got right, all by treating the
    /// number as a free parameter to taste instead of reading what the web
    /// actually does. `w / 5` (75pt blocks - flat slabs), then `w / 48` (8pt
    /// - hot-pink noise), then `w / 16`. The real answer is that 80 is not a
    /// fraction of anything: it is 80 PIXELS of a 1920-wide render, and the
    /// reason our port drifted is that we were not rendering at that width.
    /// Matching the web's canvas SHAPE is what makes every constant in this
    /// file line up, because the whole generator is written in its pixels.
    public static let blockPx = 80.0

    // MARK: - The generator

    /// Render the weave at `w x h` px. Deterministic (fixed offsets). Pure
    /// CoreGraphics so the build-time tool can call it on macOS.
    ///
    /// BUILD-TIME ONLY. Nothing in the shipping app may call this; the app
    /// loads the baked image through `FTextures`.
    public static func renderCGImage(w: Int, h: Int,
                                     palette: Palette = .classic) -> CGImage? {
        let count = w * h * 4
        var data = [UInt8](repeating: 0, count: count)

        // Brown base, opaque.
        data.withUnsafeMutableBufferPointer { buf in
            let p = buf.baseAddress!
            let br = UInt8(palette.baseR), bg = UInt8(palette.baseG), bb = UInt8(palette.baseB)
            var i = 0
            while i < count { p[i] = br; p[i+1] = bg; p[i+2] = bb; p[i+3] = 255; i += 4 }
        }

        let offX = 0.0, offY = 0.0
        func zValue(_ r: Double) -> Double { let cr = cos(r) * 1000; return cr - floor(cr) }

        let blockPx = Self.blockPx

        // Iteration budget. The web renders at 4K where the area-scaled count
        // already exceeds what the vertical-fibre phase needs to reach the bottom
        // (~h²/1.8); at a smaller size that phase would stop short and leave
        // the base brown showing through as a fringe. Floor the count at the
        // coverage requirement so the weave fills to the bottom edge.
        let areaScaled = Double(w * h) / (1920.0 * 1080.0) * 2_000_000.0
        let coverageFloor = Double(h * h) / 1.8 * 1.06   // +6% margin
        let maxIter = Int(max(areaScaled, coverageFloor))
        let switchPoint = Int(Double(maxIter) * 0.4)

        data.withUnsafeMutableBufferPointer { buf in
            let d = buf.baseAddress!

            // Soft square brush that alpha-blends a colour into the buffer.
            @inline(__always)
            func writePixel(_ x: Double, _ y: Double, _ cr: Double, _ cg: Double, _ cb: Double, _ size: Double) {
                let xi = Int(floor(x)), yi = Int(floor(y))
                let sizeInt = Int(ceil(size))
                let fx = x - Double(xi), fy = y - Double(yi)
                var dy = 0
                while dy < sizeInt {
                    var dx = 0
                    while dx < sizeInt {
                        let px = xi + dx, py = yi + dy
                        if px >= 0, px < w, py >= 0, py < h {
                            let idx = (py * w + px) * 4
                            let distX = abs(Double(dx) - fx), distY = abs(Double(dy) - fy)
                            let edge = max(0.1, 1 - (distX + distY) / 4)
                            let a = edge * 0.99, ia = 1 - a
                            d[idx]   = UInt8(max(0, min(255, Double(d[idx])   * ia + cr * a)))
                            d[idx+1] = UInt8(max(0, min(255, Double(d[idx+1]) * ia + cg * a)))
                            d[idx+2] = UInt8(max(0, min(255, Double(d[idx+2]) * ia + cb * a)))
                        }
                        dx += 1
                    }
                    dy += 1
                }
            }

            var r = 0.0
            var horizontal = true
            var u = 0.0
            var i = 0
            while i < maxIter {
                if i == switchPoint { horizontal = false; r = 0 }

                if horizontal {
                    if i % w == 0 { u = zValue(r) * 500 + 100; r += 5 }
                    let phase = sin(Double(i) / u)
                    let dx = sin(Double(i - 1)) + phase * 6
                    let zVal = zValue(r)
                    let aInt = Int(floor((r + offX) / blockPx + zVal / 4))
                    let bInt = Int(floor((Double(i % h) + offY) / blockPx))
                    // Inside a chequer block the fibre takes the palette's plaid
                    // delta; outside it, nothing. (Was `red`/`-red`/`-red/2`,
                    // i.e. the same three numbers with the hue hard-coded — see
                    // `Palette.plaidR`.)
                    let inBlock = tan(Double(aInt ^ bInt)) > 0.3
                    let pr = inBlock ? palette.plaidR : 0
                    let pg = inBlock ? palette.plaidG : 0
                    let pb = inBlock ? palette.plaidB : 0
                    let cr = palette.weftR + palette.swingR * phase + pr
                    let cg = palette.weftG + palette.swingG * phase + pg
                    let cb = palette.weftB + palette.swingB * phase + pb
                    let x = r + dx, y = Double(i % h)
                    if x >= 0, x < Double(w), y >= 0, y < Double(h) { writePixel(x, y, cr, cg, cb, 2) }
                } else {
                    if i % h == 0 { u = zValue(r) * 500 + 100; r += 3 }
                    let phase = sin(Double(i) / u)
                    let zVal = zValue(r)
                    let aInt = Int(floor((r + offY) / blockPx + zVal / 4))
                    let bInt = Int(floor((Double(i % w) + offX) / blockPx))
                    let inBlock = tan(Double(aInt ^ bInt)) > 0.3
                    let pr = inBlock ? palette.plaidR : 0
                    let pg = inBlock ? palette.plaidG : 0
                    let pb = inBlock ? palette.plaidB : 0
                    let dx = 4 * sin(Double(i - 1)) + sin(Double(i) / u) * 4
                    let cr = palette.warpR + palette.swingR * phase + pr
                    let cg = palette.warpG + palette.swingG * phase + pg
                    let cb = palette.warpB + palette.swingB * phase + pb
                    let x = Double(i % w), y = r + dx
                    let pw = 1.4 * (phase + 1.7)
                    if x >= 0, x < Double(w), y >= 0, y < Double(h) { writePixel(x, y, cr, cg, cb, pw) }
                }
                i += 1
            }
        }

        return cgImageFromRGBA(&data, w: w, h: h)
    }
}

// MARK: - shared buffer → CGImage

/// Wrap a straight RGBA8 buffer in a CGImage. Shared by both generators, and
/// UIKit-free so the build-time macOS tool compiles the same code.
func cgImageFromRGBA(_ data: inout [UInt8], w: Int, h: Int) -> CGImage? {
    let cs = CGColorSpaceCreateDeviceRGB()
    let info = CGImageAlphaInfo.premultipliedLast.rawValue
    return data.withUnsafeMutableBytes { raw -> CGImage? in
        let ctx = CGContext(data: raw.baseAddress, width: w, height: h,
                            bitsPerComponent: 8, bytesPerRow: w * 4,
                            space: cs, bitmapInfo: info)
        return ctx?.makeImage()
    }
}
