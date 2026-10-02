#if UTTT_BIG_BOARD
import CoreGraphics
import QuartzCore
import UIKit

/// THE 243 x 243 BOARD (docs/BIG_BOARD.md): recursive Ultimate Tic-Tac-Toe at
/// depth 5, 59,049 cells, shown in a scroll view that zooms from the whole
/// board (a cell under two points: texture) to a cell of about 36 points.
///
/// IT TAKES PLAIN DATA and asks nothing: the cells, the node statuses, the
/// forced region, the last move and this device's staged draft are handed in
/// as numbers, so the view depends on neither the kernel nor its binding.
///
/// MEMORY IS BOUNDED BY THE SCREEN, NOT THE BOARD. The board at the deepest
/// zoom is 8,748 points square - a backing store of it would be hundreds of
/// megabytes in a process Messages kills at about 120. So the zooming view
/// draws nothing: it is an empty, transparent view the scroll view scales and
/// moves, and the board is painted by a canvas no bigger than the screen,
/// on top of it, which paints only the part of the board that is visible,
/// every time the visible part moves - one backing store, at most the view's
/// size at the screen's scale, at every zoom.
///
/// A CATiledLayer was tried first and MEASURED OUT: it keeps the tiles of
/// every detail level a zoom passes through until it is done, and a double
/// tap from 3x to 9x peaked at 167 MB of footprint in the preview harness
/// (183 MB in another run) against 32 MB at rest - over the extension's limit
/// in the middle of a gesture.
///
/// CRISP AT EVERY ZOOM: the cells are rectangles at the screen's resolution
/// with antialiasing off, and while a cell is under `insetMinSide` points
/// they are one 243 x 243 picture drawn with interpolation NONE - each cell a
/// block of whole pixels, never a blur.
///
/// THE GEOMETRY, stated once (`rect(level:prefix:)`, `cell(at:)`), and the
/// kernel's utb_node_rect / utb_cell_rect / utb_hit compute the same thing:
/// a level-L node with prefix p has base-9 digits d1..dL (d1 most
/// significant); digit d is column d % 3, row d / 3 of its 3 x 3; reading the
/// column digits and the row digits as base-3 numbers gives col and row in
/// 0..3^L-1, and the rect is (col / 3^L, row / 3^L, 1 / 3^L, 1 / 3^L) in the
/// board's 0..1 square. A leaf is level 5 with prefix = its index, so
/// index = d1*6561 + d2*729 + d3*81 + d4*9 + d5. A point maps back to the
/// cell whose rect holds it; the far edge (exactly 1) belongs to the last
/// cell, outside 0..1 is -1.
public final class UtttBigBoardView: UIView, UIScrollViewDelegate {
    /// Cells on a side, levels below the root, and the array sizes.
    nonisolated public static let sideCells = 243
    nonisolated public static let depth = 5
    nonisolated public static let leafCount = 59_049
    nonisolated public static let nodeCount = 7_381

    // MARK: the data, all plain

    /// 59,049 cells, leaf order: 0 empty, 1 X, 2 O. Anything else draws as
    /// empty; an array of the wrong length draws no marks.
    public var cells: [UInt8] = [] { didSet { publish() } }
    /// 7,381 node statuses, level-major (id 0 the root; level L prefix p at
    /// (9^L - 1) / 8 + p): 0 open, 1 X, 2 O, 3 drawn.
    public var nodes: [UInt8] = [] { didSet { publish() } }
    /// Where the next mark must go, in the board's 0..1 square; nil for none.
    public var regionRect: CGRect? { didSet { publish() } }
    /// The last move's cell, or -1.
    public var last: Int = -1 { didSet { publish() } }
    /// This device's staged, unsent move, or -1: drawn dashed.
    public var draft: Int = -1 { didSet { publish() } }
    /// A tap on a cell, with its index.
    public var onTap: ((Int) -> Void)?

    /// Everything at once: one snapshot, one invalidation.
    public func set(cells: [UInt8], nodes: [UInt8], regionRect: CGRect?, last: Int, draft: Int) {
        batching = true
        self.cells = cells; self.nodes = nodes; self.regionRect = regionRect
        self.last = last; self.draft = draft
        batching = false
        publish()
    }

    // MARK: zoom

    /// 1 is the whole board fitted to the view; the deepest zoom makes a cell
    /// `deepCellPoints` points.
    public var zoomScale: CGFloat { scroll.zoomScale }
    public static let deepCellPoints: CGFloat = 36

    public func setZoom(_ scale: CGFloat, animated: Bool) {
        let z = min(max(scale, scroll.minimumZoomScale), scroll.maximumZoomScale)
        scroll.setZoomScale(z, animated: animated)
    }

