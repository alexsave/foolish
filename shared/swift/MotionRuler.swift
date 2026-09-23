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

import SwiftUI
#if DEBUG
import CMotionRuler
#endif

#if DEBUG

public enum MotionRuler {
    /// A dev flag is a FILE in the App Group, read fresh every time. A file and
    /// not a UserDefaults key: a `defaults write` from outside the sandbox lands
    /// in the wrong domain and cfprefsd caches App Group preferences.
    public static func flag(_ name: String, group: String) -> Bool {
        guard let dir = FileManager.default
                .containerURL(forSecurityApplicationGroupIdentifier: group)
        else { return false }
        return FileManager.default.fileExists(atPath: dir.appendingPathComponent(name).path)
    }

    public static let band = CGFloat(MR_BAND_PT)
    public static let strip = CGFloat(MR_STRIP_PT)
    public static let edge = CGFloat(MR_EDGE_PT)
    public static let side = CGFloat(MR_SIDE_PT)

    public static func pure(_ r: Double, _ g: Double, _ b: Double) -> Color {
        Color(.sRGB, red: r, green: g, blue: b, opacity: 1)
    }

    /// An ink of the palette, by its C index (MR_INK_*).
    static func ink(_ i: Int) -> Color {
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
        public var color: Color { MotionRuler.ink(index) }
    }

    /// The clock strip's left edge, past the banded strip.
    public static let clockGap = CGFloat(MR_CLOCK_GAP_PT)

    /// The value the clock strip shows now: milliseconds modulo 16384. A log
    /// line that carries it can be matched to the filmed frame showing it.
    public static var clockMs: Int {
        Int((Date().timeIntervalSince1970 * 1000).rounded()) & ((1 << MotionRulerClock.bits) - 1)
    }

    static func bandColour(_ i: Int) -> Color { ink(Int(mr_band_ink(Int32(i)))) }
}

/// The edge bars and the banded strip, filling whatever box it is laid on.
/// Attach it to the container that RESIZES, so it measures that box.
public struct MotionRulerEdges: View {
    let on: Bool
    /// The red top bar with the band strip and the clock, and the green
    /// bottom bar. Both by default; a product whose top and bottom ride
    /// different layers through a collapse draws each half on its own.
    let top: Bool
    let bottom: Bool
    public init(on: Bool, top: Bool = true, bottom: Bool = true) {
        self.on = on
        self.top = top
        self.bottom = bottom
    }

    public var body: some View {
        if on {
            GeometryReader { geo in
                let n = max(1, Int((geo.size.height / MotionRuler.band).rounded(.up)))
                ZStack(alignment: .topLeading) {
                    if top {
                        ForEach(0..<n, id: \.self) { i in
                            MotionRuler.bandColour(i)
                                .frame(width: MotionRuler.strip, height: MotionRuler.band)
                                .offset(y: CGFloat(i) * MotionRuler.band)
                        }
                        MotionRuler.ink(MR_INK_RED)
                            .frame(width: geo.size.width, height: MotionRuler.edge)
                        MotionRulerClock()
                            .offset(x: MotionRuler.strip + MotionRuler.clockGap,
                                    y: MotionRuler.edge)
                    }
                    if bottom {
                        MotionRuler.ink(MR_INK_GREEN)
                            .frame(width: geo.size.width, height: MotionRuler.edge)
                            .offset(y: geo.size.height - MotionRuler.edge)
                    }
                }
                .frame(width: geo.size.width, height: geo.size.height, alignment: .topLeading)
                .clipped()
                // THE INSTRUMENT IS NEVER ANIMATED: a bar tweened by the
                // host's animation measures the tween, not the box. Every
                // transaction, not just a height change's: nothing in here is
                // ever meant to tween, and the value-scoped form is iOS 17
                // while a product that compiles this file still targets 16.
                .transaction { $0.animation = nil }
            }
            .allowsHitTesting(false)
            .accessibilityHidden(true)
        }
    }
}

/// A per-frame CLOCK a parser reads off a filmed frame without OCR: 14 cells,
/// most significant first, white 1 and black 0, milliseconds modulo 16384.
/// `TimelineView(.animation)` re-evaluates on every display refresh, so a
/// filmed frame whose clock repeats while geometry moved is a frame the app did
/// not render - the host composited a stale picture of it.
public struct MotionRulerClock: View {
    public static let bits = Int(MR_CLOCK_BITS)
    public static let cell = CGFloat(MR_CLOCK_CELL_PT)
    public init() {}
    public var body: some View {
        TimelineView(.animation) { ctx in
            let ms = Int((ctx.date.timeIntervalSince1970 * 1000).rounded()) & ((1 << Self.bits) - 1)
            HStack(spacing: 0) {
                ForEach(0..<Self.bits, id: \.self) { i in
                    let on = (ms >> (Self.bits - 1 - i)) & 1 == 1
                    Rectangle()
                        .fill(on ? MotionRuler.pure(1, 1, 1) : MotionRuler.pure(0, 0, 0))
                        .frame(width: Self.cell, height: Self.cell)
                }
            }
        }
        .allowsHitTesting(false)
    }
}

public extension View {
    /// A square at this view's `alignment` point, above everything in it.
    /// An overlay and not a preference: a preference lands a layout pass late,
    /// which would measure the plumbing rather than the element.
    @ViewBuilder
    func motionSquare(_ ink: MotionRuler.Ink, on: Bool,
                      at alignment: Alignment = .center) -> some View {
        if on {
            overlay(alignment: alignment) {
                ink.color
                    .frame(width: MotionRuler.side, height: MotionRuler.side)
                    .allowsHitTesting(false)
                    .accessibilityHidden(true)
            }
            .zIndex(1_000)
        } else {
            self
        }
    }
}

#else

public enum MotionRuler {
    public static func flag(_ name: String, group: String) -> Bool { false }
    public enum Ink: CaseIterable { case magenta, cyan, yellow, orange, blue, violet, lime, pink }
}

public struct MotionRulerEdges: View {
    public init(on: Bool, top: Bool = true, bottom: Bool = true) {}
    public var body: some View { EmptyView() }
}

public extension View {
    func motionSquare(_ ink: MotionRuler.Ink, on: Bool,
                      at alignment: Alignment = .center) -> some View { self }
}

#endif
