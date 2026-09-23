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
    /// no wash, so the ink and the travelling highlighter can be
    /// drawn over it every frame without touching fourteen thousand polygons.
    static func cachedUnder(side: CGFloat) -> CGImage? {
        cached(stamp: stamp(active: -2, last: -2), side: side) { Uttt.underPolys() }
    }

    /// True when the image the live board needs is the one in the cache - the
    /// motion clock does not start until the board it moves over is visible.
    static var underReady: Bool { cacheStamp == stamp(active: -2, last: -2) && cacheImage != nil }

    private static func cached(stamp st: String, side: CGFloat,
                               polys make: () -> Uttt.BoardPolys) -> CGImage? {
        if st == cacheStamp, let img = cacheImage {
            if side == cacheSide { return img }
            /* THE SAME BOARD AT A NEW SIZE: the drawer is being dragged, or
             * Messages handed a new height. The picture on hand is drawn
             * into the new rectangle (scaled, for a few frames) and the
             * sharp one is painted off the main thread. Painting it here
             * cost 50-60 ms of main thread per height, and a drag hands a
             * new height every frame - measured with the ruler, the board
             * moved in 200 ms steps under a finger moving every 16 ms. */
            resize(stamp: st, side: side, polys: make())
            return img
        }
        let scale = UIScreen.main.scale
        let polys = make()
        UtttLog.note("raster", "side \(Int(side)) plies \(Uttt.plyCount) polys \(polys.first.count)")
        guard cacheImage == nil else {
            let img = render(polys, side: side, scale: scale)
            UtttLog.note("raster done")
            cacheStamp = st; cacheSide = side; cacheImage = img
            return img
        }
        /* BUT THE LINES ARE IN THE FIRST FRAME (sheet 2: blank paper, then
         * the wash with no board under it for ~0.4s). The first board is
         * painted HERE at one pixel per point - a ninth of the pixels of a
         * 3x screen - so the frame that shows the paper shows the grid, and
         * the sharp one is painted off the main thread and swapped in, the
         * same picture at a finer grain. */
        let quick = render(polys, side: side, scale: min(scale, Self.firstScale))
        UtttLog.note("raster done", "first, at \(min(scale, Self.firstScale))x")
        cacheStamp = st; cacheSide = side; cacheImage = quick
        if scale > Self.firstScale { resize(stamp: st, side: side, polys: polys) }
        return quick
    }

    /// The first board's grain: one pixel per point.
    private static let firstScale: CGFloat = 1

    /// The side the newest resize asked for, and whether one is painting.
    /// ONE PAINT AT A TIME: a drag asks every frame, and the paint that lands
    /// is followed by one more at the latest side if the finger moved on.
    private static var wantSide: CGFloat = 0
    private static var resizing = false

    private static func resize(stamp st: String, side: CGFloat, polys: Uttt.BoardPolys) {
        wantSide = side
        guard !resizing else { return }
        resizing = true
        let scale = UIScreen.main.scale
        DispatchQueue.global(qos: .userInteractive).async {
            let img = render(polys, side: side, scale: scale)
            DispatchQueue.main.async {
                resizing = false
                guard cacheStamp == st else { return }
                cacheSide = side; cacheImage = img
                if wantSide != side {
                    resize(stamp: st, side: wantSide, polys: polys)
                }
                NotificationCenter.default.post(name: rendered, object: nil)
            }
        }
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
                                 /* THE COMPOSITOR'S OWN LAYOUT, BGRA little-endian:
                                  * an RGBA bitmap was converted channel by channel
                                  * every time it was drawn at a new size, which is
                                  * every frame of a drawer move (vImage permute,
                                  * the top of the stack in a `sample` of one). */
                                 bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue
                                     | CGBitmapInfo.byteOrder32Little.rawValue)
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
/// over it whatever the kernel's frame says - the highlighter, the last
/// move's ink, its settlement and the outline of the block it sends to.
/// Draws; decides nothing.
///
/// NOTHING HERE RE-RENDERS A DRAWING PER FRAME THAT HAS NOT CHANGED
/// (TESTFLIGHT_PLAN.md 12). This view used to observe the clock itself, so
/// every display frame re-ran its whole body - the 81 VoiceOver squares, the
/// cache's stamp - and re-rendered two board-sized Canvases through
/// RenderBox on the main thread, the wash's included. A `sample` of a stage on
/// the SE simulator put 283 of 543 busy main-thread samples in RenderBox
/// waiting on Metal (`waitUntilScheduled`), and the ink's 740 ms plan drew 6
/// frames. Now the clock is observed only by three small layers: the wash is
/// a coloured rect (a layer the compositor moves, no drawing), and the ink
/// and the outline are Canvases that redraw only when their own `t` changes
/// - so the highlighter's travel, the whole of the post-settlement, draws
/// nothing at all.
public struct UtttLiveBoard: View {
    let clock: UtttMotionClock
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
            let _ = (landed, positionKey)   // an off-main paint landed, a move
            ZStack(alignment: .topLeading) {
                /* the highlighter first: it is under the ink */
                UtttWashLayer(clock: clock, side: side)
                    .frame(width: side, height: side)
                    .allowsHitTesting(false)
                    .accessibilityHidden(true)

                /* THE CACHED BOARD IS AN IMAGE VIEW, NOT A DRAW INTO A
                 * CANVAS: a view's picture is a texture the compositor scales,
                 * so a drawer move that resizes the board every frame costs a
                 * transform. It is fetched for every new position (the model's
                 * `positionKey`, which every run of the clock bumps). */
                if let img = UtttBoard.cachedUnder(side: side) {
                    Image(decorative: img, scale: 1)
                        .resizable()
                        .interpolation(.high)
                        .frame(width: side + 2 * pad, height: side + 2 * pad)
                        .offset(x: -pad, y: -pad)
                        .allowsHitTesting(false)
                        .accessibilityHidden(true)
                    /* the strokes only once the board they land on is up */
                    UtttStrokeLayer(clock: clock, side: side, pad: pad, key: positionKey)
                        .frame(width: side, height: side)
                        .allowsHitTesting(false)
                        .accessibilityHidden(true)
                }

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
        .onReceive(NotificationCenter.default.publisher(for: UtttBoard.rendered)) { _ in landed &+= 1 }
    }

