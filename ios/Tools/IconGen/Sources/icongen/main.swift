// icongen — render the 1024px app icon: the fern IFS (ported from the web's
// fernFractal, same as FoolishKit/DesignSystem/FernCardBack) drawn bone-on-felt.
// One blessed seed so the icon is stable. macOS command-line tool.

import Foundation
import CoreGraphics
import ImageIO
import UniformTypeIdentifiers

// One blessed seed (ICON_SEED) so the icon never changes between builds.
let seed: UInt64 = 0x00F0_015E_ED17_2B24

let size = 1024
let outPath = CommandLine.arguments.count > 1
    ? CommandLine.arguments[1]
    : "AppIcon.png"

struct Affine { let a, b, c, d, e, f: Double }
let transforms: [Affine] = [
    Affine(a: 0.02317, b: -0.0013, c: 0,       d: 0.21422, e: 0,      f: 0),
    Affine(a: 0.789,   b: 0.1533,  c: -0.1877, d: 0.8734,  e: 0.0617, f: 2),
    Affine(a: -0.4556, b: -0.2832, c: -0.3847, d: 0.3305,  e: 0,      f: 1),
    Affine(a: 0.3,     b: 0.2,     c: -0.2,    d: 0.2,     e: 0,      f: 0),
]
let weights: [Double] = [0.01, 0.80, 0.08, 0.08]

struct XorShift { var s: UInt64
    mutating func next() -> UInt64 { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s }
    mutating func unit() -> Double { Double(next() >> 11) * (1.0 / 9_007_199_254_740_992.0) }
}

func cumulative(_ w: [Double]) -> [Double] {
    let t = w.reduce(0, +); var acc = 0.0
    return w.map { acc += $0 / t; return acc }
}

let cs = CGColorSpace(name: CGColorSpace.sRGB)!
guard let ctx = CGContext(data: nil, width: size, height: size, bitsPerComponent: 8,
                          bytesPerRow: 0, space: cs,
                          bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else {
    fatalError("no context")
}

// Felt background (#14231C).
ctx.setFillColor(CGColor(red: 0x14/255, green: 0x23/255, blue: 0x1C/255, alpha: 1))
ctx.fill(CGRect(x: 0, y: 0, width: size, height: size))

// Fern in bone (#F4EFE6) at ~72% alpha.
ctx.setFillColor(CGColor(red: 0xF4/255, green: 0xEF/255, blue: 0xE6/255, alpha: 0.72))
var rng = XorShift(s: seed)
let cum = cumulative(weights)
let inset = 90.0
let w = Double(size) - inset * 2
let h = Double(size) - inset * 2
let scaleX = w / 6.5, scaleY = h / 11.5
let originX = Double(size) / 2, originY = Double(size) - inset
var x = 0.0, y = 0.0
for i in 0..<900_000 {
    let r = rng.unit()
    var idx = 0
    while idx < cum.count - 1 && r > cum[idx] { idx += 1 }
    let t = transforms[idx]
    let nx = t.a * x + t.b * y + t.e
    let ny = t.c * x + t.d * y + t.f
    x = nx; y = ny
    if i < 20 { continue }
    let px = originX + x * scaleX
    let py = originY - y * scaleY
    ctx.fill(CGRect(x: px, y: Double(size) - py, width: 2.0, height: 2.0))
}

guard let image = ctx.makeImage() else { fatalError("no image") }
let url = URL(fileURLWithPath: outPath)
guard let dest = CGImageDestinationCreateWithURL(url as CFURL, UTType.png.identifier as CFString, 1, nil) else {
    fatalError("no destination")
}
CGImageDestinationAddImage(dest, image, nil)
if CGImageDestinationFinalize(dest) {
    print("wrote \(outPath) (\(size)x\(size))")
} else {
    fatalError("write failed")
}
