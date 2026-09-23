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

    /// Bumped when an off-main render lands, so the Canvas draws again.
    @State private var landed = 0

    public var body: some View {
        GeometryReader { geo in
            let side = min(geo.size.width, geo.size.height)
            let pad  = side * Self.bleed
            ZStack(alignment: .topLeading) {
                Canvas { ctx, _ in
                    _ = landed
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
        .onReceive(NotificationCenter.default.publisher(for: Self.rendered)) { _ in landed &+= 1 }
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

    /// Posted on the main thread when a board rendered off it is ready.
    static let rendered = Notification.Name("UtttBoard.rendered")
    private static var inflight: String?

    /// The board at `side`, inside a bitmap bled by `bleed` on every edge, so
    /// the grid's overshoot has somewhere to go.
    ///
    /// THE FIRST BOARD OF A PROCESS IS RENDERED OFF THE MAIN THREAD. A late
    /// board is fourteen thousand fills, measured at 140-250 ms in a Debug
    /// build (`uttt-probe`: the kernel's draw is half a millisecond, the rest
    /// is CoreGraphics), and on a cold open that time was spent before the
    /// drawer's first frame - so the drawer stayed Messages' grey card for it.
    /// Now the first frame is the paper and the words, and the board lands a
    /// few frames later, while the drawer is still settling. Only the FIRST:
    /// after that a stale image is on screen, and swapping a finished move's
    /// image in late would flash the move out and back in.
    static func cached(key: Int, active: Int, last: Int, side: CGFloat) -> CGImage? {
        _ = key                     // SwiftUI's reason to redraw, not the cache's
        let st = stamp(active: active, last: last)
        if st == cacheStamp, side == cacheSide, let img = cacheImage { return img }
        let scale = UIScreen.main.scale
        let polys = Uttt.boardPolys(active: active, last: last)
        UtttLog.note("raster", "side \(Int(side)) plies \(Uttt.plyCount) polys \(polys.first.count)")
        guard cacheImage == nil else {
            let img = render(polys, side: side, scale: scale)
            UtttLog.note("raster done")
            cacheStamp = st; cacheSide = side; cacheImage = img
            return img
        }
        let job = "\(st)|\(side)"
        guard inflight != job else { return nil }
        inflight = job
        DispatchQueue.global(qos: .userInteractive).async {
            let img = render(polys, side: side, scale: scale)
            DispatchQueue.main.async {
                UtttLog.note("raster done", "off the main thread")
                if inflight == job { inflight = nil }
                if cacheImage == nil || (cacheStamp == st && cacheSide == side) || inflight == nil {
                    cacheStamp = st; cacheSide = side; cacheImage = img
                }
                NotificationCenter.default.post(name: rendered, object: nil)
            }
        }
        return nil
    }

    /// Pure: the polygons into a new bitmap. Safe on any thread.
    private static func render(_ polys: Uttt.BoardPolys, side: CGFloat, scale: CGFloat) -> CGImage? {
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
        Uttt.fill(polys, into: cg, side: side)
        return cg.makeImage()
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
