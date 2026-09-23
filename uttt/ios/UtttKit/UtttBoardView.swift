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

    /// HOW FAR THE PEN IS ALLOWED PAST THE BOARD, as a fraction of it.
    ///
    /// The four main lines overshoot the grid by design - nobody ruling a
    /// board stops the pen neatly at the last cell - and a canvas cut tight
    /// to the board sliced every one of them off at the same clean vertical,
    /// which is the one thing a hand-drawn grid must never have. So the
    /// bitmap is bigger than the board and the board sits inside it; whatever
    /// still runs past is clipped by the SHEET, which is paper running out
    /// rather than a box.
    static let bleed: CGFloat = 0.115

    public var body: some View {
        GeometryReader { geo in
            let side = min(geo.size.width, geo.size.height)
            let pad  = side * Self.bleed
            ZStack(alignment: .topLeading) {
                Canvas { ctx, _ in
                    if let img = Self.cached(key: positionKey, active: active,
                                             last: animating == nil ? last : -1,
                                             side: side) {
                        ctx.draw(Image(decorative: img, scale: 1),
                                 in: CGRect(x: 0, y: 0,
                                            width: side + 2 * pad,
                                            height: side + 2 * pad))
                    }
                    if let a = animating {
                        ctx.translateBy(x: pad, y: pad)
                        Self.fill(Uttt.stroke(move: a.move, t: Float(a.t)),
                                  into: ctx, side: side)
                    }
                }
                .frame(width: side + 2 * pad, height: side + 2 * pad)
                .offset(x: -pad, y: -pad)
                .allowsHitTesting(false)

                // The tap map is against the BOARD, not the bled bitmap.
                Color.clear.contentShape(Rectangle())
                    .frame(width: side, height: side)
                    .onTapGesture { p in
                        guard let onTap else { return }
                        let u = p.x / side, v = p.y / side
                        guard u >= 0, u <= 1, v >= 0, v <= 1 else { return }
                        onTap(CGPoint(x: u, y: v))
                    }
            }
        }
        .aspectRatio(1, contentMode: .fit)
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

    /// WHAT THE CACHED IMAGE IS OF: the game itself, not a counter.
    ///
    /// It was keyed on `positionKey` alone, which every new model starts at
    /// the same small number - so opening a second game while the extension
    /// stayed up (a bubble tapped with the drawer open, Again, a take-back)
    /// could be handed the PREVIOUS game's picture at the same size. The seed,
    /// the game's own bytes, the wash and the heavy mark are everything the
    /// kernel draws from, so they are the key.
    private static var cacheStamp = ""
    private static var cacheSide: CGFloat = 0
    private static var cacheImage: CGImage?

    private static func stamp(active: Int, last: Int) -> String {
        let code = Uttt.code.map { String(format: "%02x", $0) }.joined()
        return "\(Uttt.seed)|\(code)|\(active)|\(last)"
    }

    /// The board at `side`, inside a bitmap bled by `bleed` on every edge, so
    /// the grid's overshoot has somewhere to go.
    static func cached(key: Int, active: Int, last: Int, side: CGFloat) -> CGImage? {
        _ = key                     // SwiftUI's reason to redraw, not the cache's
        let st = stamp(active: active, last: last)
        if st == cacheStamp, side == cacheSide, let img = cacheImage { return img }
        UtttLog.note("raster", "side \(Int(side)) plies \(Uttt.plyCount)")
        defer { UtttLog.note("raster done") }
        let scale = UIScreen.main.scale
        let pad = side * bleed
        let box = side + 2 * pad
        let px = Int(box * scale)
        guard px > 0,
              let cg = CGContext(data: nil, width: px, height: px,
                                 bitsPerComponent: 8, bytesPerRow: 0,
                                 space: CGColorSpaceCreateDeviceRGB(),
                                 bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)
        else { return nil }
        /* A CGBitmapContext has its ORIGIN AT THE BOTTOM LEFT and SwiftUI
         * does not, so a board drawn straight into one comes out mirrored
         * top to bottom - which reads as the game having been played upside
         * down rather than as a coordinate bug. Flip once, here, so the
         * kernel's coordinates mean the same thing in both places. */
        cg.translateBy(x: 0, y: box * scale)
        cg.scaleBy(x: scale, y: -scale)
        cg.translateBy(x: pad, y: pad)
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
        cacheStamp = st; cacheSide = side; cacheImage = cg.makeImage()
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
