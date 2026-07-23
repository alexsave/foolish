// WoodTexture.swift — the wood-grain button/chrome material (§IOS_PHONE_LAYOUT
// §4). A faithful CPU port of the web's WoodTexture generator (CPU-fallback
// path): a dark red-brown base with 576 overlaid grain streaks from a chaotic
// cosine map, edge-softened per streak. The math is ported, not the WebGL.
//
// THIS FILE NO LONGER RUNS IN THE SHIPPING APP. Like WoolTexture it is the
// SOURCE OF TRUTH for the look, executed at BUILD time by
// ios/Tools/GenerateTextures.swift into FoolishKit/Resources/wood-classic.jpg.
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

    /// Base name of the baked image in FoolishKit's bundle. A dark variant
    /// would be `wood-dark` beside it.
    public static let resourceName = "wood-classic"

    /// One wood texel is one POINT, everywhere. This is round-5 B2 expressed as
    /// a number: no aspect-fill, no stretch-to-fit, no per-surface scale. The
    /// bug it forbids is a taller plank getting proportionally giant grain.
    public static let pointsPerTexel: CGFloat = 1.0

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

        // The per-x softening of a streak depends ONLY on the pixel's distance
        // from the streak centre, so across 576 columns x every row x 25
        // k-iterations it recomputes the same 40 numbers tens of millions of
        // times. Hoisted into a table: same output, one table lookup per pixel
        // instead of a subtract/abs/divide/max/multiply chain. (Kept even
        // though this is build-time code — it is the difference between a
        // regeneration you run and one you wait out.)
        let rectW = 40
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
                let xPosFloat = ((T + offX / 200) * 200).truncatingRemainder(dividingBy: Double(w))
                let xPos = Int(xPosFloat)
                let xEnd = min(xPos + rectW, w)
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
                            var x = xPos
                            while x < xEnd {
                                let idx = (I * w + x) * 4
                                let a = alphas[x - xPos], ia = invAlphas[x - xPos]
                                d[idx]   = UInt8(max(0, min(255, red   * a + Double(d[idx])   * ia)))
                                d[idx+1] = UInt8(max(0, min(255, green * a + Double(d[idx+1]) * ia)))
                                d[idx+2] = UInt8(max(0, min(255, blue  * a + Double(d[idx+2]) * ia)))
                                x += 1
                            }
                        }
                        k -= 1
                    }
                    I -= 1
                }
            }

            var i = 0
            while i < 576 { drawColumn(Double(i) / 60); i += 1 }
        }

        return cgImageFromRGBA(&data, w: w, h: h)
    }
}
