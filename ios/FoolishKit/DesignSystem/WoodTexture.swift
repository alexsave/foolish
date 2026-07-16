// WoodTexture.swift — the wood-grain button/chrome material (§IOS_PHONE_LAYOUT
// §4). A faithful CPU port of the web's WoodTexture generator (CPU-fallback
// path): a dark red-brown base with 576 overlaid grain streaks from a chaotic
// cosine map, edge-softened per streak. The math is ported, not the WebGL.
//
// The per-pixel k-loop is expensive, so we render ONE small swatch, cache it in
// memory and on disk, and stretch it (resizable) under every wooden control —
// buttons are small and share one wood look.

import SwiftUI
import CoreGraphics
import UIKit

public enum WoodTexture {

    private static let version = 2
    private static var mem: [String: UIImage] = [:]
    private static let lock = NSLock()

    /// A wood swatch `w×h` px (kept small — it is stretched under controls).
    public static func image(w: Int = 300, h: Int = 120) -> UIImage {
        let key = "wood-v\(version)-\(w)x\(h)"
        lock.lock(); if let img = mem[key] { lock.unlock(); return img }; lock.unlock()
        if let disk = loadDisk(key) {
            lock.lock(); mem[key] = disk; lock.unlock(); return disk
        }
        let img = render(w: w, h: h)
        lock.lock(); mem[key] = img; lock.unlock()
        saveDisk(img, key)
        return img
    }

    private static func render(w: Int, h: Int) -> UIImage {
        let count = w * h * 4
        var data = [UInt8](repeating: 0, count: count)
        data.withUnsafeMutableBufferPointer { buf in
            let p = buf.baseAddress!
            var i = 0
            while i < count { p[i] = 70; p[i+1] = 14; p[i+2] = 9; p[i+3] = 255; i += 4 } // base 70/14/9
        }

        let offX = 0.0, offY = 0.0
        var iFactors = [Double](repeating: 0, count: h)
        for I in 0..<h { iFactors[I] = (Double(I) + offY) * 0.001 }

        data.withUnsafeMutableBufferPointer { buf in
            let d = buf.baseAddress!

            func drawColumn(_ T: Double) {
                let xPosFloat = ((T + offX / 200) * 200).truncatingRemainder(dividingBy: Double(w))
                let xPos = Int(xPosFloat)
                let rectW = 40
                let xEnd = min(xPos + rectW, w)
                let xCenter = Double(xPos) + Double(rectW) / 2
                var I = h - 1
                while I >= 0 {
                    let iFactor = iFactors[I]
                    var b = (T + offX * 0.01) / 24
                    var k = 24
                    while k >= 0 {
                        let bHalf = (b * b) * 0.5
                        b = cos(iFactor + cos(bHalf) * b + 4 + offY * 0.001) * b - 2.8
                        if b > 0 {
                            let red = b * 120
                            let green = b * b * 14
                            let blue = 9.0
                            var x = xPos
                            while x < xEnd {
                                let idx = (I * w + x) * 4
                                let dist = abs(Double(x) - xCenter) / (Double(rectW) / 2)
                                let edge = max(0.3, 1 - dist * 0.5)
                                let a = 0.1 * edge, ia = 1 - a
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

        return imageFromRGBA(&data, w: w, h: h)
    }

    private static func imageFromRGBA(_ data: inout [UInt8], w: Int, h: Int) -> UIImage {
        let cs = CGColorSpaceCreateDeviceRGB()
        let info = CGImageAlphaInfo.premultipliedLast.rawValue
        let cg = data.withUnsafeMutableBytes { raw -> CGImage? in
            CGContext(data: raw.baseAddress, width: w, height: h, bitsPerComponent: 8,
                      bytesPerRow: w * 4, space: cs, bitmapInfo: info)?.makeImage()
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
