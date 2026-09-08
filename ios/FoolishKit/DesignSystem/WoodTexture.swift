// WoodTexture.swift — the wood-grain button/chrome material (§IOS_PHONE_LAYOUT
// §4). A faithful CPU port of the web's WoodTexture generator (CPU-fallback
// path): a dark red-brown base with 576 overlaid grain streaks from a chaotic
// cosine map, edge-softened per streak. The math is ported, not the WebGL.
//
// THIS FILE NO LONGER RUNS IN THE SHIPPING APP. Like WoolTexture it is the
// SOURCE OF TRUTH for the look, executed at BUILD time by
// ios/Tools/GenerateTextures.swift into FoolishKit/Resources/ — one image per
// entry in `bakes` (wood-classic.jpg and wood-dark.jpg).
//
// Why, in one paragraph, because this is the exact code that took the extension
// down on a real phone: `renderCGImage` is 576 grain columns x every row x 25
// k-iterations x a 40px blend span. At the old 300x120 swatch that is ~69M
// blends; at the 384x288 one round-5 needed to cover the tall game-over plank
// it is ~166M, and the iMessage extension came up as a dark, empty panel on
// device while the simulator (no memory cap, no watchdog) looked perfect. The
// interim fix was to TILE a small swatch, which the owner rejected in round-6
// (#16 "tiling isn't an option"). Baking the big swatch at build time is the
// version with no downside: no tiling seams, no repetition, constant grain
// size, and zero procedural pixels at launch.
//
// TO CHANGE THE LOOK: edit `render` / `Palette` here, then run
//   ios/Tools/regenerate_textures.sh
// and commit the regenerated images.
//
// Deliberately UIKit-free (CoreGraphics only) so the macOS build-time tool can
// compile this exact file — one generator, no port to drift.

import CoreGraphics
import Foundation

public enum WoodTexture {

    // MARK: - Palette (the ONE place wood colour lives)

    /// Every colour the grain uses. A dark-mode wood is a second `Palette` plus
    /// a second output file (see `resourceName` and FTextures.Variant), not a
    /// draw-time tint and not a second generator.
    public struct Palette {
        /// The unlit board the streaks are painted onto.
        public let baseR, baseG, baseB: Double
        /// Streak colour, as multipliers on the chaotic map's `b`:
        /// red = b * redGain, green = b² * greenGain, blue = blueFlat.
        public let redGain, greenGain, blueFlat: Double
        /// How opaque one streak pass is at the centre of its 40px span.
        public let streakAlpha: Double
        /// The flat colour every wood surface sits on before the grain is drawn
        /// (and all it shows if the resource is ever missing). Close to the
        /// texture's own base so a failure is dull, not wrong.
        public let fallbackHex: UInt32

        public init(baseR: Double, baseG: Double, baseB: Double,
                    redGain: Double, greenGain: Double, blueFlat: Double,
                    streakAlpha: Double, fallbackHex: UInt32) {
            self.baseR = baseR; self.baseG = baseG; self.baseB = baseB
            self.redGain = redGain; self.greenGain = greenGain; self.blueFlat = blueFlat
            self.streakAlpha = streakAlpha
            self.fallbackHex = fallbackHex
        }

        /// The shipped light wood — the web's numbers, unchanged.
        public static let classic = Palette(
            baseR: 70, baseG: 14, baseB: 9,
            redGain: 120, greenGain: 14, blueFlat: 9,
            streakAlpha: 0.1,
            fallbackHex: 0x5A2412)

        /// The dark-mode wood ("darker wood texture", round-7).
        ///
        /// Every LIGHT-EMITTING number halved and nothing else touched: the base
        /// board, the streak gains, and the fallback. `streakAlpha` deliberately
        /// stays at 0.1 — it controls how much of each of the 576 passes lands,
        /// i.e. the CONTRAST of the grain, not its brightness. Dimming a texture
        /// by flattening its contrast is how wood turns into cardboard; halving
        /// the colours the grain is painted IN keeps every streak exactly where
        /// it was, just in walnut instead of bright orange. Same grain, same
        /// scale (`pointsPerTexel` is untouched), one stop down.
        public static let dark = Palette(
            baseR: 34, baseG: 9, baseB: 6,
            redGain: 62, greenGain: 7, blueFlat: 6,
            streakAlpha: 0.1,
            fallbackHex: 0x2C1209)
    }

    // MARK: - The shipped swatch

    /// The baked swatch size, in texels, and (see `pointsPerTexel`) in points.
    ///
    /// Big enough that the LARGEST wood surface in the app is a crop of it, so
    /// nothing ever tiles: the widest is a full-width control on a 440pt iPhone
    /// inside the board's padding (~408pt), the tallest is the game-over plank
    /// at 8 rows x 34pt = 272pt. 448x288 covers both with margin. Every smaller
    /// surface — a 96x40 action pill — shows a smaller PIECE of exactly this
    /// grain, which is round-5 B2's rule ("the wood grains should be the same
    /// size everywhere, just maybe smaller or larger wood chunks").
    public static let renderCanvas = (w: 448, h: 288)

