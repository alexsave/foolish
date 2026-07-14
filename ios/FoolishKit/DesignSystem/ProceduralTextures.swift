// ProceduralTextures.swift — the wool / wood / fern materials that give the app
// its identity, ported from the web (src/components/WoolBackground.tsx,
// WoodTexture.tsx, src/utils/fernFractal.tsx). Pure CoreGraphics: each generator
// fills an RGBA buffer and wraps it in a CGImage.
//
// Two things the web taught us and we keep:
//  1. Each PLAYER gets a subtly different weave/grain/fern. The web reseeds from
//     Math.random() once per page load; we persist one random `installSeed` so a
//     given device is stable across launches but unique from the next player.
//  2. Generation is expensive (millions of iterations) — so we generate once,
//     off the main thread, and cache the PNGs to disk (the web caches to
//     IndexedDB). Subsequent launches load the cached image instantly.
//
// The heather flecks are toned to a warm russet rather than the web's hot-pink
// (the "smoother/cleaner" brief); everything else matches the source math.

import SwiftUI
import CoreGraphics
import ImageIO
import UniformTypeIdentifiers

// MARK: - per-install seed

public enum TextureSeed {
    private static let key = "foolish.textureSeed.v1"
    /// A random-but-persistent 64-bit seed. Stable for this install, unique per
    /// player — drives the wool/wood offsets and the fern RNG.
    public static var value: UInt64 = {
        let d = UserDefaults.standard
        let existing = d.object(forKey: key) as? NSNumber
        if let n = existing { return n.uint64Value }
        var s = UInt64.random(in: 1...UInt64.max)
        if s == 0 { s = 0x9E3779B97F4A7C15 }
        d.set(NSNumber(value: s), forKey: key)
        return s
    }()
}

/// SplitMix64 — reproducible per-seed randomness without a dependency.
struct SplitMix64 {
    var s: UInt64
    init(_ seed: UInt64) { s = seed }
    mutating func next() -> UInt64 {
        s &+= 0x9E3779B97F4A7C15
        var z = s
        z = (z ^ (z >> 30)) &* 0xBF58476D1CE4E5B9
        z = (z ^ (z >> 27)) &* 0x94D049BB133111EB
        return z ^ (z >> 31)
    }
    mutating func unit() -> Double { Double(next() >> 11) * (1.0 / 9_007_199_254_740_992.0) }
}

// MARK: - buffer → CGImage

private func imageFromRGBA(_ buf: [UInt8], _ w: Int, _ h: Int) -> CGImage? {
    var data = buf
    let cs = CGColorSpaceCreateDeviceRGB()
    let info = CGImageAlphaInfo.premultipliedLast.rawValue
    guard let ctx = data.withUnsafeMutableBytes({ ptr in
        CGContext(data: ptr.baseAddress, width: w, height: h, bitsPerComponent: 8,
                  bytesPerRow: w * 4, space: cs, bitmapInfo: info)
    }) else { return nil }
    return ctx.makeImage()
}

// MARK: - generators (validated against the web source)

public enum ProceduralTextures {