    /// Zoom so `rect` (the board's 0..1 square) fills about a third of the
    /// view, and centre it.
    public func focus(on rect: CGRect, animated: Bool) {
        guard side > 0, rect.width > 0, rect.height > 0 else { return }
        let view = min(bounds.width, bounds.height)
        let want = view / (3 * max(rect.width, rect.height) * side)
        zoom(to: CGPoint(x: rect.midX * side, y: rect.midY * side), scale: want, animated: animated)
    }

    // MARK: the geometry, once

    /// A level-L node's rect (L 0..5; level 5 is a cell, prefix its index) in
    /// the board's 0..1 square; .zero off the tree.
    nonisolated public static func rect(level: Int, prefix: Int) -> CGRect {
        guard (0...depth).contains(level), prefix >= 0, prefix < pow9[level] else { return .zero }
        let (col, row) = grid(level: level, prefix: prefix)
        let n = CGFloat(pow3[level])
        return CGRect(x: CGFloat(col) / n, y: CGFloat(row) / n, width: 1 / n, height: 1 / n)
    }

    /// The cell under a point of the board's 0..1 square, or -1 outside it.
    nonisolated public static func cell(at p: CGPoint) -> Int {
        guard p.x >= 0, p.x <= 1, p.y >= 0, p.y <= 1 else { return -1 }
        return prefix(level: depth, col: axis(p.x), row: axis(p.y))
    }

    /// A node id's level and prefix; nil off the tree.
    nonisolated public static func node(id: Int) -> (level: Int, prefix: Int)? {
        guard id >= 0, id < nodeCount else { return nil }
        var level = 0
        while level + 1 < depth && id >= nodeOffset[level + 1] { level += 1 }
        return (level, id - nodeOffset[level])
    }

    nonisolated static let pow3: [Int] = [1, 3, 9, 27, 81, 243]
    nonisolated static let pow9: [Int] = [1, 9, 81, 729, 6561, 59_049]
    /// (9^L - 1) / 8: the first id at level L.
    nonisolated static let nodeOffset: [Int] = [0, 1, 10, 91, 820]

    /// A level-L prefix's column and row in its 3^L grid.
    nonisolated static func grid(level: Int, prefix p: Int) -> (Int, Int) {
        var col = 0, row = 0
        guard level > 0 else { return (0, 0) }
        for l in 1...level {
            let d = (p / pow9[level - l]) % 9
            col = col * 3 + d % 3
            row = row * 3 + d / 3
        }
        return (col, row)
    }

    /// The prefix at `level` of the grid square (col, row): the base-3 digits
    /// of col and row read as the column and row of each base-9 digit.
    nonisolated static func prefix(level: Int, col: Int, row: Int) -> Int {
        var p = 0
        for l in stride(from: level - 1, through: 0, by: -1) {
            let s = pow3[l]
            p = p * 9 + ((row / s) % 3) * 3 + (col / s) % 3
        }
        return p
    }

    /// One axis to a cell column: the grid line k / 243 at or before u,
    /// nudged where u * 243 rounds across a boundary, as utb_hit does.
    nonisolated static func axis(_ u: CGFloat) -> Int {
        let n = sideCells
        var k = min(max(Int(u * CGFloat(n)), 0), n - 1)
        while k > 0 && u < CGFloat(k) / CGFloat(n) { k -= 1 }
        while k < n - 1 && u >= CGFloat(k + 1) / CGFloat(n) { k += 1 }
        return k
    }

    // MARK: the views

    private let scroll = UIScrollView()
    /// What the scroll view zooms and pans: the board's square at zoom 1,
    /// empty and transparent, so it has no backing store at any zoom. Taps
    /// land on it, in board points.
    private let content = UIView()
    /// Where the board is painted: the visible part of it, on top.
    private let canvas = UtttBigCanvas()
    /// The board's side in points at zoom 1 (the fitted board).
    private var side: CGFloat = 0
    private var batching = false
    /// Follows a zoom animation frame by frame: UIScrollView reports the end
    /// of an animated zoom, not its frames.
    private var link: CADisplayLink?
    private var still = 0