    /// Base name of the baked LIGHT image in FoolishKit's bundle.
    public static let classicResourceName = "wood-classic"
    /// Base name of the baked DARK image. Only one dark wood exists — the
    /// green/navy choice is the WOOL's (`WoolTexture.darkAccent`); the wood is
    /// the same walnut under either.
    public static let darkResourceName = "wood-dark"

    /// Every grain the build-time tool bakes, as (file base name, palette) —
    /// the twin of `WoolTexture.bakes`, and for the same reason: the list sits
    /// beside the palettes, and the UIKit-free tool just walks it.
    public static let bakes: [(name: String, palette: Palette)] = [
        (classicResourceName, .classic),
        (darkResourceName, .dark),
    ]

    /// One wood texel is one POINT, everywhere. This is round-5 B2 expressed as
    /// a number: no aspect-fill, no stretch-to-fit, no per-surface scale. The
    /// bug it forbids is a taller plank getting proportionally giant grain.
    public static let pointsPerTexel: CGFloat = 1.0

#if FOOLISH_TEXTURE_BAKE
// BUILD-TIME ONLY, and now enforced rather than only asked for.
//
// The generator below has no caller in any shipping target - ios/Tools/
// GenerateTextures.swift and FeltVariations.swift are the only ones - but
// `public` in a DYNAMIC framework is a dead-strip root, so it was linked into
// FoolishKit.framework anyway, and FoolishKit.framework ships inside the
// iMessage bundle.  A procedural render on launch is what took the extension
// down on a real phone (see this file's header); carrying the code that does it
// is the same mistake one step removed.  The two tools pass
// `-D FOOLISH_TEXTURE_BAKE`; no shipping target defines it.
//
// Worth ~1KB of binary, measured (2.639MB -> 2.638MB), so this is a RULE and
// not a diet: the reason to keep it is that a shipping build cannot render a
// texture procedurally even by accident, not the bytes.
    // MARK: - The generator

