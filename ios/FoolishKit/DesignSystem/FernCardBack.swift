// FernCardBack.swift — the card-back fern, a FAITHFUL port of the web's finely
// tuned generator (src/utils/fernFractal.tsx, generateFernPattern), NOT the crude
// 4-map single-colour slice this file used to be. The real fern is what the owner
// spent a long time tuning, and it has all of this:
//
//   * SIX generators: f1..f4 affine PLUS circle1/circle2 (randomPointInCircle) -
//     the red / bronze BULB-DOTS the old port dropped.
//   * THREE colours with PROPAGATION: a gold spine (#ffd700); a point born from a
//     bulb keeps its colour (#ff0000 / #bd7800) until the STEM (f1) resets it, so
//     whole fronds glow red/bronze.
//   * a -67 degree ROTATION (the diagonal spine) and a random point-mirror (negate
//     x,y with p=0.5) that doubles the fern into a full radiating spray.
//   * a BLACK field (CardBack.tsx composites the fern on #000, under a red frame).
//
// BUILD-TIME ONLY, like WoolTexture/WoodTexture: the shipping app never runs this
// (a heavy procedural render on launch is what took the iMessage extension down on
// a real phone - see those files). `regenerate_textures.sh` bakes `fern-back.jpg`
// into FoolishKit/Resources/; FCard loads it through FTextures and draws zero
// procedural pixels. UIKit-free CoreGraphics so the macOS bake tool can call it.
//
// Determinism here is byte-reproducible builds (a fixed-seed xorshift), NOT parity
// with the web's Math.random() stream - the back is cosmetic. The tuning lab this
// mirrors 1:1 is ios/Tools/fern_ifs.html (its "card-back" preset == these numbers).

import Foundation
import CoreGraphics

public enum FernCardBack {

    /// Base name of the baked image in FoolishKit's bundle.
    public static let resourceName = "fern-back"

    /// The bake list (one image), shaped like WoodTexture.bakes so GenerateTextures
    /// treats all three materials the same way.
    public static let bakes: [(name: String, w: Int, h: Int)] = [
        // 5:7 card aspect. Big enough that the fine gold spine and the frond
        // filigree survive being shown on the deck / discard stacks; scaled DOWN
        // for the tiny seat-badge fans, never up.
        (resourceName, 480, 672),
    ]

    // ---- fernFractal.tsx DEFAULT_FERN_PARAMS, verbatim ----------------------
    private struct Affine { let a, b, c, d, e, f: Double }
    private static let transforms: [Affine] = [
        Affine(a: 0.02317, b: -0.0013, c: 0,       d: 0.21422, e: 0,      f: 0),  // f1 stem
        Affine(a: 0.789,   b: 0.1533,  c: -0.1877, d: 0.8734,  e: 0.0617, f: 2),  // f2 main
        Affine(a: -0.4556, b: -0.2832, c: -0.3847, d: 0.3305,  e: 0,      f: 1),  // f3 left
        Affine(a: 0.3,     b: 0.2,     c: -0.2,    d: 0.2,     e: 0,      f: 0),  // f4 right
    ]
    private struct Circle { let cx, cy, r: Double }
    private static let circle1 = Circle(cx: 2.803,   cy: 0.5296, r: 0.4817)  // red bulbs
    private static let circle2 = Circle(cx: -4.5784, cy: 1.4463, r: 0.2894)  // bronze bulbs
    // probabilities f1,f2,f3,f4,circle1,circle2 (normalized in `cumulative`)
    private static let weights: [Double] = [0.01, 0.80, 0.08, 0.08, 0.02, 0.01]
    // colours: primary (spine/default), secondary (circle1), tertiary (circle2)
    private static let colPrimary   = (r: 0xFF, g: 0xD7, b: 0x00)
    private static let colSecondary = (r: 0xFF, g: 0x00, b: 0x00)
    private static let colTertiary  = (r: 0xBD, g: 0x78, b: 0x00)
    private static let rotationDeg = -67.0
    // createCardParams: scaleY = 14 * (min(w,h)/70). Written that way so the bake
    // is resolution-independent - the fern fills the card at any canvas size.
    private static let scaleYBase = 14.0

