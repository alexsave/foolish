import CUttt
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
    public let positionKey: Int     // changes when the board changes
    public let onTap: ((CGPoint) -> Void)?   // in the board's own 0..1 space

    public init(active: Int, last: Int, positionKey: Int,
                onTap: ((CGPoint) -> Void)? = nil) {
        self.active = active; self.last = last
        self.positionKey = positionKey
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
                                             last: last,
                                             side: side) {
                        ctx.draw(Image(decorative: img, scale: 1),
                                 in: CGRect(x: 0, y: 0,
                                            width: side + 2 * pad,
                                            height: side + 2 * pad))
                    }
                }
                .frame(width: side + 2 * pad, height: side + 2 * pad)
                .offset(x: -pad, y: -pad)
                .allowsHitTesting(false)
                .accessibilityHidden(true)

                // The tap map is against the BOARD, not the bled bitmap.
                Color.clear.contentShape(Rectangle())
                    .frame(width: side, height: side)
                    .onTapGesture { p in
                        guard let onTap else { return }
                        let u = p.x / side, v = p.y / side
                        guard u >= 0, u <= 1, v >= 0, v <= 1 else { return }
                        onTap(CGPoint(x: u, y: v))
                    }
                    .accessibilityHidden(true)

                UtttSquares(side: side, positionKey: positionKey, onTap: onTap)
            }
        }
        .aspectRatio(1, contentMode: .fit)
        .onReceive(NotificationCenter.default.publisher(for: Self.rendered)) { _ in landed &+= 1 }
    }

    static func fill(_ polys: [Uttt.Poly], into ctx: GraphicsContext, side: CGFloat) {
        fill(polys, into: ctx, size: CGSize(width: side, height: side))
    }

    /// The same, for a drawing that is not square (the Again door).
    static func fill(_ polys: [Uttt.Poly], into ctx: GraphicsContext, size: CGSize) {
        let sx = size.width, sy = size.height
        for poly in polys {
            var path = Path()
            guard let head = poly.points.first else { continue }
            path.move(to: CGPoint(x: head.x * sx, y: head.y * sy))
            for p in poly.points.dropFirst() {
                path.addLine(to: CGPoint(x: p.x * sx, y: p.y * sy))
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
        return cached(stamp: stamp(active: active, last: last), side: side) {
            Uttt.boardPolys(active: active, last: last)
        }
    }

    /// THE BOARD UNDER THE MOTION: every stroke but the last move's mark and
    /// no wash, so the ink, the travelling highlighter and the ring can be
    /// drawn over it every frame without touching fourteen thousand polygons.
    static func cachedUnder(side: CGFloat) -> CGImage? {
        cached(stamp: stamp(active: -2, last: -2), side: side) { Uttt.underPolys() }
    }

    /// True when the image the live board needs is the one in the cache - the
    /// motion clock does not start until the board it moves over is visible.
    static var underReady: Bool { cacheStamp == stamp(active: -2, last: -2) && cacheImage != nil }

    private static func cached(stamp st: String, side: CGFloat,
                               polys make: () -> Uttt.BoardPolys) -> CGImage? {
        if st == cacheStamp, side == cacheSide, let img = cacheImage { return img }
        let scale = UIScreen.main.scale
        let polys = make()
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

/// The board the player plays on: the cached board under the motion, and
/// over it whatever the kernel's frame says - the travelling highlighter,
/// the destination ring and the last move's ink. Draws; decides nothing.
public struct UtttLiveBoard: View {
    @ObservedObject var clock: UtttMotionClock
    public let positionKey: Int
    public let onTap: ((CGPoint) -> Void)?

    public init(clock: UtttMotionClock, positionKey: Int,
                onTap: ((CGPoint) -> Void)? = nil) {
        self.clock = clock; self.positionKey = positionKey; self.onTap = onTap
    }

    @State private var landed = 0

    public var body: some View {
        GeometryReader { geo in
            let side = min(geo.size.width, geo.size.height)
            let pad  = side * UtttBoard.bleed
            let f = clock.frame
            ZStack(alignment: .topLeading) {
                Canvas { ctx, _ in
                    _ = landed; _ = positionKey
                    ctx.translateBy(x: pad, y: pad)
                    /* the highlighter first: it is under the ink */
                    if f.wash.2 > 0 {
                        ctx.fill(Path(Self.rect(f.wash, side)), with: .color(Self.color(f.wash_rgba)))
                    }
                    /* the ring stands out from the block's edge, under the grid */
                    if f.pulse.2 > 0, f.pulse_rgba & 0xff > 0 {
                        let inner = Self.rect(f.pulse, side)
                        let outer = inner.insetBy(dx: -CGFloat(f.pulse_spread) * side,
                                                  dy: -CGFloat(f.pulse_spread) * side)
                        var ring = Path(outer)
                        ring.addRect(inner)
                        ctx.fill(ring, with: .color(Self.color(f.pulse_rgba)),
                                 style: FillStyle(eoFill: true))
                    }
                    if let img = UtttBoard.cachedUnder(side: side) {
                        ctx.draw(Image(decorative: img, scale: 1),
                                 in: CGRect(x: -pad, y: -pad,
                                            width: side + 2 * pad, height: side + 2 * pad))
                        /* the last mark only once the board it lands on is up */
                        UtttBoard.fill(Uttt.lastStroke(t: f.mark_t), into: ctx, side: side)
                    }
                }
                .frame(width: side + 2 * pad, height: side + 2 * pad)
                .offset(x: -pad, y: -pad)
                .allowsHitTesting(false)
                .accessibilityHidden(true)

                Color.clear.contentShape(Rectangle())
                    .frame(width: side, height: side)
                    .onTapGesture { p in
                        guard let onTap else { return }
                        let u = p.x / side, v = p.y / side
                        guard u >= 0, u <= 1, v >= 0, v <= 1 else { return }
                        onTap(CGPoint(x: u, y: v))
                    }
                    .accessibilityHidden(true)

                UtttSquares(side: side, positionKey: positionKey, onTap: onTap)
                if UtttRuler.on { Self.rulerMarks(f, side: side) }
            }
        }
        .aspectRatio(1, contentMode: .fit)
        .onReceive(NotificationCenter.default.publisher(for: UtttBoard.rendered)) { _ in landed &+= 1 }
    }

    /// The ruler's two marks inside the board (UtttRuler): pink on the
    /// highlighter's centre, violet on the centre of the pen stroke drawn so
    /// far. Positioned in the same board space the Canvas draws in.
    @ViewBuilder
    private static func rulerMarks(_ f: UtiFrame, side: CGFloat) -> some View {
        let dot = MotionRuler.side
        if f.wash.2 > 0 {
            let w = rect(f.wash, side)
            Color.clear.frame(width: dot, height: dot)
                .motionSquare(.pink, on: true)
                .position(x: w.midX, y: w.midY)
                .allowsHitTesting(false)
        }
        let pts = Uttt.lastStroke(t: f.mark_t).flatMap(\.points)
        if let x0 = pts.map(\.x).min(), let x1 = pts.map(\.x).max(),
           let y0 = pts.map(\.y).min(), let y1 = pts.map(\.y).max() {
            Color.clear.frame(width: dot, height: dot)
                .motionSquare(.violet, on: true)
                .position(x: (x0 + x1) / 2 * side, y: (y0 + y1) / 2 * side)
                .allowsHitTesting(false)
        }
    }

    private static func rect(_ r: (Float, Float, Float, Float), _ side: CGFloat) -> CGRect {
        CGRect(x: CGFloat(r.0) * side, y: CGFloat(r.1) * side,
               width: CGFloat(r.2) * side, height: CGFloat(r.3) * side)
    }

    private static func color(_ c: UInt32) -> Color {
        Color(.sRGB, red: Double((c >> 24) & 0xff) / 255,
              green: Double((c >> 16) & 0xff) / 255,
              blue: Double((c >> 8) & 0xff) / 255,
              opacity: Double(c & 0xff) / 255)
    }
}

/// WHAT VOICEOVER FINDS ON THE BOARD: one element per square, where the
/// square is. The rectangle and the words are both the kernel's
/// (`uttt_cell_rect`, the inverse of the tap map, and `uttt_say_cell`), so
/// this places and labels and decides nothing. A square the player may take
/// is a button, and activating it is the same tap a finger makes.
struct UtttSquares: View {
    let side: CGFloat
    let positionKey: Int
    let onTap: ((CGPoint) -> Void)?

    var body: some View {
        let legal = onTap != nil && Uttt.canMove ? Set(Uttt.legal.map(Int.init)) : []
        ZStack(alignment: .topLeading) {
            ForEach(0..<81, id: \.self) { mv in
                let r = Uttt.cellRect(mv)
                Color.clear
                    .frame(width: r.width * side, height: r.height * side)
                    .accessibilityElement()
                    .accessibilityLabel(Uttt.sayCell(mv))
                    .accessibilityAction {
                        onTap?(CGPoint(x: r.midX, y: r.midY))
                    }
                    /* An action makes any element a button; only a square
                     * the player may take is one. */
                    .accessibilityRemoveTraits(legal.contains(mv) ? [] : .isButton)
                    .accessibilityAddTraits(legal.contains(mv) ? .isButton : [])
                    .position(x: r.midX * side, y: r.midY * side)
            }
        }
        .frame(width: side, height: side, alignment: .topLeading)
        .allowsHitTesting(false)
        .id(positionKey)
    }
}
