// FernCardBack.swift — the ONE procedural material carried over from the web
// (§5.3): the fern-IFS card back, drawn on the CPU into a CGImage once per seed
// and cached. Ports the affine iterated-function-system from the web's
// src/utils/fernFractal.tsx (the f1–f4 transforms and their weights) — the math,
// not the WebGL. Bone-white points on the ink card; identical in spirit to the
// web backs, which keeps cross-platform brand identity and later enables the
// seed-cosmetic ideas.
//
// Determinism here is per-seed reproducibility (a xorshift keyed by the seed),
// NOT byte-parity with the web's Math.random() stream — the back is cosmetic.

import CoreGraphics
import UIKit

public enum FernCardBack {

    // The web's fern affine transforms (fernFractal.tsx:59-62) and cumulative
    // weights (f1_stem .01, f2_main .8, f3_left .08, f4_right .08 → the two
    // decorative circles are dropped for v1's single-material port).
    private struct Affine { let a, b, c, d, e, f: Double }
    private static let transforms: [Affine] = [
        Affine(a: 0.02317, b: -0.0013, c: 0,       d: 0.21422, e: 0,      f: 0),  // f1 stem
        Affine(a: 0.789,   b: 0.1533,  c: -0.1877, d: 0.8734,  e: 0.0617, f: 2),  // f2 main
        Affine(a: -0.4556, b: -0.2832, c: -0.3847, d: 0.3305,  e: 0,      f: 1),  // f3 left
        Affine(a: 0.3,     b: 0.2,     c: -0.2,    d: 0.2,     e: 0,      f: 0),  // f4 right
    ]
    private static let weights: [Double] = [0.01, 0.80, 0.08, 0.08] // renormalized to sum 0.97+circles

    private static var cache: [String: UIImage] = [:]
    private static let lock = NSLock()

    /// A card back for `seed` at `size` points. Rendered at a fixed 3x (Retina
    /// enough for a small card; avoids UIScreen.main, which is extension-unsafe
    /// and deprecated). Cached by seed+size+scale.
    public static func image(seed: UInt64, size: CGSize, scale: CGFloat = 3) -> UIImage {
        let key = "\(seed)-\(Int(size.width))x\(Int(size.height))@\(Int(scale))"
        lock.lock(); if let img = cache[key] { lock.unlock(); return img }; lock.unlock()

        let img = render(seed: seed, size: size, scale: scale)
        lock.lock(); cache[key] = img; lock.unlock()
        return img
    }

    private static func render(seed: UInt64, size: CGSize, scale: CGFloat) -> UIImage {
        let format = UIGraphicsImageRendererFormat()
        format.scale = scale
        format.opaque = true
        let renderer = UIGraphicsImageRenderer(size: size, format: format)
        return renderer.image { ctx in
            let cg = ctx.cgContext
            cg.setFillColor(FColor.ink.cgColor)
            cg.fill(CGRect(origin: .zero, size: size))

            var rng = XorShift(seed: seed == 0 ? 0x9E3779B97F4A7C15 : seed)
            let cum = cumulative(weights)

            // Fit the fern's roughly [-3,3]x[0,11] extent into the card, inset.
            let inset: CGFloat = 6
            let w = size.width - inset * 2, h = size.height - inset * 2
            let scaleX = w / 6.5, scaleY = h / 11.5
            let originX = size.width / 2
            let originY = size.height - inset

            let bone = FColor.card.withAlphaComponent(0.72).cgColor
            cg.setFillColor(bone)

            var x = 0.0, y = 0.0
            let iterations = 42_000
            for i in 0..<iterations {
                let r = rng.nextUnit()
                var idx = 0
                while idx < cum.count - 1 && r > cum[idx] { idx += 1 }
                let t = transforms[idx]
                let nx = t.a * x + t.b * y + t.e
                let ny = t.c * x + t.d * y + t.f
                x = nx; y = ny
                if i < 20 { continue } // let the attractor settle
                let px = originX + CGFloat(x) * scaleX
                let py = originY - CGFloat(y) * scaleY
                cg.fill(CGRect(x: px, y: py, width: 1.0, height: 1.0))
            }
        }
    }

    private static func cumulative(_ w: [Double]) -> [Double] {
        let total = w.reduce(0, +)
        var acc = 0.0
        return w.map { acc += $0 / total; return acc }
    }

    /// Tiny deterministic PRNG — reproducible card backs without pulling in a dep.
    private struct XorShift {
        var state: UInt64
        init(seed: UInt64) { state = seed }
        mutating func next() -> UInt64 {
            state ^= state << 13; state ^= state >> 7; state ^= state << 17; return state
        }
        mutating func nextUnit() -> Double { Double(next() >> 11) * (1.0 / 9_007_199_254_740_992.0) }
    }
}