    static func rect(_ r: (Float, Float, Float, Float), _ side: CGFloat) -> CGRect {
        CGRect(x: CGFloat(r.0) * side, y: CGFloat(r.1) * side,
               width: CGFloat(r.2) * side, height: CGFloat(r.3) * side)
    }

    static func color(_ c: UInt32) -> Color {
        Color(.sRGB, red: Double((c >> 24) & 0xff) / 255,
              green: Double((c >> 16) & 0xff) / 255,
              blue: Double((c >> 8) & 0xff) / 255,
              opacity: Double(c & 0xff) / 255)
    }
}

/// THE HIGHLIGHTER: one coloured rect where the kernel's frame puts it, on a
/// Core Animation layer the clock moves directly - nothing is drawn when it
/// travels, and SwiftUI is not asked about it.
struct UtttWashLayer: UIViewRepresentable {
    let clock: UtttMotionClock
    let side: CGFloat

    func makeUIView(context: Context) -> UtttMotionView { UtttMotionView(role: .wash) }
    func updateUIView(_ v: UtttMotionView, context: Context) {
        v.bind(clock, side: side, key: 0)
    }
}

/// THE STROKES THAT MOVE: the last mark and its settlement, and the outline
/// under its own opacity, each a bitmap of just its own ink on a layer of
/// its own, painted again only when its own `t` moves.
struct UtttStrokeLayer: UIViewRepresentable {
    let clock: UtttMotionClock
    let side: CGFloat
    let pad: CGFloat
    let key: Int

