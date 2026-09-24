// MotionRuler.swift - the generic half of a debug ruler for filming motion.
//
// A filmed transition is scored per frame: where was every moving element in
// this frame, and did it ride the container it lives in. Pixels of the real UI
// cannot answer that - paper and ink look the same everywhere - so a DEBUG run
// with the ruler on paints unambiguous markers the rig's trackers read back
// (shared/rig/lib/squares.py, bars.py):
//
//   - a RED bar on the container's top edge and a GREEN bar on its bottom,
//   - a banded strip counted from the red bar (10pt bands, every tenth yellow),
//     so a snapshot scaled on its way to the screen reads as a wrong pitch,
//   - a 12pt fully saturated SQUARE on every element that moves or resizes.
//
// WHY SATURATED PRIMARIES, AS LITERAL sRGB: see shared/c/motion_ruler/
// motion_ruler.h, which owns every colour and size here (the CMotionRuler
// module) and which the finder (shared/tools/motion) reads too, so what is
// drawn and what is looked for cannot drift apart.
//
// WHAT A PRODUCT SUPPLIES: the App Group its dev files live in, and where the
// squares go. Everything here is product-free. DEBUG only - the release branch
// compiles to no-ops so call sites stay unconditional.
//
// UIKIT AND CORE ANIMATION ONLY: the bars, the strip and the squares are
// plain layers, the clock a row of layers a display link recolours, so a
// product with no SwiftUI in its process can carry the ruler.

import UIKit
#if DEBUG
import CMotionRuler
#endif

#if DEBUG

public enum MotionRuler {
    /// A dev flag is a FILE in the App Group, read fresh every time. A file and
    /// not a UserDefaults key: a `defaults write` from outside the sandbox lands
    /// in the wrong domain and cfprefsd caches App Group preferences.
    public static func flag(_ name: String, group: String) -> Bool {
        guard let dir = container(group) else { return false }
        return FileManager.default.fileExists(atPath: dir.appendingPathComponent(name).path)
    }

    /// THE GROUP'S DIRECTORY, looked up once per process. The FILE in it is
    /// still read fresh every time; only where the directory is gets cached.
    /// `containerURL(forSecurityApplicationGroupIdentifier:)` takes dyld's
    /// loader lock and an XPC round trip, and a `sample` of an opening drawer
    /// (TESTFLIGHT_PLAN.md 12) put 30 main-thread samples (~40 ms) in it under
    /// ONE view body that asked whether the ruler was on. The directory of an
    /// App Group never moves while the process lives.
    public static func container(_ group: String) -> URL? {
        lock.lock(); defer { lock.unlock() }
        if let u = containers[group] { return u }
        let u = FileManager.default.containerURL(forSecurityApplicationGroupIdentifier: group)
        containers[group] = u
        return u
    }
    nonisolated(unsafe) private static var containers: [String: URL] = [:]
    private static let lock = NSLock()

    public static let band = CGFloat(MR_BAND_PT)
    public static let strip = CGFloat(MR_STRIP_PT)
    public static let edge = CGFloat(MR_EDGE_PT)
    public static let side = CGFloat(MR_SIDE_PT)

    public static func pure(_ r: Double, _ g: Double, _ b: Double) -> CGColor {
        CGColor(srgbRed: r, green: g, blue: b, alpha: 1)
    }

    /// An ink of the palette, by its C index (MR_INK_*).
    static func ink(_ i: Int) -> CGColor {
        let k = Int32(i)
        return pure(mr_ink_unit(k, 0), mr_ink_unit(k, 1), mr_ink_unit(k, 2))
    }

    /// The square palette. Red and green are the edge bars' and never a square.
    /// Squares of one colour are told apart by position by the reader.
    public enum Ink: CaseIterable {
        case magenta, cyan, yellow, orange, blue, violet, lime, pink
        var index: Int {
            switch self {
            case .magenta: return MR_INK_MAGENTA
            case .cyan:    return MR_INK_CYAN
            case .yellow:  return MR_INK_YELLOW
            case .orange:  return MR_INK_ORANGE
            case .blue:    return MR_INK_BLUE
            case .violet:  return MR_INK_VIOLET
            case .lime:    return MR_INK_LIME
            case .pink:    return MR_INK_PINK
            }
        }
        public var color: CGColor { MotionRuler.ink(index) }
    }

    /// The clock strip's left edge, past the banded strip.
    public static let clockGap = CGFloat(MR_CLOCK_GAP_PT)

    /// The value the clock strip shows now: milliseconds modulo 16384. A log
    /// line that carries it can be matched to the filmed frame showing it.
    public static var clockMs: Int {
        Int((Date().timeIntervalSince1970 * 1000).rounded()) & ((1 << MotionRulerClock.bits) - 1)
    }

    static func bandColour(_ i: Int) -> CGColor { ink(Int(mr_band_ink(Int32(i)))) }

    /// No implicit animation on any instrument layer: a bar tweened by the
    /// host's animation measures the tween, not the box.
    static let still: [String: CAAction] = [
        "position": NSNull(), "bounds": NSNull(), "frame": NSNull(),
        "backgroundColor": NSNull(), "hidden": NSNull(), "opacity": NSNull(),
    ]

