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
// WHY SATURATED PRIMARIES, AS LITERAL sRGB. They survive h264 4:2:0 chroma
// subsampling, nothing on a paper or felt surface is that colour, and literal
// values do not shift between light and dark the way the system colours do.
//
// WHAT A PRODUCT SUPPLIES: the App Group its dev files live in, and where the
// squares go. Everything here is product-free. DEBUG only - the release branch
// compiles to no-ops so call sites stay unconditional.

import SwiftUI

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

    public static let band: CGFloat = 10
    public static let strip: CGFloat = 18
    public static let edge: CGFloat = 4
    public static let side: CGFloat = 12

    public static func pure(_ r: Double, _ g: Double, _ b: Double) -> Color {
        Color(.sRGB, red: r, green: g, blue: b, opacity: 1)
    }

    /// The square palette. Red and green are the edge bars' and never a square.
    /// Squares of one colour are told apart by position by the reader.
    public enum Ink: CaseIterable {
        case magenta, cyan, yellow, orange, blue, violet, lime, pink
        public var color: Color {
            switch self {
            case .magenta: return pure(1, 0, 1)
            case .cyan:    return pure(0, 1, 1)
            case .yellow:  return pure(1, 1, 0)
            case .orange:  return pure(1, 0.5, 0)
            case .blue:    return pure(0, 0, 1)
            case .violet:  return pure(0.5, 0, 1)
            case .lime:    return pure(0.5, 1, 0)
            case .pink:    return pure(1, 0, 0.5)
            }
        }
    }

    /// The clock strip's left edge, past the banded strip.
    public static let clockGap: CGFloat = 6

    /// The value the clock strip shows now: milliseconds modulo 16384. A log
    /// line that carries it can be matched to the filmed frame showing it.
    public static var clockMs: Int {
        Int((Date().timeIntervalSince1970 * 1000).rounded()) & ((1 << MotionRulerClock.bits) - 1)
    }

    static func bandColour(_ i: Int) -> Color {
        if i == 0 { return pure(1, 0, 0) }
        if i % 10 == 0 { return pure(1, 1, 0) }
        return i % 2 == 0 ? pure(0, 1, 1) : pure(1, 0, 1)
    }
}

/// The edge bars and the banded strip, filling whatever box it is laid on.
/// Attach it to the container that RESIZES, so it measures that box.
public struct MotionRulerEdges: View {
    let on: Bool
    public init(on: Bool) { self.on = on }

    public var body: some View {
        if on {
            GeometryReader { geo in
                let n = max(1, Int((geo.size.height / MotionRuler.band).rounded(.up)))
                ZStack(alignment: .topLeading) {
                    ForEach(0..<n, id: \.self) { i in
                        MotionRuler.bandColour(i)
                            .frame(width: MotionRuler.strip, height: MotionRuler.band)
                            .offset(y: CGFloat(i) * MotionRuler.band)
                    }
                    MotionRuler.pure(1, 0, 0)
                        .frame(width: geo.size.width, height: MotionRuler.edge)
                    MotionRulerClock()
                        .offset(x: MotionRuler.strip + MotionRuler.clockGap,
                                y: MotionRuler.edge)
                    MotionRuler.pure(0, 1, 0)
                        .frame(width: geo.size.width, height: MotionRuler.edge)
                        .offset(y: geo.size.height - MotionRuler.edge)
                }
                .frame(width: geo.size.width, height: geo.size.height, alignment: .topLeading)
                .clipped()
                // THE INSTRUMENT IS NEVER ANIMATED: a bar tweened by the
                // host's animation measures the tween, not the box.
                .transaction(value: geo.size.height) { $0.animation = nil }
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
    public static let bits = 14
    public static let cell: CGFloat = 12
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
    public init(on: Bool) {}
    public var body: some View { EmptyView() }
}

public extension View {
    func motionSquare(_ ink: MotionRuler.Ink, on: Bool,
                      at alignment: Alignment = .center) -> some View { self }
}

#endif