    /// Woven wool — brown base with cream horizontal + vertical fibers and warm
    /// russet heather. Port of generateWoolTextureFallback.
    public static func wool(_ w: Int, _ h: Int, seed: UInt64) -> CGImage? {
        var rng = SplitMix64(seed ^ 0x1111)
        let offsetX = rng.unit() * 1000 - 500
        let offsetY = rng.unit() * 1000 - 500
        var px = [Double](repeating: 0, count: w * h * 3)
        for i in 0..<(w * h) { px[i*3] = 113; px[i*3+1] = 65; px[i*3+2] = 27 }

        func writePixel(_ x: Double, _ y: Double, _ r: Double, _ g: Double, _ b: Double, _ size: Double) {
            let xi = Int(x.rounded(.down)), yi = Int(y.rounded(.down))
            let sizeInt = Int(size.rounded(.up))
            let fx = x - Double(xi), fy = y - Double(yi)
            for dy in 0..<sizeInt { for dx in 0..<sizeInt {
                let pxX = xi + dx, pyY = yi + dy
                if pxX >= 0 && pxX < w && pyY >= 0 && pyY < h {
                    let idx = (pyY * w + pxX) * 3
                    let edge = max(0.1, 1 - (abs(Double(dx) - fx) + abs(Double(dy) - fy)) / 4)
                    let a = edge * 0.99, ia = 1 - a
                    px[idx]   = px[idx]   * ia + r * a
                    px[idx+1] = px[idx+1] * ia + g * a
                    px[idx+2] = px[idx+2] * ia + b * a
                }
            }}
        }

        var r = 0.0
        func zValue() -> Double { let cr = cos(r) * 1000; return cr - cr.rounded(.down) }
        var hPhase = true
        var u = 0.0
        let maxIter = Int((Double(w * h) / (1920.0 * 1080.0)) * 2_000_000)
        let switchPoint = Int(Double(maxIter) * 0.4)
        for i in 0..<maxIter {
            if i == switchPoint { hPhase = false; r = 0 }
            if hPhase {
                if i % w == 0 { u = zValue() * 500 + 100; r += 5 }
                let phase = sin(Double(i) / u)
                let dx = sin(Double(i - 1)) + phase * 6
                let zv = zValue()
                let a = Int((((r + offsetX) / 80 + zv / 4)).rounded(.down))
                let b = Int(((Double(i % h) + offsetY) / 80).rounded(.down))
                let heather = tan(Double(a ^ b)) > 0.55 ? 1.0 : 0.0
                let cR = 209 + 46 * phase - heather * 26
                let cG = 208 + 45 * phase - heather * 74
                let cB = 183 + 53 * phase - heather * 86
                let x = r + dx, y = Double(i % h)
                if x >= 0 && x < Double(w) && y >= 0 && y < Double(h) { writePixel(x, y, cR, cG, cB, 2) }
            } else {
                if i % h == 0 { u = zValue() * 500 + 100; r += 3 }
                let phase = sin(Double(i) / u)
                let zv = zValue()
                let a = Int((((r + offsetY) / 80 + zv / 4)).rounded(.down))
                let b = Int(((Double(i % w) + offsetX) / 80).rounded(.down))
                let heather = tan(Double(a ^ b)) > 0.55 ? 1.0 : 0.0
                let dx = 4 * sin(Double(i - 1)) + sin(Double(i) / u) * 4
                let cR = 189 + 46 * phase - heather * 26
                let cG = 188 + 45 * phase - heather * 74
                let cB = 163 + 53 * phase - heather * 86
                let x = Double(i % w), y = r + dx
                let pw = 1.4 * (phase + 1.7)
                if x >= 0 && x < Double(w) && y >= 0 && y < Double(h) { writePixel(x, y, cR, cG, cB, pw) }
            }
        }
        var out = [UInt8](repeating: 255, count: w * h * 4)
        for i in 0..<(w * h) {
            out[i*4]   = UInt8(max(0, min(255, px[i*3])))
            out[i*4+1] = UInt8(max(0, min(255, px[i*3+1])))
            out[i*4+2] = UInt8(max(0, min(255, px[i*3+2])))
        }
        return imageFromRGBA(out, w, h)
    }

    /// Dark mahogany plank — base rgb(70,14,9) with warm horizontal grain. Port
    /// of generateWoodTextureFallback. Used for wooden buttons.
    public static func wood(_ w: Int, _ h: Int, seed: UInt64) -> CGImage? {
        var rng = SplitMix64(seed ^ 0x2222)
        let offsetX = rng.unit() * 1000 - 500
        let offsetY = rng.unit() * 1000 - 500
        var px = [Double](repeating: 0, count: w * h * 3)
        for i in 0..<(w * h) { px[i*3] = 70; px[i*3+1] = 14; px[i*3+2] = 9 }
        var iFac = [Double](repeating: 0, count: h)
        for I in 0..<h { iFac[I] = (Double(I) + offsetY) * 0.001 }

        func D(_ T: Double) {
            let xPos = Int(((T + offsetX / 200) * 200).truncatingRemainder(dividingBy: Double(w)))
            let rectW = 40
            let xEnd = min(xPos + rectW, w)
            let xCenter = Double(xPos) + Double(rectW) / 2
            var I = h - 1
            while I >= 0 {
                var b = (T + offsetX * 0.01) / 24
                var k = 24
                while k >= 0 { b = cos(iFac[I] + cos(b * b * 0.5) * b + 4 + offsetY * 0.001) * b - 2.8; k -= 1 }
                if b > 0 {
                    let red = b * 120, green = min(b * b * 14, 40), blue = 9.0
                    let alpha = 0.1
                    var x = xPos
                    while x < xEnd {
                        if x >= 0 {
                            let idx = (I * w + x) * 3
                            let dist = abs(Double(x) - xCenter) / (Double(rectW) / 2)
                            let ea = alpha * max(0.3, 1 - dist * 0.5), iea = 1 - ea
                            px[idx]   = red * ea + px[idx] * iea
                            px[idx+1] = green * ea + px[idx+1] * iea
                            px[idx+2] = blue * ea + px[idx+2] * iea
                        }
                        x += 1
                    }
                }
                I -= 1
            }
        }
        for i in 0..<576 { D(Double(i) / 60) }
        var out = [UInt8](repeating: 255, count: w * h * 4)
        for i in 0..<(w * h) {
            out[i*4]   = UInt8(max(0, min(255, px[i*3])))
            out[i*4+1] = UInt8(max(0, min(255, px[i*3+1])))
            out[i*4+2] = UInt8(max(0, min(255, px[i*3+2])))
        }
        return imageFromRGBA(out, w, h)
    }

