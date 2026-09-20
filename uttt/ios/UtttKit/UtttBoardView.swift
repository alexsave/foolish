import CoreGraphics
import SwiftUI

/// Fills what the kernel hands over, and decides nothing.
///
/// THE BOARD IS CACHED AS AN IMAGE. A finished position is fourteen thousand
/// polygons, which is fine to fill once and hopeless to fill sixty times a
/// second - so the position is rasterised when it changes and the animating
/// stroke, a few dozen polygons, is composited on top of it.
public struct UtttBoard: View {
    public let active: Int          // block 0..8, 9 anywhere, -1 none
    public let last: Int            // block*9+cell, or -1
    public let animating: (move: Int, t: Double)?
    public let positionKey: Int     // changes when the board changes
    public let onTap: ((CGPoint) -> Void)?   // in the board's own 0..1 space

    public init(active: Int, last: Int, positionKey: Int,
                animating: (move: Int, t: Double)? = nil,
                onTap: ((CGPoint) -> Void)? = nil) {
        self.active = active; self.last = last
        self.positionKey = positionKey; self.animating = animating
        self.onTap = onTap
    }

    public var body: some View {
        Canvas { ctx, size in
            let side = min(size.width, size.height)
            if let img = Self.cached(key: positionKey, active: active,
                                     last: animating == nil ? last : -1,
                                     side: side) {
                ctx.draw(Image(decorative: img, scale: 1),
                         in: CGRect(x: 0, y: 0, width: side, height: side))
            }
            if let a = animating {
                Self.fill(Uttt.stroke(move: a.move, t: Float(a.t)),
                          into: ctx, side: side)
            }
        }
        .aspectRatio(1, contentMode: .fit)
        .drawingGroup()
        .overlay(GeometryReader { geo in
            // The board is square inside whatever box it is given, so a tap
            // has to be mapped against the SIDE and not the box - otherwise
            // every column is off by half the letterboxing.
            Color.clear.contentShape(Rectangle())
                .onTapGesture { p in
                    guard let onTap else { return }
                    let side = min(geo.size.width, geo.size.height)
                    let ox = (geo.size.width  - side) / 2
                    let oy = (geo.size.height - side) / 2
                    let u = (p.x - ox) / side, v = (p.y - oy) / side
                    guard u >= 0, u <= 1, v >= 0, v <= 1 else { return }
                    onTap(CGPoint(x: u, y: v))
                }
        })
    }

    static func fill(_ polys: [Uttt.Poly], into ctx: GraphicsContext, side: CGFloat) {
        for poly in polys {
            var path = Path()
            guard let head = poly.points.first else { continue }
            path.move(to: CGPoint(x: head.x * side, y: head.y * side))
            for p in poly.points.dropFirst() {
                path.addLine(to: CGPoint(x: p.x * side, y: p.y * side))
            }
            path.closeSubpath()
            ctx.fill(path, with: .color(Color(poly.color)))
        }
    }

    // MARK: the one-entry cache
    private static var cacheKey: Int = .min
    private static var cacheSide: CGFloat = 0
    private static var cacheImage: CGImage?

    static func cached(key: Int, active: Int, last: Int, side: CGFloat) -> CGImage? {
        if key == cacheKey, side == cacheSide, let img = cacheImage { return img }
        let scale = UIScreen.main.scale
        let px = Int(side * scale)
        guard px > 0,
              let cg = CGContext(data: nil, width: px, height: px,
                                 bitsPerComponent: 8, bytesPerRow: 0,
                                 space: CGColorSpaceCreateDeviceRGB(),
                                 bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)
        else { return nil }
        cg.scaleBy(x: scale, y: scale)
        for poly in Uttt.board(active: active, last: last) {
            guard let head = poly.points.first else { continue }
            cg.beginPath()
            cg.move(to: CGPoint(x: head.x * side, y: head.y * side))
            for p in poly.points.dropFirst() {
                cg.addLine(to: CGPoint(x: p.x * side, y: p.y * side))
            }
            cg.closePath()
            cg.setFillColor(poly.color)
            cg.fillPath()
        }
        cacheKey = key; cacheSide = side; cacheImage = cg.makeImage()
        return cacheImage
    }
}

/// The side indicator - the same X that is about to land on the board, drawn
/// by the same code, because a glyph from a font would be the only thing in
/// the frame that did not come out of the pen.
public struct UtttMarkIcon: View {
    public let mark: Uttt.Mark
    public let seed: Int32
    public init(mark: Uttt.Mark, seed: Int32) { self.mark = mark; self.seed = seed }
    public var body: some View {
        Canvas { ctx, size in
            UtttBoard.fill(Uttt.mark(mark, seed: seed), into: ctx,
                           side: min(size.width, size.height))
        }
        .aspectRatio(1, contentMode: .fit)
    }
}
