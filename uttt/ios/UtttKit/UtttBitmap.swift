import CoreGraphics
import IOSurface
import QuartzCore
import SwiftUI
import UIKit

/// ONE BITMAP, SHARED WITH THE COMPOSITOR (TESTFLIGHT_PLAN.md 12, memory).
///
/// The board and the paper were CGImages shown by SwiftUI `Image`s, and a
/// CGImage handed to Core Animation is COPIED into the render server's
/// shared memory: vmmap of an idle drawer had the board twice (CG raster
/// 1,085 KB and a CoreAnimation region of 1,088 KB) and the paper twice
/// (689 KB of malloc and 720 KB of CoreAnimation). An IOSurface is memory the
/// render server maps as it is, so a layer whose contents is one draws from
/// the very pixels Core Graphics painted - one copy, not two.
///
/// Painted once, then read-only: nothing writes a surface after `init`
/// returns, so it is safe to build on any thread and hand to the main one.
public final class UtttBitmap: @unchecked Sendable {
    public let surface: IOSurface
    public let width: Int
    public let height: Int

    /// A BGRA (premultiplied, the compositor's own layout) surface of
    /// `width` x `height` pixels, cleared, with `paint` given a context on it
    /// whose origin is Core Graphics' own, bottom left.
    public init?(width: Int, height: Int, paint: (CGContext) -> Void) {
        guard width > 0, height > 0,
              let s = IOSurface(properties: [
                  .width: width, .height: height,
                  .bytesPerElement: 4,
                  .pixelFormat: 0x4247_5241,          // 'BGRA'
              ])
        else { return nil }
        s.lock(options: [], seed: nil)
        defer { s.unlock(options: [], seed: nil) }
        memset(s.baseAddress, 0, s.bytesPerRow * height)
        guard let cg = CGContext(data: s.baseAddress, width: width, height: height,
                                 bitsPerComponent: 8, bytesPerRow: s.bytesPerRow,
                                 space: CGColorSpaceCreateDeviceRGB(),
                                 bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue
                                     | CGBitmapInfo.byteOrder32Little.rawValue)
        else { return nil }
        paint(cg)
        surface = s
        self.width = width
        self.height = height
    }

    /// Raw rows written straight into the surface: `fill(base, bytesPerRow)`.
    public init?(width: Int, height: Int, rows fill: (UnsafeMutableRawPointer, Int) -> Void) {
        guard width > 0, height > 0,
              let s = IOSurface(properties: [
                  .width: width, .height: height,
                  .bytesPerElement: 4,
                  .pixelFormat: 0x4247_5241,
              ])
        else { return nil }
        s.lock(options: [], seed: nil)
        fill(s.baseAddress, s.bytesPerRow)
        s.unlock(options: [], seed: nil)
        surface = s
        self.width = width
        self.height = height
    }
}

/// A bitmap on screen, stretched to whatever frame SwiftUI gives it: a plain
/// layer whose contents is the surface, so a resize is a transform the
/// compositor does and nothing is drawn.
struct UtttSurface: UIViewRepresentable {
    let bitmap: UtttBitmap?

    final class View: UIView {
        override init(frame: CGRect) {
            super.init(frame: frame)
            isUserInteractionEnabled = false
            isAccessibilityElement = false
            layer.contentsGravity = .resize
        }
        required init?(coder: NSCoder) { fatalError() }
        var shown: UtttBitmap?
    }

    func makeUIView(context: Context) -> View { View() }

    func updateUIView(_ v: View, context: Context) {
        guard v.shown !== bitmap else { return }
        v.shown = bitmap
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        v.layer.contents = bitmap?.surface
        CATransaction.commit()
    }
}