    /// Barnsley-fern IFS card back — gold/red/amber fronds on black, rotated
    /// -67° with mirror flips and two decorative berry circles. Port of
    /// fernFractal.tsx (the math, not the WebGL).
    public static func fern(_ w: Int, _ h: Int, seed: UInt64, iterations: Int = 900_000) -> CGImage? {
        var rng = SplitMix64(seed ^ 0x3333)
        var out = [UInt8](repeating: 255, count: w * h * 4)
        for i in 0..<(w * h) { out[i*4] = 0; out[i*4+1] = 0; out[i*4+2] = 0 }

        struct Aff { let a, b, c, d, e, f: Double }
        let f1 = Aff(a: 0.02317, b: -0.0013, c: 0, d: 0.21422, e: 0, f: 0)
        let f2 = Aff(a: 0.789, b: 0.1533, c: -0.1877, d: 0.8734, e: 0.0617, f: 2)
        let f3 = Aff(a: -0.4556, b: -0.2832, c: -0.3847, d: 0.3305, e: 0, f: 1)
        let f4 = Aff(a: 0.3, b: 0.2, c: -0.2, d: 0.2, e: 0, f: 0)
        let circle1 = (cx: 2.803, cy: 0.5296, r: 0.4817)
        let circle2 = (cx: -4.5784, cy: 1.4463, r: 0.2894)
        let probs = [0.01, 0.8, 0.08, 0.08, 0.02, 0.01]
        let sum = probs.reduce(0, +)
        var cum = [Double](); var acc = 0.0
        for p in probs { acc += p / sum; cum.append(acc) }
        let gold = (255.0, 215.0, 0.0), red = (255.0, 0.0, 0.0), amber = (189.0, 120.0, 0.0)

        let scale = Double(min(w, h)) / 70.0
        let scaleY = 14.0 * scale
        let rot = -67.0 * Double.pi / 180
        let cosR = cos(rot), sinR = sin(rot)
        let cx = Double(w) / 2, cy = Double(h) / 2

        func aff(_ x: Double, _ y: Double, _ t: Aff) -> (Double, Double) {
            (t.a * x + t.b * y + t.e, t.c * x + t.d * y + t.f)
        }
        func inCircle(_ ccx: Double, _ ccy: Double, _ rr: Double) -> (Double, Double) {
            let t = rng.unit(); let ang = rng.unit() * .pi * 2; let rad = t.squareRoot() * rr
            return (ccx + rad * cos(ang), ccy + rad * sin(ang))
        }
        func plot(_ px: Double, _ py: Double, _ col: (Double, Double, Double)) {
            let xi = Int(px), yi = Int(py)
            if xi >= 0 && xi < w && yi >= 0 && yi < h {
                let idx = (yi * w + xi) * 4
                out[idx] = UInt8(col.0); out[idx+1] = UInt8(col.1); out[idx+2] = UInt8(col.2)
            }
        }
        func pick() -> Int { let r = rng.unit(); for i in 0..<cum.count { if r < cum[i] { return i } }; return cum.count - 1 }
        func stepFn(_ idx: Int, _ x: Double, _ y: Double) -> (Double, Double) {
            switch idx {
            case 0: return aff(x, y, f1); case 1: return aff(x, y, f2)
            case 2: return aff(x, y, f3); case 3: return aff(x, y, f4)
            case 4: return inCircle(circle1.cx, circle1.cy, circle1.r)
            default: return inCircle(circle2.cx, circle2.cy, circle2.r)
            }
        }

        var x = 0.0, y = 0.0
        for _ in 0..<20 { let idx = pick(); (x, y) = stepFn(idx, x, y) }
        var propagate = 0
        for _ in 0..<iterations {
            let idx = pick()
            (x, y) = stepFn(idx, x, y)
            if idx == 4 {
                var (dx, dy) = inCircle(circle1.cx, circle1.cy, circle1.r)
                if rng.unit() < 0.5 { dx = -dx; dy = -dy }
                plot(cx + (dx * cosR - dy * sinR) * scaleY, cy - (dx * sinR + dy * cosR) * scaleY, red); propagate = 1; continue
            }
            if idx == 5 {
                var (dx, dy) = inCircle(circle2.cx, circle2.cy, circle2.r)
                if rng.unit() < 0.5 { dx = -dx; dy = -dy }
                plot(cx + (dx * cosR - dy * sinR) * scaleY, cy - (dx * sinR + dy * cosR) * scaleY, amber); propagate = 2; continue
            }
            if idx == 0 { propagate = 0 }
            var drawX = x, drawY = y
            if rng.unit() < 0.5 { drawX = -x; drawY = -y }
            let col = propagate == 2 ? amber : (propagate == 1 ? red : gold)
            plot(cx + (drawX * cosR - drawY * sinR) * scaleY, cy - (drawX * sinR + drawY * cosR) * scaleY, col)
        }
        return imageFromRGBA(out, w, h)
    }
}