    func makeUIView(context: Context) -> UtttMotionView { UtttMotionView(role: .strokes) }
    func updateUIView(_ v: UtttMotionView, context: Context) {
        v.bind(clock, side: side, key: key)
    }
}

/// WHAT DRAWS A FRAME, OFF SWIFTUI (TESTFLIGHT_PLAN.md 12).
///
/// Measured on the SE simulator: every Canvas or Text SwiftUI re-renders is a
/// RenderBox pass on the main thread that then WAITS for Metal
/// (`waitUntilScheduled`) - 336 of ~900 busy main-thread samples in one
/// stage, and a display link that could only tick every ~90 ms. So nothing
/// that moves is SwiftUI: this view's layers are set straight from the
/// clock's frame (`UtttMotionClock.observe`), the wash as a layer's colour
/// and frame and the ink as a small bitmap of only its own polygons,
/// painted by Core Graphics on the CPU (a few hundred fills, well under a
/// millisecond) and handed to the render server as layer contents.
final class UtttMotionView: UIView {
    enum Role { case wash, strokes }
    private let role: Role
    private weak var clock: UtttMotionClock?
    private var token: Int?
    private var side: CGFloat = 0
    private var key = 0

    private let wash = CALayer()
    private let ink = CALayer()
    private let outline = CALayer()
    private var inkDrawn: (Float, Float, Float, Int, CGFloat)?
    private var outlineDrawn: (Int32, Float, Int, CGFloat)?
#if DEBUG
    private let pink = CALayer(), violet = CALayer()
#endif

    init(role: Role) {
        self.role = role
        super.init(frame: .zero)
        isUserInteractionEnabled = false
        isAccessibilityElement = false
        backgroundColor = .clear
        for l in [wash, ink, outline] { l.actions = Self.still; layer.addSublayer(l) }
        ink.contentsGravity = .resize
        outline.contentsGravity = .resize
#if DEBUG
        for (l, c) in [(pink, UIColor(MotionRuler.Ink.pink.color).cgColor), (violet, UIColor(MotionRuler.Ink.violet.color).cgColor)] {
            l.actions = Self.still
            l.backgroundColor = c
            l.isHidden = true
            layer.addSublayer(l)
        }
#endif
    }
    required init?(coder: NSCoder) { fatalError() }

    /// No implicit animation on any of these: the clock is the only motion.
    private static let still: [String: CAAction] = [
        "position": NSNull(), "bounds": NSNull(), "frame": NSNull(), "contents": NSNull(),
        "backgroundColor": NSNull(), "opacity": NSNull(), "hidden": NSNull(),
    ]

    func bind(_ c: UtttMotionClock, side: CGFloat, key: Int) {
        if clock !== c {
            if let t = token { clock?.unobserve(t) }
            clock = c
            token = c.observe { [weak self] in self?.apply() }
        }
        let changed = side != self.side || key != self.key
        self.side = side
        self.key = key
        if changed { inkDrawn = nil; outlineDrawn = nil }
        apply()
    }

    deinit {
        guard let t = token, let c = clock else { return }
        Task { @MainActor in c.unobserve(t) }
    }

