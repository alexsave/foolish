// WoolTexture.swift — the woven-wool table surface (§IOS_PHONE_LAYOUT §4:
// "the phone app should lean on what the website does — wool/wood/fern").
// A faithful CPU port of the web's WoolBackground generator (the CPU-fallback
// path, which writes a flat RGBA buffer — the same shape as our UInt8 buffer):
// a brown base woven by a horizontal fiber phase then a vertical one, with a
// tan-XOR plaid modulating the colour. The math is ported, not the WebGL.
//
// Millions of pixel writes rule out per-point CGContext.fill (what FernCardBack
// can afford at 42k iterations) — we write straight into a raw buffer and wrap
// it in one CGImage. Rendered once per size, cached in memory and on disk so a
// cold launch pays for it at most once ever.

import SwiftUI
import CoreGraphics
import UIKit

public enum WoolTexture {

    // Bump when the algorithm changes so the on-disk cache invalidates.
    private static let version = 4

    private static var mem: [String: UIImage] = [:]
    private static let lock = NSLock()

    /// A wool image `w×h` px. Deterministic (fixed offsets) so it is stable
    /// across launches and safe to cache to disk. Synchronous; call off-main.
    public static func image(w: Int, h: Int) -> UIImage {
        let key = "wool-v\(version)-\(w)x\(h)"
        lock.lock(); if let img = mem[key] { lock.unlock(); return img }; lock.unlock()

        if let disk = loadDisk(key) {
            lock.lock(); mem[key] = disk; lock.unlock()
            return disk
        }
        let img = render(w: w, h: h)
        lock.lock(); mem[key] = img; lock.unlock()
        saveDisk(img, key)
        return img
    }

    private static func render(w: Int, h: Int) -> UIImage {
        let count = w * h * 4
        var data = [UInt8](repeating: 0, count: count)

        // Brown base (web BASE_R/G/B = 113/65/27), opaque.
        data.withUnsafeMutableBufferPointer { buf in
            let p = buf.baseAddress!
            var i = 0
            while i < count { p[i] = 113; p[i+1] = 65; p[i+2] = 27; p[i+3] = 255; i += 4 }
        }

        let offX = 0.0, offY = 0.0
        func zValue(_ r: Double) -> Double { let cr = cos(r) * 1000; return cr - floor(cr) }

        // The plaid block size. The web's fixed `/80` is in pixels, so blocks
        // shrink as the render grows; tie it to the render width instead (~5
        // blocks across, matching the website's large soft plaid) so the weave
        // can be high-resolution while the blocks stay big.
        let blockPx = Double(w) / 5.0

        // Iteration budget. The web renders at 4K where the area-scaled count
        // already exceeds what the vertical-fibre phase needs to reach the bottom
        // (~h²/1.8); at our smaller size that phase would stop short and leave
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
                    let red: Double = tan(Double(aInt ^ bInt)) > 0.3 ? 100 : 0
                    let cr = 209 + 46 * phase + red
                    let cg = 208 + 45 * phase - red
                    let cb = 183 + 53 * phase - red / 2
                    let x = r + dx, y = Double(i % h)
                    if x >= 0, x < Double(w), y >= 0, y < Double(h) { writePixel(x, y, cr, cg, cb, 2) }
                } else {
                    if i % h == 0 { u = zValue(r) * 500 + 100; r += 3 }
                    let phase = sin(Double(i) / u)
                    let zVal = zValue(r)
                    let aInt = Int(floor((r + offY) / blockPx + zVal / 4))
                    let bInt = Int(floor((Double(i % w) + offX) / blockPx))
                    let red: Double = tan(Double(aInt ^ bInt)) > 0.3 ? 100 : 0
                    let dx = 4 * sin(Double(i - 1)) + sin(Double(i) / u) * 4
                    let cr = 189 + 46 * phase + red
                    let cg = 188 + 45 * phase - red
                    let cb = 163 + 53 * phase - red / 2
                    let x = Double(i % w), y = r + dx
                    let pw = 1.4 * (phase + 1.7)
                    if x >= 0, x < Double(w), y >= 0, y < Double(h) { writePixel(x, y, cr, cg, cb, pw) }
                }
                i += 1
            }
        }

        return imageFromRGBA(&data, w: w, h: h)
    }

    // MARK: - buffer → image + disk cache

    private static func imageFromRGBA(_ data: inout [UInt8], w: Int, h: Int) -> UIImage {
        let cs = CGColorSpaceCreateDeviceRGB()
        let info = CGImageAlphaInfo.premultipliedLast.rawValue
        let cg = data.withUnsafeMutableBytes { raw -> CGImage? in
            let ctx = CGContext(data: raw.baseAddress, width: w, height: h,
                                bitsPerComponent: 8, bytesPerRow: w * 4,
                                space: cs, bitmapInfo: info)
            return ctx?.makeImage()
        }
        return cg.map { UIImage(cgImage: $0) } ?? UIImage()
    }

    private static func cacheURL(_ key: String) -> URL? {
        let dir = try? FileManager.default.url(for: .cachesDirectory, in: .userDomainMask,
                                               appropriateFor: nil, create: true)
        return dir?.appendingPathComponent("\(key).png")
    }

    private static func loadDisk(_ key: String) -> UIImage? {
        guard let url = cacheURL(key), let data = try? Data(contentsOf: url) else { return nil }
        return UIImage(data: data)
    }

    private static func saveDisk(_ img: UIImage, _ key: String) {
        guard let url = cacheURL(key), let png = img.pngData() else { return }
        try? png.write(to: url, options: .atomic)
    }
}