    /// Render one fern card back `w x h` px, on black. BUILD-TIME ONLY.
    ///
    /// `dotSize` px per plotted point, `density` samples per pixel (the dot
    /// COUNT), and `alpha` the per-dot opacity: each dot is BLENDED over what is
    /// there, so overlapping fronds build up toward full colour while sparse
    /// stray dots stay dim - lowering all three is how the fern comes down from
    /// "blown out" to a softer glow. The defaults are the owner's chosen look
    /// (round-7, the d1/den14/a0.20 pick off the density x alpha sweep): 1px
    /// dots, plenty of them, but low per-dot opacity so the dense fronds build
    /// to a deep glow while stray dots stay dim - a soft fern, not a block.
    public static func renderCGImage(w: Int, h: Int,
                                     dotSize: Int = 1, density: Double = 14.0,
                                     alpha: Double = 0.20) -> CGImage? {
        let count = w * h * 4
        var data = [UInt8](repeating: 0, count: count)
        // black opaque field
        data.withUnsafeMutableBufferPointer { buf in
            let p = buf.baseAddress!
            var i = 0
            while i < count { p[i] = 0; p[i+1] = 0; p[i+2] = 0; p[i+3] = 255; i += 4 }
        }

        let cum = cumulative(weights)
        var rng = XorShift(seed: 0x9E3779B97F4A7C15)
        let rot = rotationDeg * Double.pi / 180
        let cosR = cos(rot), sinR = sin(rot)
        let s = Double(min(w, h)) / 70.0
        let sc = scaleYBase * s
        let cx = Double(w) / 2.0, cy = Double(h) / 2.0
        // Sample count = density per pixel (resolution-independent). Capped only
        // as a runaway backstop - high and generous, because this is a build-time
        // bake (a few seconds, no runtime cost) and the cap must not clamp a
        // high-density look at the ship resolution while leaving small preview
        // thumbnails un-clamped (which would make the two disagree).
        let iterations = min(12_000_000, Int(Double(w * h) * density))

        func circlePoint(_ c: Circle) -> (Double, Double) {
            let t = rng.nextUnit(), ang = rng.nextUnit() * 2 * Double.pi
            let rad = (t.squareRoot()) * c.r
            return (c.cx + rad * cos(ang), c.cy + rad * sin(ang))
        }
        func pick() -> Int {
            let r = rng.nextUnit()
            var i = 0
            while i < cum.count - 1 && r >= cum[i] { i += 1 }
            return i
        }

        data.withUnsafeMutableBufferPointer { buf in
            let d = buf.baseAddress!
            let a = alpha, ia = 1 - alpha
            func plot(_ px: Double, _ py: Double, _ col: (r: Int, g: Int, b: Int)) {
                let X = Int(px), Y = Int(py)
                let cr = Double(col.r) * a, cg = Double(col.g) * a, cb = Double(col.b) * a
                var yy = Y
                while yy < Y + dotSize {
                    if yy >= 0 && yy < h {
                        var xx = X
                        while xx < X + dotSize {
                            if xx >= 0 && xx < w {
                                let o = (yy * w + xx) * 4
                                // blend col over the existing pixel; overlapping
                                // dots accumulate toward full colour.
                                d[o]   = UInt8(min(255, cr + Double(d[o])   * ia))
                                d[o+1] = UInt8(min(255, cg + Double(d[o+1]) * ia))
                                d[o+2] = UInt8(min(255, cb + Double(d[o+2]) * ia))
                            }
                            xx += 1
                        }
                    }
                    yy += 1
                }
            }
            // mirror -> rotate -> scale -> translate (translate is 0 here)
            func place(_ dx0: Double, _ dy0: Double) -> (Double, Double) {
                var dx = dx0, dy = dy0
                if rng.nextUnit() < 0.5 { dx = -dx; dy = -dy }
                let rx = dx * cosR - dy * sinR, ry = dx * sinR + dy * cosR
                return (cx + rx * sc, cy - (ry * sc))
            }

            var x = 0.0, y = 0.0, prop = 0
            for _ in 0..<20 {                      // warmup (let the attractor settle)
                let idx = pick()
                if idx == 4 { (x, y) = circlePoint(circle1) }
                else if idx == 5 { (x, y) = circlePoint(circle2) }
                else { let t = transforms[idx]; let nx = t.a*x+t.b*y+t.e, ny = t.c*x+t.d*y+t.f; x = nx; y = ny }
            }
            for _ in 0..<iterations {
                let idx = pick()
                if idx == 4 {
                    (x, y) = circlePoint(circle1)
                    let (dx, dy) = circlePoint(circle1)
                    let (px, py) = place(dx, dy); plot(px, py, colSecondary); prop = 1; continue
                }
                if idx == 5 {
                    (x, y) = circlePoint(circle2)
                    let (dx, dy) = circlePoint(circle2)
                    let (px, py) = place(dx, dy); plot(px, py, colTertiary); prop = 2; continue
                }
                let t = transforms[idx]
                let nx = t.a*x + t.b*y + t.e, ny = t.c*x + t.d*y + t.f
                x = nx; y = ny
                if idx == 0 { prop = 0 }            // stem resets colour to the gold spine
                let (px, py) = place(x, y)
                plot(px, py, prop == 2 ? colTertiary : (prop == 1 ? colSecondary : colPrimary))
            }
        }

        return imageFromRGBA(&data, w: w, h: h)
    }

    private static func cumulative(_ w: [Double]) -> [Double] {
        let total = w.reduce(0, +)
        var acc = 0.0
        return w.map { acc += $0 / total; return acc }
    }

    private static func imageFromRGBA(_ data: inout [UInt8], w: Int, h: Int) -> CGImage? {
        let cs = CGColorSpaceCreateDeviceRGB()
        let info = CGImageAlphaInfo.premultipliedLast.rawValue
        return data.withUnsafeMutableBytes { raw -> CGImage? in
            CGContext(data: raw.baseAddress, width: w, height: h, bitsPerComponent: 8,
                      bytesPerRow: w * 4, space: cs, bitmapInfo: info)?.makeImage()
        }
    }

    /// Tiny deterministic PRNG - byte-reproducible bakes without a dependency.
    private struct XorShift {
        var state: UInt64
        init(seed: UInt64) { state = seed == 0 ? 0x9E3779B97F4A7C15 : seed }
        mutating func next() -> UInt64 { state ^= state << 13; state ^= state >> 7; state ^= state << 17; return state }
        mutating func nextUnit() -> Double { Double(next() >> 11) * (1.0 / 9_007_199_254_740_992.0) }
    }
}