    /// A 12pt square of `ink`, a layer of its own: the caller places it
    /// (`place`) where the element's alignment point is.
    public static func square(_ ink: Ink) -> CALayer {
        let l = CALayer()
        l.actions = still
        l.backgroundColor = ink.color
        l.zPosition = 1_000
        l.bounds = CGRect(x: 0, y: 0, width: side, height: side)
        return l
    }

    /// Where a square sits on a box: its centre (`at` .5,.5) or inside a
    /// corner (`at` 0 or 1 on an axis puts its edge on the box's).
    public static func place(_ l: CALayer, in r: CGRect, at u: CGPoint = CGPoint(x: 0.5, y: 0.5)) {
        l.frame = CGRect(x: r.minX + (r.width - side) * u.x, y: r.minY + (r.height - side) * u.y,
                         width: side, height: side)
    }
}

/// The edge bars and the banded strip, filling whatever box it is laid on.
/// Attach it to the container that RESIZES, so it measures that box.
public final class MotionRulerEdges: UIView {
    /// The red top bar with the band strip and the clock, and the green
    /// bottom bar. Both by default; a product whose top and bottom ride
    /// different layers through a collapse draws each half on its own.
    private let top: Bool
    private let bottom: Bool
    private let red = CALayer(), green = CALayer()
    private var bands: [CALayer] = []
    private let clock = MotionRulerClock()

    public init(top: Bool = true, bottom: Bool = true) {
        self.top = top
        self.bottom = bottom
        super.init(frame: .zero)
        isUserInteractionEnabled = false
        isAccessibilityElement = false
        accessibilityElementsHidden = true
        clipsToBounds = true
        layer.actions = MotionRuler.still
        red.actions = MotionRuler.still
        green.actions = MotionRuler.still
        red.backgroundColor = MotionRuler.ink(MR_INK_RED)
        green.backgroundColor = MotionRuler.ink(MR_INK_GREEN)
        if top { layer.addSublayer(red); addSubview(clock) }
        if bottom { layer.addSublayer(green) }
    }
    required init?(coder: NSCoder) { fatalError() }

    public override func layoutSubviews() {
        super.layoutSubviews()
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        let w = bounds.width, h = bounds.height
        if top {
            let n = max(1, Int((h / MotionRuler.band).rounded(.up)))
            while bands.count < n {
                let b = CALayer()
                b.actions = MotionRuler.still
                b.backgroundColor = MotionRuler.bandColour(bands.count)
                layer.insertSublayer(b, below: red)
                bands.append(b)
            }
            for (i, b) in bands.enumerated() {
                b.isHidden = i >= n
                b.frame = CGRect(x: 0, y: CGFloat(i) * MotionRuler.band,
                                 width: MotionRuler.strip, height: MotionRuler.band)
            }
            red.frame = CGRect(x: 0, y: 0, width: w, height: MotionRuler.edge)
            clock.frame = CGRect(x: MotionRuler.strip + MotionRuler.clockGap, y: MotionRuler.edge,
                                 width: MotionRulerClock.cell * CGFloat(MotionRulerClock.bits),
                                 height: MotionRulerClock.cell)
        }
        if bottom {
            green.frame = CGRect(x: 0, y: h - MotionRuler.edge, width: w, height: MotionRuler.edge)
        }
        CATransaction.commit()
    }
}

/// A per-frame CLOCK a parser reads off a filmed frame without OCR: 14 cells,
/// most significant first, white 1 and black 0, milliseconds modulo 16384.
/// A display link recolours the cells every display refresh, so a filmed
/// frame whose clock repeats while geometry moved is a frame the app did not
/// render - the host composited a stale picture of it.
public final class MotionRulerClock: UIView {
    public static let bits = Int(MR_CLOCK_BITS)
    public static let cell = CGFloat(MR_CLOCK_CELL_PT)
    private var cells: [CALayer] = []
    private var link: CADisplayLink?

    public init() {
        super.init(frame: .zero)
        isUserInteractionEnabled = false
        for i in 0..<Self.bits {
            let c = CALayer()
            c.actions = MotionRuler.still
            c.frame = CGRect(x: CGFloat(i) * Self.cell, y: 0, width: Self.cell, height: Self.cell)
            layer.addSublayer(c)
            cells.append(c)
        }
        tick()
    }
    required init?(coder: NSCoder) { fatalError() }

    public override func didMoveToWindow() {
        super.didMoveToWindow()
        link?.invalidate()
        link = nil
        guard window != nil else { return }
        let l = CADisplayLink(target: Tick(self), selector: #selector(Tick.fire(_:)))
        l.add(to: .main, forMode: .common)
        link = l
    }

    fileprivate func tick() {
        let ms = MotionRuler.clockMs
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        for (i, c) in cells.enumerated() {
            let on = (ms >> (Self.bits - 1 - i)) & 1 == 1
            c.backgroundColor = on ? MotionRuler.pure(1, 1, 1) : MotionRuler.pure(0, 0, 0)
        }
        CATransaction.commit()
    }

    private final class Tick: NSObject {
        weak var clock: MotionRulerClock?
        init(_ c: MotionRulerClock) { clock = c }
        @objc func fire(_ l: CADisplayLink) {
            guard let clock else { l.invalidate(); return }
            MainActor.assumeIsolated { clock.tick() }
        }
    }
}

#else

public enum MotionRuler {
    public static func flag(_ name: String, group: String) -> Bool { false }
    public enum Ink: CaseIterable { case magenta, cyan, yellow, orange, blue, violet, lime, pink }
}

#endif
