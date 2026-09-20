import SwiftUI

/// The one door on the expanded sheet.
///
/// A SQUARE WHOSE EDGE AND FILL ARE BOTH ROUGH. It sits on the same piece of
/// paper as the board and is drawn by the same hand, so a flat rectangle would
/// be the only printed thing in the frame - and the glyph on it is filled with
/// its own hachure at about 55% because a bright solid book would fight the
/// fill underneath it.
public struct UtttRulebookButton: View {
    public let side: CGFloat
    private let action: () -> Void

    public init(side: CGFloat = 54, action: @escaping () -> Void = {}) {
        self.side = side
        self.action = action
    }

    public var body: some View {
        Button(action: action) {
            Canvas { ctx, size in
                for s in UtttRough.rulebook(w: size.width, h: size.height) {
                    ctx.stroke(s.path, with: .color(s.color),
                               style: StrokeStyle(lineWidth: s.width,
                                                  lineCap: .round, lineJoin: .round))
                }
            }
            .frame(width: side, height: side)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityLabel("Rulebook")
    }
}

/// Sketched geometry: a rough line, and a hachure fill made of rough lines.
///
/// TODO(kernel): THIS BELONGS IN C, beside uttt_pen.c, as
///
///     int uti_draw_rulebook(float w, float h);
///
/// handing its polygons back through the same uti_points / uti_poly_first /
/// uti_poly_n / uti_poly_rgba arrays as every other drawn thing, so that the
/// one rule this app has - Swift computes no coordinate - keeps being true.
/// It is Swift today only because the change that drew this door did not own
/// uttt/c. Nothing else in UtttKit calculates a point; this file is the single
/// exception and it is meant to stop being one.
///
/// The algorithm is rough.js's, because the design document is drawn with
/// rough.js and a different squiggle is a different door: a Lehmer generator
/// off a fixed seed, every line laid down twice as a bowed cubic with its ends
/// and its control points jittered, and a fill that is parallel scan lines
/// through the polygon drawn with that same line.
enum UtttRough {

    struct Stroke {
        let path: Path
        let color: Color
        let width: CGFloat
    }

    struct Options {
        var roughness: Double = 1
        var bowing: Double = 1
        var maxRandomnessOffset: Double = 2
    }

    /// The generator rough.js seeds with, so a seed means the same squiggle
    /// here as it does in the design document. A reference rather than a value
    /// because every line in a shape draws from the same stream, in order.
    final class RNG {
        private var s: UInt32
        init(_ seed: Int) { s = UInt32(truncatingIfNeeded: seed) }
        func next() -> Double {
            let m = Int32(truncatingIfNeeded: 48271 &* Int64(Int32(bitPattern: s)))
            s = UInt32(bitPattern: m) & 0x7fff_ffff
            return Double(s) / 2_147_483_648
        }
    }

    // MARK: the door

    private static var cache: [CGFloat: [Stroke]] = [:]

    static func rulebook(w: CGFloat, h: CGFloat) -> [Stroke] {
        if let c = cache[w], w == h { return c }

        let ink  = Color(red: 0.145, green: 0.216, blue: 0.420)            // #25376b
        let edge = Color(red: 0.106, green: 0.165, blue: 0.322)            // #1b2a52
        let page = Color(red: 0.886, green: 0.910, blue: 0.957, opacity: 0.55)

        var out: [Stroke] = []

        out += shape([CGPoint(x: 4, y: 4), CGPoint(x: w - 4, y: 4),
                      CGPoint(x: w - 4, y: h - 4), CGPoint(x: 4, y: h - 4)],
                     fill: ink, gap: 4.2, angle: -41, weight: 1.4,
                     stroke: edge, strokeWidth: 1.8,
                     o: Options(roughness: 1.5, bowing: 1.3), seed: 19)

        // The book: two leaves off one spine, each hachured the other way so
        // the fold reads without a line down the middle.
        out += shape([CGPoint(x: 15, y: 17), CGPoint(x: w / 2, y: 20),
                      CGPoint(x: w / 2, y: h - 15), CGPoint(x: 15, y: h - 18)],
                     fill: page, gap: 2.6, angle: 38, weight: 0.9,
                     stroke: page, strokeWidth: 1.3,
                     o: Options(roughness: 1.3), seed: 23)
        out += shape([CGPoint(x: w - 15, y: 17), CGPoint(x: w / 2, y: 20),
                      CGPoint(x: w / 2, y: h - 15), CGPoint(x: w - 15, y: h - 18)],
                     fill: page, gap: 2.6, angle: -38, weight: 0.9,
                     stroke: page, strokeWidth: 1.3,
                     o: Options(roughness: 1.3), seed: 29)

        if w == h { cache[w] = out }
        return out
    }