    /// Render the grain at `w x h` px. Deterministic. Pure CoreGraphics so the
    /// build-time tool can call it on macOS.
    ///
    /// BUILD-TIME ONLY. Nothing in the shipping app may call this; the app
    /// loads the baked image through `FTextures`.
    public static func renderCGImage(w: Int, h: Int,
                                     palette: Palette = .classic) -> CGImage? {
        let count = w * h * 4
        var data = [UInt8](repeating: 0, count: count)
        data.withUnsafeMutableBufferPointer { buf in
            let p = buf.baseAddress!
            let br = UInt8(palette.baseR), bg = UInt8(palette.baseG), bb = UInt8(palette.baseB)
            var i = 0
            while i < count { p[i] = br; p[i+1] = bg; p[i+2] = bb; p[i+3] = 255; i += 4 }
        }

        let offX = 0.0, offY = 0.0
        var iFactors = [Double](repeating: 0, count: h)
        for I in 0..<h { iFactors[I] = (Double(I) + offY) * 0.001 }

        /// A streak's width in px, and so the span of the edge-softening table.
        let rectW = 40

        // THE STREAK MARCH, and why it no longer wraps.
        //
        // Every streak is a 40px-wide band whose left edge advances by a fixed
        // step. The web's generator placed it at `(T * 200) mod w` and clipped
        // the overhang at the right edge (`min(xPos + rectW, w)`). Both halves
        // of that are visible defects in a baked swatch, and they were measured
        // rather than guessed - count, for each column, how many of the 40px
        // spans overlap it:
        //
        //   x = 0        2 passes   (a starved left edge: nothing wrapped into
        //                            it, so it is nearly bare base colour)
        //   x = 0..32    2 -> 48    (the ramp out of that hole)
        //   x = 48..140  60 passes  (a 25% OVER-painted band)
        //   x = 140..447 48 passes  (flat, the only honest part of the swatch)
        //
        // The band is arithmetic, not chaos: 576 streaks at 200/60 px each
        // travel 1920px across a 448px canvas, which is 4.29 wraps. The
        // leftover 0.29 of a wrap lays a FIFTH pass over x 0..124 while the
        // rest of the swatch gets four. A 95x40 button pill is cut from the
        // middle and never shows it; a 408pt full-width plank spans x 20..428
        // and shows the starved edge, the ramp and the step all at once, which
        // is how the owner spotted it.
        //
        // The fix is to stop wrapping. The streaks march LINEARLY off both ends
        // of a longer run and the swatch is cut out of the middle, so every
        // column inside the crop is covered by exactly the same number of
        // spans. Wrapping the overhang around to x=0 instead was tried and is
        // strictly worse: it repairs the starved edge but leaves the fifth-pass
        // band (spread 14 vs 58), because the band is caused by the fractional
        // wrap, not by the clipping.
        //
        // THE STREAK COUNT AND STEP ARE UNCHANGED, and that is a measured
        // decision rather than a lazy one. Marching off the ends drops every
        // column from 48 overlapping spans to 12, and the obvious worry is that
        // a quarter of the paint makes a paler wood. It does not:
        //
        //   shipped        mean RGB (233, 97, 11)   R>=250 23.3%   lum 127.9
        //   run off ends   mean RGB (232, 95, 11)   R>=250 22.5%   lum 126.0
        //
        // The reason is that this palette rails. `red = b * redGain` clears 255
        // in 57% of the passes that paint at all (median 283, max 1782), so at
        // streakAlpha 0.1 the red channel is already saturated after about ten
        // passes - the 13th through 48th were never adding colour, only hiding
        // the fact that the first twelve had run out of headroom. Twelve passes
        // clip as hard as forty-eight and land within 2/255 of the same wood.
        //
        // (A 4x finer T step, 2304 streaks, restores 48 uniform passes and was
        // built and filmed. It is indistinguishable in colour and reads as a
        // finer, busier grain; the coarse march keeps the long diagonal
        // features and was the one chosen. It is also 4x the build-time work.)
        //
        // What this does NOT fix is the clipping itself - that is redGain's
        // doing and belongs to a palette change, not to this one.
        let streaks = 576
        let tDiv = 60.0
        let stepPx = 200.0 / tDiv
        let travel = Double(streaks) * stepPx
        // Where the swatch is cut from the march. Anywhere with a full 40px of
        // run-up behind it and 40px of run-out ahead has identical coverage;
        // the middle is the one choice that needs no justification.
        let cropX = max(rectW, Int((travel - Double(w)) / 2))

        // The per-x softening of a streak depends ONLY on the pixel's distance
        // from the streak centre, so across 576 columns x every row x 25
        // k-iterations it recomputes the same 40 numbers tens of millions of
        // times. Hoisted into a table: same output, one table lookup per pixel
        // instead of a subtract/abs/divide/max/multiply chain. (Kept even
        // though this is build-time code — it is the difference between a
        // regeneration you run and one you wait out.)
        var alphas = [Double](repeating: 0, count: rectW)
        var invAlphas = [Double](repeating: 0, count: rectW)
        for dx in 0..<rectW {
            let dist = abs(Double(dx) - Double(rectW) / 2) / (Double(rectW) / 2)
            let edge = max(0.3, 1 - dist * 0.5)
            alphas[dx] = palette.streakAlpha * edge
            invAlphas[dx] = 1 - alphas[dx]
        }

        data.withUnsafeMutableBufferPointer { buf in
            let d = buf.baseAddress!

            func drawColumn(_ T: Double) {
                // The streak's left edge in MARCH coordinates, then in the
                // swatch's own. No remainder: the march runs off both ends.
                let start = Int((T + offX / 200) * 200) - cropX
                // Most of the march misses the crop entirely. Skipping those
                // streaks outright is what keeps this the same amount of work
                // as the wrapping version despite a 4x longer run.
                if start >= w || start + rectW <= 0 { return }
                let kLo = max(0, -start)
                let kHi = min(rectW, w - start)
                var I = h - 1
                while I >= 0 {
                    let iFactor = iFactors[I]
                    var b = (T + offX * 0.01) / 24
                    var k = 24
                    while k >= 0 {
                        let bHalf = (b * b) * 0.5
                        b = cos(iFactor + cos(bHalf) * b + 4 + offY * 0.001) * b - 2.8
                        if b > 0 {
                            let red = b * palette.redGain
                            let green = b * b * palette.greenGain
                            let blue = palette.blueFlat
                            // `dx` is the offset INSIDE the streak, so it still
                            // indexes the edge-softening table directly even
                            // when the span hangs off the crop.
                            var dx = kLo
                            while dx < kHi {
                                let idx = (I * w + (start + dx)) * 4
                                let a = alphas[dx], ia = invAlphas[dx]
                                d[idx]   = UInt8(max(0, min(255, red   * a + Double(d[idx])   * ia)))
                                d[idx+1] = UInt8(max(0, min(255, green * a + Double(d[idx+1]) * ia)))
                                d[idx+2] = UInt8(max(0, min(255, blue  * a + Double(d[idx+2]) * ia)))
                                dx += 1
                            }
                        }
                        k -= 1
                    }
                    I -= 1
                }
            }

            var i = 0
            while i < streaks { drawColumn(Double(i) / tDiv); i += 1 }
        }

        return cgImageFromRGBA(&data, w: w, h: h)
    }
#endif  // FOOLISH_TEXTURE_BAKE
}