    public override init(frame: CGRect) {
        super.init(frame: frame)
        backgroundColor = UtttPaper.flat
        scroll.delegate = self
        scroll.bounces = true
        scroll.bouncesZoom = true
        scroll.showsVerticalScrollIndicator = false
        scroll.showsHorizontalScrollIndicator = false
        scroll.contentInsetAdjustmentBehavior = .never
        scroll.backgroundColor = UtttPaper.flat
        addSubview(scroll)
        content.backgroundColor = nil
        content.isOpaque = false
        scroll.addSubview(content)
        canvas.isUserInteractionEnabled = false
        addSubview(canvas)

        let double = UITapGestureRecognizer(target: self, action: #selector(doubleTapped(_:)))
        double.numberOfTapsRequired = 2
        content.addGestureRecognizer(double)
        let single = UITapGestureRecognizer(target: self, action: #selector(tapped(_:)))
        single.require(toFail: double)
        content.addGestureRecognizer(single)

        /* ONE ELEMENT: 59,049 squares for VoiceOver would be absurd. Direct
         * interaction hands VoiceOver users the pinch and the taps. */
        isAccessibilityElement = true
        accessibilityLabel = "243 by 243 board"
        accessibilityTraits = .allowsDirectInteraction
        /* a new screen scale means new pixel sizes for every line */
        registerForTraitChanges([UITraitDisplayScale.self]) { (self: Self, _: UITraitCollection) in self.publish() }
    }
    required init?(coder: NSCoder) { fatalError() }

    /// THE ZOOM SURVIVES A SIZE CHANGE. The board is fitted on the first
    /// layout only; when the bounds change after that (the drawer going from
    /// expanded to compact and back) the zoom scale and the board point under
    /// the view's centre are kept, clamped to the new limits.
    public override func layoutSubviews() {
        super.layoutSubviews()
        let s = floor(min(bounds.width, bounds.height))
        let resized = s > 0 && s != side
        /* read before the scroll view takes the new frame: the board point
         * under the OLD visible centre, as a fraction of the board */
        let keepZoom = scroll.zoomScale
        let mid = scroll.convert(CGPoint(x: scroll.bounds.midX, y: scroll.bounds.midY), to: content)
        let keepCentre = side > 0 ? CGPoint(x: mid.x / side, y: mid.y / side) : CGPoint(x: 0.5, y: 0.5)
        let first = side == 0
        scroll.frame = bounds
        if resized {
            side = s
            scroll.minimumZoomScale = 1
            scroll.maximumZoomScale = 1
            scroll.zoomScale = 1
            content.frame = CGRect(x: 0, y: 0, width: s, height: s)
            scroll.contentSize = content.frame.size
            scroll.maximumZoomScale = max(1, Self.deepCellPoints * CGFloat(Self.sideCells) / s)
            if !first {
                scroll.zoomScale = min(max(keepZoom, scroll.minimumZoomScale), scroll.maximumZoomScale)
                centre()
                place(centre: keepCentre)
            }
            publish()
        }
        centre()
        follow()
    }

    /// Scroll so the board point `c` (a fraction of the board) is under the
    /// view's centre, as near as the content allows.
    private func place(centre c: CGPoint) {
        let z = scroll.zoomScale, inset = scroll.contentInset
        let size = scroll.bounds.size, content = scroll.contentSize
        func clamp(_ v: CGFloat, _ lo: CGFloat, _ hi: CGFloat) -> CGFloat { min(max(v, lo), max(lo, hi)) }
        scroll.contentOffset = CGPoint(
            x: clamp(c.x * side * z - size.width / 2, -inset.left, content.width - size.width + inset.right),
            y: clamp(c.y * side * z - size.height / 2, -inset.top, content.height - size.height + inset.bottom))
    }

    /// The board centred while it is smaller than the view.
    private func centre() {
        let w = content.frame.width, h = content.frame.height
        let x = max(0, (bounds.width - w) / 2), y = max(0, (bounds.height - h) / 2)
        let inset = UIEdgeInsets(top: y, left: x, bottom: y, right: x)
        if scroll.contentInset != inset { scroll.contentInset = inset }
    }

    public func viewForZooming(in scrollView: UIScrollView) -> UIView? { content }
    public func scrollViewDidZoom(_ scrollView: UIScrollView) { centre(); follow() }
    public func scrollViewDidScroll(_ scrollView: UIScrollView) { follow() }
    public func scrollViewDidEndZooming(_ scrollView: UIScrollView, with view: UIView?, atScale scale: CGFloat) {
        follow()
    }

    /// The canvas onto the board where it is NOW - the presentation layers,
    /// so an animated zoom is followed frame by frame - and the display link
    /// kept running while anything is still moving.
    private func follow() {
        guard side > 0 else { return }
        let f = content.layer.presentation()?.frame ?? content.frame
        let o = scroll.layer.presentation()?.bounds.origin ?? scroll.bounds.origin
        let board = CGRect(x: f.minX - o.x + scroll.frame.minX, y: f.minY - o.y + scroll.frame.minY,
                           width: f.width, height: f.height)
        if canvas.show(board: board, in: bounds) { still = 0 }
        let moving = content.layer.animationKeys()?.isEmpty == false
            || scroll.layer.animationKeys()?.isEmpty == false
        if moving || still < 2 {
            if link == nil {
                let l = CADisplayLink(target: self, selector: #selector(tick))
                l.add(to: .main, forMode: .common)
                link = l
            }
        }
    }

    @objc private func tick() {
        still += 1
        follow()
        let moving = content.layer.animationKeys()?.isEmpty == false
            || scroll.layer.animationKeys()?.isEmpty == false
        if !moving && still >= 2 {
            link?.invalidate()
            link = nil
        }
    }

    private func zoom(to p: CGPoint, scale: CGFloat, animated: Bool) {
        let z = min(max(scale, scroll.minimumZoomScale), scroll.maximumZoomScale)
        let w = scroll.bounds.width / z, h = scroll.bounds.height / z
        scroll.zoom(to: CGRect(x: p.x - w / 2, y: p.y - h / 2, width: w, height: h), animated: animated)
        follow()
    }

    /// In by three on the tapped point; back out to the whole board from the
    /// deepest zoom. Every zoom is reachable without a pinch.
    @objc private func doubleTapped(_ g: UITapGestureRecognizer) {
        if scroll.zoomScale >= scroll.maximumZoomScale * 0.99 {
            scroll.setZoomScale(scroll.minimumZoomScale, animated: true)
            follow()
        } else {
            zoom(to: g.location(in: content), scale: scroll.zoomScale * 3, animated: true)
        }
    }

    @objc private func tapped(_ g: UITapGestureRecognizer) {
        guard side > 0 else { return }
        let p = g.location(in: content)
        let mv = Self.cell(at: CGPoint(x: p.x / side, y: p.y / side))
        if mv >= 0 { onTap?(mv) }
    }

    // MARK: accessibility value: what a UI test reads

    /// "zoom Z offset X,Y whole W centre C": the zoom (1 = fitted), the
    /// visible top left in points, whether the whole board is on screen, and
    /// the cell under the view's centre.
    public override var accessibilityValue: String? {
        get {
            let off = CGPoint(x: scroll.contentOffset.x + scroll.contentInset.left,
                              y: scroll.contentOffset.y + scroll.contentInset.top)
            let seen = convert(bounds, to: content)
            let board = content.bounds.insetBy(dx: 0.5, dy: 0.5)
            let whole = side > 0 && seen.contains(board)
            let mid = convert(CGPoint(x: bounds.midX, y: bounds.midY), to: content)
            let c = side > 0 ? Self.cell(at: CGPoint(x: mid.x / side, y: mid.y / side)) : -1
            return String(format: "zoom %.2f offset %.0f,%.0f whole %d centre %d",
                          scroll.zoomScale, off.x, off.y, whole ? 1 : 0, c)
        }
        set { _ = newValue }
    }

    // MARK: the snapshot

    /// The data as the canvas paints it. The arrays share the view's buffers
    /// (copy on write): the cells are held once, not twice.
    private func publish() {
        guard !batching else { return }
        var s = UtttBigSnap()
        s.cells = cells.count == Self.leafCount ? cells : []
        s.nodes = nodes.count == Self.nodeCount ? nodes : []
        s.region = regionRect
        s.last = last
        s.draft = draft
        s.side = side
        s.screenScale = traitCollection.displayScale > 0 ? traitCollection.displayScale : 2
        canvas.update(s, cellsChanged: canvas.snap.cells != s.cells)
    }
}

/// What the canvas paints from.
struct UtttBigSnap {
    var cells: [UInt8] = []
    var nodes: [UInt8] = []
    var region: CGRect?
    var last = -1
    var draft = -1
    /// The board's side in points at zoom 1.
    var side: CGFloat = 0
    /// Device pixels per point.
    var screenScale: CGFloat = 2
}

/// The visible part of the board, painted at the screen's resolution. Its
/// frame is the board's on-screen rect clipped to the view, so its backing
/// store is never bigger than the view.
final class UtttBigCanvas: UIView {
    private(set) var snap = UtttBigSnap()
    /// The cells as a 243 x 243 picture - paper transparent, X and O opaque -
    /// for zooms where a cell is too small to inset: drawn with
    /// interpolation none, each cell a block of whole pixels.
    private var picture: CGImage?
    /// The whole board's rect in the owner's coordinates.
    private var board = CGRect.zero

    override init(frame: CGRect) {
        super.init(frame: frame)
        isOpaque = true
        backgroundColor = UtttPaper.flat
        contentMode = .redraw
        layer.actions = ["position": NSNull(), "bounds": NSNull(), "frame": NSNull(), "contents": NSNull()]
    }
    required init?(coder: NSCoder) { fatalError() }

    func update(_ s: UtttBigSnap, cellsChanged: Bool) {
        snap = s
        if cellsChanged || picture == nil { picture = UtttBigPainter.picture(s.cells) }
        setNeedsDisplay()
    }

    /// Place the canvas over `board` (the whole board's rect) clipped to
    /// `view`; true when that moved it, and then it repaints.
    @discardableResult
    func show(board b: CGRect, in view: CGRect) -> Bool {
        let seen = b.intersection(view)
        let f = seen.isNull ? .zero : seen
        guard b != board || f != frame else { return false }
        board = b
        frame = f
        setNeedsDisplay()
        return true
    }

    override func draw(_ rect: CGRect) {
        guard let ctx = UIGraphicsGetCurrentContext(), snap.side > 0, board.width > 0 else { return }
        let zoom = board.width / snap.side
        /* board points from here on: the board's top left is where it sits
         * relative to this canvas */
        ctx.translateBy(x: board.minX - frame.minX, y: board.minY - frame.minY)
        ctx.scaleBy(x: zoom, y: zoom)
        let visible = CGRect(x: (frame.minX - board.minX) / zoom, y: (frame.minY - board.minY) / zoom,
                             width: bounds.width / zoom, height: bounds.height / zoom)
        UtttBigPainter.paint(snap, picture: picture, in: ctx, rect: visible, pt: zoom)
    }
}

/// The painter: pure, from a snapshot into a context in board points.
enum UtttBigPainter {
    /// The inks of the 9 x 9's pen (uttt_draw.c INK_X, INK_O, INK_GR) and
    /// its wash (uttt_anim.c WASH_RGB), as plain values: the view asks the
    /// kernel nothing.
    static let inkX = CGColor(srgbRed: 0x25 / 255.0, green: 0x37 / 255.0, blue: 0x6b / 255.0, alpha: 1)
    static let inkO = CGColor(srgbRed: 0xa8 / 255.0, green: 0x32 / 255.0, blue: 0x1f / 255.0, alpha: 1)
    static let inkGrey = CGColor(srgbRed: 0x2f / 255.0, green: 0x2b / 255.0, blue: 0x26 / 255.0, alpha: 1)
    static let wash = CGColor(srgbRed: 0xd6 / 255.0, green: 0xa8 / 255.0, blue: 0x36 / 255.0, alpha: 0.35)
    /// The highlighter itself, at full strength: the forced target's outline.
    static let highlighter = CGColor(srgbRed: 0xd6 / 255.0, green: 0xa8 / 255.0, blue: 0x36 / 255.0, alpha: 1)
    static let paper = UtttPaper.flat.cgColor
    /// The grid's dark ink, UtttInk.ink (#1d1b16), at each level's alpha.
    static let gridInk = UtttInk.ink.cgColor

    /// THE GRID OF GRIDS: one row per level, level 1 (the top 3 x 3) to
    /// level 5 (the cells). `width` is in screen points (0 = a hairline,
    /// one device pixel) and is drawn as a strip of whole pixels; `alpha` is
    /// the line's ink at full strength. A level FADES IN WITH ITS PITCH ON
    /// SCREEN (the distance between its lines): alpha 0 while the pitch is
    /// under `from` points, full from `full` points, linear between.
    /// Fitted on a phone (a cell about 1.6 pt) the pitches are 134, 45, 15,
    /// 5 and 1.6 pt: levels 1-3 solid, level 4 faint, the cells absent. A
    /// 27 x 27 block across the screen (zoom 9) makes level 4 solid and the
    /// cells faint; at the deepest zoom (36 pt cells) all five are solid.
    /// The ramps are not one multiple of the width: a level-4 line at its
    /// full 0.8 pt every 5 pt is a fifth of the board in ink, which is the
    /// grey static this table exists to prevent, so level 4 waits until 16.
    struct GridLevel {
        let width: CGFloat
        let alpha: CGFloat
        let from: CGFloat
        let full: CGFloat
    }
    static let grid: [GridLevel] = [
        GridLevel(width: 0, alpha: 0, from: .infinity, full: .infinity),  // level 0: the board's edge, no line
        GridLevel(width: 3.0, alpha: 1.00, from: 8, full: 14),
        GridLevel(width: 2.0, alpha: 0.88, from: 6, full: 12),
        GridLevel(width: 1.2, alpha: 0.66, from: 4, full: 10),
        GridLevel(width: 0.8, alpha: 0.60, from: 3, full: 16),
        GridLevel(width: 0, alpha: 0.28, from: 8, full: 30),
    ]

    /// How strongly a level's lines show at `pitch` screen points: 0..1.
    static func fade(level: Int, pitch: CGFloat) -> CGFloat {
        let g = grid[level]
        guard pitch > g.from else { return 0 }
        return min(1, (pitch - g.from) / (g.full - g.from))
    }

    /// A DECIDED NODE, at the level it was decided: a tint of its rect in the
    /// winner's ink (grey for a draw) under the cells, so the won regions
    /// stand out of the board at the fit, and over it the winner's big mark
    /// (as the 9 x 9 board draws over a won block) once the node is
    /// `markMinSide` points on screen, its stroke 1 / `markStroke` of the
    /// side (never under a point), at `markAlpha`.
    static let tintAlpha: CGFloat = 0.26
    static let drawTintAlpha: CGFloat = 0.20
    static let markMinSide: CGFloat = 8
    static let markStroke: CGFloat = 14
    static let markAlpha: CGFloat = 0.9
    /// The forced target's outline: this wide, never smaller than
    /// `regionMinPoints` on screen (a level-4 block is 5 pt at the fit),
    /// and `anywhereWidth` round the whole board when the target is anywhere.
    static let regionWidth: CGFloat = 3
    static let regionMinPoints: CGFloat = 12
    static let anywhereWidth: CGFloat = 4
    /// A cell bigger than this on screen is inset; smaller ones fill solid.
    static let insetMinSide: CGFloat = 6
    /// The last move's and the draft's outline: never smaller than this on
    /// screen (at the whole board a cell is under two points), this wide.
    static let ringMinPoints: CGFloat = 10
    static let ringWidthPoints: CGFloat = 2

    static func ink(_ mark: UInt8) -> CGColor { mark == 2 ? inkO : inkX }

    /// A column or row of the 243 grid, its base-3 digits spread into base 9:
    /// a cell's index is spread[col] + 3 * spread[row].
    static let spread: [Int] = (0..<UtttBigBoardView.sideCells).map {
        UtttBigBoardView.prefix(level: UtttBigBoardView.depth, col: $0, row: 0)
    }

    /// The cells as a 243 x 243 RGBA picture, row 0 at the top: X and O in
    /// their inks, empty transparent so the paper and the wash show through.
    static func picture(_ cells: [UInt8]) -> CGImage? {
        let n = UtttBigBoardView.sideCells
        guard cells.count == UtttBigBoardView.leafCount else { return nil }
        var px = [UInt8](repeating: 0, count: n * n * 4)
        let x: [UInt8] = [0x25, 0x37, 0x6b, 0xff], o: [UInt8] = [0xa8, 0x32, 0x1f, 0xff]
        for row in 0..<n {
            let rowPart = spread[row] * 3
            for col in 0..<n {
                let v = cells[rowPart + spread[col]]
                guard v == 1 || v == 2 else { continue }
                let c = v == 1 ? x : o, at = (row * n + col) * 4
                px[at] = c[0]; px[at + 1] = c[1]; px[at + 2] = c[2]; px[at + 3] = c[3]
            }
        }
        guard let data = CGDataProvider(data: Data(px) as CFData),
              let space = CGColorSpace(name: CGColorSpace.sRGB) else { return nil }
        return CGImage(width: n, height: n, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: n * 4,
                       space: space, bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedLast.rawValue),
                       provider: data, decode: nil, shouldInterpolate: false, intent: .defaultIntent)
    }

    /// Paint the part `rect` of the board (board points) into `ctx`, whose
    /// transform maps board points to the screen at `pt` points per board
    /// point.
    static func paint(_ s: UtttBigSnap, picture: CGImage?, in ctx: CGContext, rect: CGRect, pt: CGFloat) {
        let side = s.side
        guard side > 0, pt > 0 else { return }
        typealias V = UtttBigBoardView
        let n = V.sideCells
        let cell = side / CGFloat(n)
        let px = pt * max(s.screenScale, 1)            // device pixels per board point
        let area = rect.intersection(CGRect(x: 0, y: 0, width: side, height: side))
        guard !area.isNull, !area.isEmpty else { return }
        ctx.setFillColor(paper)
        ctx.fill(area)

        func range(_ lo: CGFloat, _ hi: CGFloat, _ pitch: CGFloat, _ count: Int) -> ClosedRange<Int> {
            let a = max(0, Int(floor(lo / pitch))), b = min(count - 1, Int(ceil(hi / pitch)))
            return a...max(a, b)
        }

        /* the highlighter first: the marks sit on it, as ink on a wash. The
         * whole board (anywhere) gets no wash, only its outline below. */
        let region = s.region.map { CGRect(x: $0.minX * side, y: $0.minY * side,
                                           width: $0.width * side, height: $0.height * side) }
        let anywhere = s.region.map { $0.width >= 0.999 && $0.height >= 0.999 } ?? false
        if let r = region, !anywhere {
            ctx.setFillColor(wash)
            ctx.fill(r)
        }

        /* THE DECIDED NODES, each at the level it was decided: the topmost
         * decided node on every path (a node whose ancestors are all open).
         * Their tints go under the cells, so the marks stay full ink; their
         * big marks go over the grid. */
        var decided: [(rect: CGRect, status: UInt8)] = []
        if s.nodes.count == V.nodeCount {
            for level in 0..<V.depth {
                let pitch = side / CGFloat(V.pow3[level])
                let count = V.pow3[level]
                for row in range(area.minY, area.maxY, pitch, count) {
                    for col in range(area.minX, area.maxX, pitch, count) {
                        let p = V.prefix(level: level, col: col, row: row)
                        let st = s.nodes[V.nodeOffset[level] + p]
                        guard (1...3).contains(st), !ancestorDecided(s.nodes, level: level, prefix: p) else { continue }
                        let r = CGRect(x: CGFloat(col) * pitch, y: CGFloat(row) * pitch, width: pitch, height: pitch)
                        decided.append((r, st))
                    }
                }
            }
        }
        for d in decided {
            let ink = d.status == 3 ? inkGrey : ink(d.status)
            ctx.setFillColor(ink.copy(alpha: d.status == 3 ? drawTintAlpha : tintAlpha) ?? ink)
            ctx.fill(d.rect)
        }

        /* THE CELLS. Small: the picture, one pixel per cell, scaled with
         * interpolation none. Large: rectangles at the screen's resolution,
         * inset, antialiasing off so every edge is a whole pixel. */
        if s.cells.count == V.leafCount {
            if cell * pt <= insetMinSide {
                if let picture {
                    ctx.saveGState()
                    ctx.interpolationQuality = .none
                    /* CGContext.draw puts an image's first row at the bottom;
                     * flip around the board so row 0 is the top */
                    ctx.translateBy(x: 0, y: side)
                    ctx.scaleBy(x: 1, y: -1)
                    ctx.draw(picture, in: CGRect(x: 0, y: 0, width: side, height: side))
                    ctx.restoreGState()
                }
            } else {
                let inset = max(cell * 0.09, 0.5 / pt)
                var xs: [CGRect] = [], os: [CGRect] = []
                let cols = range(area.minX, area.maxX, cell, n), rows = range(area.minY, area.maxY, cell, n)
                s.cells.withUnsafeBufferPointer { cells in
                    for row in rows {
                        let rowPart = spread[row] * 3
                        for col in cols {
                            let v = cells[rowPart + spread[col]]
                            guard v == 1 || v == 2 else { continue }
                            let r = CGRect(x: CGFloat(col) * cell + inset, y: CGFloat(row) * cell + inset,
                                           width: cell - 2 * inset, height: cell - 2 * inset)
                            if v == 1 { xs.append(r) } else { os.append(r) }
                        }
                    }
                }
                ctx.setShouldAntialias(false)
                ctx.setFillColor(inkX); ctx.fill(xs)
                ctx.setFillColor(inkO); ctx.fill(os)
                ctx.setShouldAntialias(true)
            }
        }

        /* THE GRID OF GRIDS, a level at a time as its pitch opens on screen
         * (`grid`): every line a strip of whole pixels so it stays sharp, the
         * heavier levels drawn last so a coarse line is never under a finer
         * one; a line a coarser level draws is not drawn twice. */
        ctx.setShouldAntialias(false)
        for level in stride(from: V.depth, through: 1, by: -1) {
            let count = V.pow3[level]
            let pitch = side / CGFloat(count)
            let a = fade(level: level, pitch: pitch * pt)
            guard a > 0.01 else { continue }
            let g = grid[level]
            let w = max(1, (g.width * s.screenScale).rounded()) / px
            ctx.setFillColor(gridInk.copy(alpha: g.alpha * a) ?? gridInk)
            var strips: [CGRect] = []
            for k in range(area.minX - w, area.maxX + w, pitch, count + 1) where level == 1 || k % 3 != 0 {
                strips.append(CGRect(x: CGFloat(k) * pitch - w / 2, y: area.minY, width: w, height: area.height))
            }
            for k in range(area.minY - w, area.maxY + w, pitch, count + 1) where level == 1 || k % 3 != 0 {
                strips.append(CGRect(x: area.minX, y: CGFloat(k) * pitch - w / 2, width: area.width, height: w))
            }
            ctx.fill(strips)
        }
        ctx.setShouldAntialias(true)

        /* the big marks of the decided nodes big enough to read */
        for d in decided where d.rect.width * pt >= markMinSide {
            bigMark(d.status, in: d.rect, ctx: ctx, pt: pt)
        }

        /* THE FORCED TARGET'S OUTLINE, in the highlighter at full strength
         * and never smaller than `regionMinPoints`, so a 3 x 3 target reads
         * at the fit; anywhere is an outline round the whole board. */
        if let r = region {
            if anywhere {
                let w = anywhereWidth / pt
                ctx.setStrokeColor(highlighter)
                ctx.setLineWidth(w)
                ctx.stroke(r.insetBy(dx: w / 2, dy: w / 2))
            } else {
                let w = regionWidth / pt, least = regionMinPoints / pt
                var o = r
                if o.width < least { o = o.insetBy(dx: (o.width - least) / 2, dy: (o.height - least) / 2) }
                ctx.setStrokeColor(highlighter)
                ctx.setLineWidth(w)
                ctx.stroke(o)
            }
        }

        /* the last move, solid, and the staged draft, dashed */
        ring(s.last, s, ctx: ctx, pt: pt, dashed: false)
        ring(s.draft, s, ctx: ctx, pt: pt, dashed: true)
    }

    static func ancestorDecided(_ nodes: [UInt8], level: Int, prefix: Int) -> Bool {
        var l = level - 1, p = prefix / 9
        while l >= 0 {
            if nodes[UtttBigBoardView.nodeOffset[l] + p] != 0 { return true }
            l -= 1; p /= 9
        }
        return false
    }

    /// A decided node's big mark over its rect `r` (board points): a bold X
    /// (two strokes) or an O (a ring) in the winner's ink, or a grey dash
    /// for a draw; the stroke is 1 / `markStroke` of the side on screen,
    /// never under a point. The cells show between the strokes and through
    /// the tint, as on the 9 x 9 board.
    static func bigMark(_ status: UInt8, in r: CGRect, ctx: CGContext, pt: CGFloat) {
        let g = r.insetBy(dx: r.width * 0.18, dy: r.height * 0.18)
        let ink = status == 3 ? inkGrey : ink(status)
        ctx.setStrokeColor(ink.copy(alpha: markAlpha) ?? ink)
        ctx.setLineWidth(max(r.width / markStroke, 1 / pt))
        ctx.setLineCap(.round)
        switch status {
        case 1:
            ctx.strokeLineSegments(between: [CGPoint(x: g.minX, y: g.minY), CGPoint(x: g.maxX, y: g.maxY),
                                             CGPoint(x: g.maxX, y: g.minY), CGPoint(x: g.minX, y: g.maxY)])
        case 2:
            ctx.strokeEllipse(in: g)
        default:
            /* a draw: one short level dash across the middle */
            let h = g.insetBy(dx: g.width * 0.12, dy: 0)
            ctx.strokeLineSegments(between: [CGPoint(x: h.minX, y: h.midY), CGPoint(x: h.maxX, y: h.midY)])
        }
    }

    static func ring(_ mv: Int, _ s: UtttBigSnap, ctx: CGContext, pt: CGFloat, dashed: Bool) {
        guard mv >= 0, mv < UtttBigBoardView.leafCount else { return }
        let u = UtttBigBoardView.rect(level: UtttBigBoardView.depth, prefix: mv)
        var r = CGRect(x: u.minX * s.side, y: u.minY * s.side, width: u.width * s.side, height: u.height * s.side)
        let least = ringMinPoints / pt
        if r.width < least { r = r.insetBy(dx: (r.width - least) / 2, dy: (r.height - least) / 2) }
        let mark = s.cells.count == UtttBigBoardView.leafCount ? s.cells[mv] : 0
        let w = ringWidthPoints / pt
        ctx.setStrokeColor(mark == 1 || mark == 2 ? ink(mark) : inkGrey)
        ctx.setLineWidth(w)
        ctx.setLineDash(phase: 0, lengths: dashed ? [4 / pt, 3 / pt] : [])
        ctx.stroke(r.insetBy(dx: -w / 2, dy: -w / 2))
        ctx.setLineDash(phase: 0, lengths: [])
    }
}
#endif