    /// One filled, outlined shape. The fill and the outline share a generator
    /// and consume it in that order, which is what rough.js does and what
    /// makes a seed reproduce a whole shape rather than half of one.
    private static func shape(_ points: [CGPoint],
                              fill: Color, gap: Double, angle: Double, weight: CGFloat,
                              stroke: Color, strokeWidth: CGFloat,
                              o: Options, seed: Int) -> [Stroke] {
        let rng = RNG(seed)

        var fillPath = Path()
        for seg in hachure(points, gap: gap, angle: angle) {
            doubleLine(seg.0, seg.1, o: o, rng: rng, into: &fillPath)
        }

        var edgePath = Path()
        for i in 0..<points.count {
            doubleLine(points[i], points[(i + 1) % points.count],
                       o: o, rng: rng, into: &edgePath)
        }

        return [Stroke(path: fillPath, color: fill, width: weight),
                Stroke(path: edgePath, color: stroke, width: strokeWidth)]
    }

    // MARK: the pen

    /// Every edge is drawn TWICE, once cleanly jittered and once with half the
    /// jitter over the top. One pass is a wobbly line; two passes is a line
    /// somebody drew.
    private static func doubleLine(_ p1: CGPoint, _ p2: CGPoint,
                                   o: Options, rng: RNG, into path: inout Path) {
        line(p1, p2, o: o, overlay: false, rng: rng, into: &path)
        line(p1, p2, o: o, overlay: true,  rng: rng, into: &path)
    }

    private static func line(_ p1: CGPoint, _ p2: CGPoint, o: Options,
                             overlay: Bool, rng: RNG, into path: inout Path) {
        let dx = Double(p2.x - p1.x), dy = Double(p2.y - p1.y)
        let lenSq = dx * dx + dy * dy
        let len = lenSq.squareRoot()

        // A long line wobbles proportionally less, or a board-wide rule comes
        // out as a scribble.
        var gain = 1.0
        if len > 500 { gain = 0.4 }
        else if len > 200 { gain = -0.0016668 * len + 1.233334 }

        var offset = o.maxRandomnessOffset
        if offset * offset * 100 > lenSq { offset = len / 10 }
        let half = offset / 2

        func jitter(_ lo: Double, _ hi: Double) -> Double {
            o.roughness * gain * (rng.next() * (hi - lo) + lo)
        }
        func full() -> Double { jitter(-offset, offset) }
        func small() -> Double { jitter(-half, half) }
        func about(_ x: Double) -> Double { jitter(-x, x) }

        let diverge = 0.2 + rng.next() * 0.2
        let midX = about(o.bowing * o.maxRandomnessOffset * dy / 200)
        let midY = about(o.bowing * o.maxRandomnessOffset * -dx / 200)

        let r: () -> Double = overlay ? small : full
        let x1 = Double(p1.x), y1 = Double(p1.y)
        let x2 = Double(p2.x), y2 = Double(p2.y)

        path.move(to: CGPoint(x: x1 + r(), y: y1 + r()))
        path.addCurve(
            to: CGPoint(x: x2 + r(), y: y2 + r()),
            control1: CGPoint(x: midX + x1 + dx * diverge + r(),
                              y: midY + y1 + dy * diverge + r()),
            control2: CGPoint(x: midX + x1 + 2 * dx * diverge + r(),
                              y: midY + y1 + 2 * dy * diverge + r()))
    }

    // MARK: the fill

    /// Parallel scan lines through a convex polygon, at the hachure angle.
    /// Rotate the shape so the lines are horizontal, walk it in steps of the
    /// gap, and rotate each span back - which is how rough.js does it and why
    /// the gap is measured across the lines rather than along an axis.
    private static func hachure(_ poly: [CGPoint], gap: Double,
                                angle: Double) -> [(CGPoint, CGPoint)] {
        let a = (angle + 90) * .pi / 180
        let c = cos(a), s = sin(a)
        let rot = poly.map { (x: Double($0.x) * c - Double($0.y) * s,
                              y: Double($0.x) * s + Double($0.y) * c) }
        func back(_ x: Double, _ y: Double) -> CGPoint {
            CGPoint(x: x * c + y * s, y: -x * s + y * c)
        }

        let step = max(gap, 0.1)
        guard let lo = rot.map(\.y).min(), let hi = rot.map(\.y).max() else { return [] }

        var out: [(CGPoint, CGPoint)] = []
        var y = lo
        while y <= hi {
            var left = Double.infinity, right = -Double.infinity
            for i in 0..<rot.count {
                let p = rot[i], q = rot[(i + 1) % rot.count]
                guard (p.y <= y && q.y > y) || (q.y <= y && p.y > y) else { continue }
                let x = p.x + (y - p.y) / (q.y - p.y) * (q.x - p.x)
                left = min(left, x); right = max(right, x)
            }
            if right > left { out.append((back(left, y), back(right, y))) }
            y += step
        }
        return out
    }
}
