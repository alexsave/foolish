// The 243 diagnostics' test JPEGs (tests/uttt_big_diag_test.c), written once
// on macOS 27 with ImageIO and committed; ImageIO writes the same bytes every
// time, so running this again reproduces them.
//
//     swiftc -O make_fixtures.swift -o /tmp/mf && /tmp/mf uttt/c/tests/fixtures
//
// The cjpeg_* files are libjpeg-turbo's cjpeg (Homebrew) from
// imageio_q100_444.jpg decoded to a PPM (any decoder):
//     cjpeg -quality 50 -sample 1x1 src.ppm > cjpeg_q50_444.jpg
//     cjpeg -quality 75 -sample 2x1 src.ppm > cjpeg_q75_422.jpg
//     cjpeg -quality 90 -progressive src.ppm > cjpeg_q90_420_progressive.jpg
// messages_sim_q050_243.jpeg is Messages' own file for a bubble's picture,
// taken from the iOS 27 simulator by the layout probe (2026-10-01).
import Foundation
import ImageIO
import CoreGraphics
import UniformTypeIdentifiers
let out = CommandLine.arguments[1]
func image(_ w: Int, _ h: Int, grey: Bool) -> CGImage {
  let comps = grey ? 1 : 4
  var px = [UInt8](repeating: 0, count: w*h*comps)
  for y in 0..<h { for x in 0..<w {
    let lv: [UInt8] = [255, 128, 0]
    let v = lv[((x/3) * 7 + (y/3) * 13) % 3]
    if grey { px[y*w+x] = v } else { let i = 4*(y*w+x); px[i] = v; px[i+1] = UInt8((Int(v) + x) & 255); px[i+2] = v; px[i+3] = 255 }
  } }
  let cs = grey ? CGColorSpaceCreateDeviceGray() : CGColorSpace(name: CGColorSpace.sRGB)!
  let ctx = CGContext(data: &px, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w*comps, space: cs, bitmapInfo: grey ? CGImageAlphaInfo.none.rawValue : CGImageAlphaInfo.noneSkipLast.rawValue)!
  return ctx.makeImage()!
}
func write(_ name: String, _ im: CGImage, _ q: Double, progressive: Bool = false) {
  let url = URL(fileURLWithPath: out + "/" + name) as CFURL
  let dst = CGImageDestinationCreateWithURL(url, UTType.jpeg.identifier as CFString, 1, nil)!
  var opts: [CFString: Any] = [kCGImageDestinationLossyCompressionQuality: q]
  if progressive { opts[kCGImagePropertyJFIFDictionary] = [kCGImagePropertyJFIFIsProgressive: true] }
  CGImageDestinationAddImage(dst, im, opts as CFDictionary)
  CGImageDestinationFinalize(dst)
}
let rgb = image(96, 99, grey: false)
write("imageio_q050_420.jpg", rgb, 0.50)
write("imageio_q089_420.jpg", rgb, 0.89)
write("imageio_q0915_420.jpg", rgb, 0.915)
write("imageio_q100_444.jpg", rgb, 1.0)
write("imageio_q050_progressive.jpg", rgb, 0.50, progressive: true)
write("imageio_q050_grey.jpg", image(96, 99, grey: true), 0.50)