    private func apply() {
        guard let f = clock?.frame, side > 0 else { return }
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        switch role {
        case .wash:
            if f.wash.2 > 0 {
                wash.isHidden = false
                wash.frame = UtttLiveBoard.rect(f.wash, side)
                wash.backgroundColor = Self.cg(f.wash_rgba)
            } else {
                wash.isHidden = true
            }
#if DEBUG
            pink.isHidden = !(UtttRuler.on && f.wash.2 > 0)
            if !pink.isHidden {
                let w = UtttLiveBoard.rect(f.wash, side), d = MotionRuler.side
                pink.frame = CGRect(x: w.midX - d / 2, y: w.midY - d / 2, width: d, height: d)
            }
#endif
        case .strokes:
            let want = (f.mark_t, f.fall_t, f.line_t, key, side)
            if inkDrawn.map({ $0 != want }) ?? true {
                inkDrawn = want
                let polys = Uttt.lastStroke(t: f.mark_t) + Uttt.settleStroke(fall: f.fall_t, line: f.line_t)
                Self.paint(polys, into: ink, side: side)
#if DEBUG
                violet.isHidden = !UtttRuler.on
                if UtttRuler.on {
                    let pts = Uttt.lastStroke(t: f.mark_t).flatMap(\.points)
                    if let x0 = pts.map(\.x).min(), let x1 = pts.map(\.x).max(),
                       let y0 = pts.map(\.y).min(), let y1 = pts.map(\.y).max() {
                        let d = MotionRuler.side
                        violet.frame = CGRect(x: (x0 + x1) / 2 * side - d / 2,
                                              y: (y0 + y1) / 2 * side - d / 2, width: d, height: d)
                    } else { violet.isHidden = true }
                }
#endif
            }
            if f.outline >= 0 {
                let want = (f.outline, f.outline_t, key, side)
                if outlineDrawn.map({ $0 != want }) ?? true {
                    outlineDrawn = want
                    Self.paint(Uttt.outlineStroke(block: f.outline, t: f.outline_t), into: outline, side: side)
                }
                outline.isHidden = false
                outline.opacity = f.outline_a
            } else {
                outline.isHidden = true
                outlineDrawn = nil
            }
        }
        CATransaction.commit()
    }

    private static func cg(_ c: UInt32) -> CGColor {
        CGColor(srgbRed: CGFloat((c >> 24) & 0xff) / 255, green: CGFloat((c >> 16) & 0xff) / 255,
                blue: CGFloat((c >> 8) & 0xff) / 255, alpha: CGFloat(c & 0xff) / 255)
    }

    /// `polys` (the board's 0..1 square) into a bitmap of just their bounds,
    /// at the screen's scale, as `layer`'s contents - placed in board points.
    private static func paint(_ polys: [Uttt.Poly], into layer: CALayer, side: CGFloat) {
        var lo = CGPoint(x: CGFloat.infinity, y: CGFloat.infinity)
        var hi = CGPoint(x: -CGFloat.infinity, y: -CGFloat.infinity)
        for p in polys { for q in p.points {
            lo.x = min(lo.x, q.x); lo.y = min(lo.y, q.y)
            hi.x = max(hi.x, q.x); hi.y = max(hi.y, q.y)
        } }
        guard lo.x.isFinite, hi.x > lo.x || hi.y > lo.y else {
            layer.contents = nil
            return
        }
        let r = CGRect(x: lo.x * side - 1, y: lo.y * side - 1,
                       width: (hi.x - lo.x) * side + 2, height: (hi.y - lo.y) * side + 2)
        let scale = UIScreen.main.scale
        let pw = Int((r.width * scale).rounded(.up)), ph = Int((r.height * scale).rounded(.up))
        guard pw > 0, ph > 0,
              let cg = CGContext(data: nil, width: pw, height: ph, bitsPerComponent: 8, bytesPerRow: 0,
                                 space: CGColorSpaceCreateDeviceRGB(),
                                 bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue
                                     | CGBitmapInfo.byteOrder32Little.rawValue)
        else { return }
        /* top-left origin, board points, as the kernel's coordinates mean */
        cg.translateBy(x: 0, y: CGFloat(ph))
        cg.scaleBy(x: scale, y: -scale)
        cg.translateBy(x: -r.minX, y: -r.minY)
        for p in polys {
            guard let head = p.points.first else { continue }
            cg.setFillColor(p.color)
            cg.beginPath()
            cg.move(to: CGPoint(x: head.x * side, y: head.y * side))
            for q in p.points.dropFirst() { cg.addLine(to: CGPoint(x: q.x * side, y: q.y * side)) }
            cg.closePath()
            cg.fillPath()
        }
        layer.contents = cg.makeImage()
        layer.frame = r
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
