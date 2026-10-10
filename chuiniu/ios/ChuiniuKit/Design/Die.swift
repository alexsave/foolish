// Die.swift - one die face, 1 to 6, as the study cuts it (UI.html SHAPE.die,
// the Dice tab; DECISIONS I27): a bone face (the baked tallow, cn_bone.png,
// each die its own slice of it), lit from the top-left, worn dark at the
// corners and edges where it hits the table, a bevel, and DRILLED pips: a
// dark hole whose far wall catches the light. The 1's pip is the deep red
// locked in foolish's round 44 (#8B1A1A), so a wild 1 reads apart at a glance.
//
// Two states the caller decides from the model, never here: `counts` (the
// die counts for the bid: the aquamarine ring, the counting ring) and
// `drowned` (it does not: under the sea's tint). Which dice count is the
// kernel's (`Reveal.counts`).
//
// The face is `size` square; the ring and the drop shadow reach a little past
// it (the study's die is drawn in a box 16% bigger each way).

import SwiftUI

public struct Die: View {
    /// 1 to 6; anything else draws a blank face (a die not yet known).
    public let face: Int
    public var size: CGFloat
    /// The model says this die counts for the bid on the table.
    public var counts: Bool
    /// The model says this die does not count (a reveal).
    public var drowned: Bool
    /// Which slice of the bone and which wear: variety for the eye only.
    public var seed: Int

    public init(face: Int, size: CGFloat = 32, counts: Bool = false, drowned: Bool = false, seed: Int? = nil) {
        self.face = face
        self.size = size
        self.counts = counts
        self.drowned = drowned
        self.seed = seed ?? face
    }

    /// The pip centres of `face` on a unit square (0...1 each way), in
    /// reading order. The one table of where pips go; the view and the tests
    /// both read it.
    public static func pips(_ face: Int) -> [CGPoint] {
        let lo = 0.5 - pipOffset, mid = 0.5, hi = 0.5 + pipOffset
        switch face {
        case 1: return [CGPoint(x: mid, y: mid)]
        case 2: return [CGPoint(x: hi, y: lo), CGPoint(x: lo, y: hi)]
        case 3: return [CGPoint(x: hi, y: lo), CGPoint(x: mid, y: mid), CGPoint(x: lo, y: hi)]
        case 4: return [CGPoint(x: lo, y: lo), CGPoint(x: hi, y: lo),
                        CGPoint(x: lo, y: hi), CGPoint(x: hi, y: hi)]
        case 5: return [CGPoint(x: lo, y: lo), CGPoint(x: hi, y: lo), CGPoint(x: mid, y: mid),
                        CGPoint(x: lo, y: hi), CGPoint(x: hi, y: hi)]
        case 6: return [CGPoint(x: lo, y: lo), CGPoint(x: hi, y: lo),
                        CGPoint(x: lo, y: mid), CGPoint(x: hi, y: mid),
                        CGPoint(x: lo, y: hi), CGPoint(x: hi, y: hi)]
        default: return []
        }
    }

    /// How far a corner pip sits from the centre, as a fraction of the side.
    static let pipOffset: CGFloat = 0.25
    /// A pip's diameter as a fraction of the side (the study's pip size m:
    /// .21, the lone 1 .26).
    static func pipDiameter(_ face: Int) -> CGFloat { face == 1 ? 0.26 : 0.21 }
    /// The face's corner radius (the study's 'square' shape, 18%).
    static let corner: CGFloat = 0.18
    /// The pips' hand-drilled jitter, a fraction of the side ('slight').
    static let jitter: CGFloat = 0.012

    /// The drilled pip: black at the bottom of the hole, the far wall lit.
    static let pipStops: [Gradient.Stop] = [.init(color: Color(hex: 0x000406), location: 0),
                                            .init(color: Color(hex: 0x0A1210), location: 0.62),
                                            .init(color: Color(hex: 0x36463D), location: 1)]
    /// The wild 1: the same hole in blood, #8B1A1A at its body.
    static let wildStops: [Gradient.Stop] = [.init(color: Color(hex: 0x3A0906), location: 0),
                                             .init(color: Ink.wild, location: 0.55),
                                             .init(color: Color(hex: 0xB2503F), location: 1)]

    public var body: some View {
        Canvas { ctx, sz in
            Self.draw(ctx, side: sz.width, face: face, seed: seed, drowned: drowned)
        }
        .frame(width: size, height: size)
        .shadow(color: .black.opacity(0.55), radius: 2, y: 2)
        .overlay {
            if counts { CountRing(size: size) }
        }
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(Text(verbatim: face >= 1 && face <= 6 ? "\(face)" : ""))
    }

    static func facePath(_ s: CGFloat) -> Path {
        Path(roundedRect: CGRect(x: 0, y: 0, width: s, height: s), cornerRadius: s * corner, style: .circular)
    }

    static func draw(_ ctx: GraphicsContext, side s: CGFloat, face: Int, seed: Int, drowned: Bool) {
        let path = facePath(s)
        ctx.drawLayer { l in
            if drowned { l.opacity = 0.7 }
            l.clip(to: path)
            // the bone, at this die's own slice of the tile (64 points a tile)
            if let img = CnTextures.bone {
                let o = PaintSeed.offset(seed + 31, span: 64), image = Image(uiImage: img)
                var y = o.y
                while y < s {
                    var x = o.x
                    while x < s { l.draw(image, in: CGRect(x: x, y: y, width: 64, height: 64)); x += 64 }
                    y += 64
                }
            } else {
                l.fill(path, with: .color(Ink.bone))
            }
            // the one cold light, top-left
            l.fill(path, with: .linearGradient(
                Gradient(stops: [.init(color: .white.opacity(0.26), location: 0), .init(color: .white.opacity(0), location: 0.45),
                                 .init(color: .black.opacity(0.30), location: 1)]),
                startPoint: .zero, endPoint: CGPoint(x: s, y: s)))
            // strike wear: grime at each corner and along each edge, by seed
            let wear = Color(hex: 0x4E3F26)
            for c in 0..<4 {
                let cx: CGFloat = c & 1 == 1 ? s : 0, cy: CGFloat = c & 2 == 2 ? s : 0
                let rr = s * (0.16 + 0.18 * PaintSeed.unit(c, seed, 911)), op = 0.2 + 0.24 * PaintSeed.unit(c + 4, seed, 911)
                l.fill(Path(ellipseIn: CGRect(x: cx - rr, y: cy - rr, width: 2 * rr, height: 2 * rr)),
                       with: .radialGradient(Gradient(stops: [.init(color: wear.opacity(op), location: 0),
                                                              .init(color: wear.opacity(0.55 * op), location: 0.45),
                                                              .init(color: wear.opacity(0), location: 1)]),
                                             center: CGPoint(x: cx, y: cy), startRadius: 0, endRadius: rr))
            }
            let r = s * corner
            let edges: [(CGPoint, CGPoint)] = [(CGPoint(x: r, y: 0), CGPoint(x: s - r, y: 0)), (CGPoint(x: s, y: r), CGPoint(x: s, y: s - r)),
                                               (CGPoint(x: r, y: s), CGPoint(x: s - r, y: s)), (CGPoint(x: 0, y: r), CGPoint(x: 0, y: s - r))]
            for (e, (a, b)) in edges.enumerated() {
                let op = 0.08 + 0.18 * PaintSeed.unit(e + 8, seed, 911)
                var line = Path(); line.move(to: a); line.addLine(to: b)
                l.stroke(line, with: .color(wear.opacity(op)), style: StrokeStyle(lineWidth: s * 0.11, lineCap: .round))
                l.stroke(line, with: .color(Color(hex: 0x3D3120).opacity(min(1, op * 1.2))), style: StrokeStyle(lineWidth: s * 0.045, lineCap: .round))
            }
            // the bevel: lit top-left, dark bottom-right, and the dark lip
            l.stroke(path, with: .linearGradient(
                Gradient(stops: [.init(color: .white.opacity(0.42), location: 0), .init(color: .white.opacity(0.04), location: 0.5),
                                 .init(color: .black.opacity(0.08), location: 0.55), .init(color: .black.opacity(0.5), location: 1)]),
                startPoint: .zero, endPoint: CGPoint(x: s, y: s)), lineWidth: s * 0.1)
            l.stroke(path, with: .color(.black.opacity(0.4)), lineWidth: max(0.6, s * 0.03) * 2)
            // the pips, drilled, each a little off its station
            let d = s * pipDiameter(face), jit = s * jitter
            for p in pips(face) {
                let ix = Int(p.x * 100), iy = Int(p.y * 100)
                let jx = (PaintSeed.unit(ix, iy, seed + face) - 0.5) * 2 * jit, jy = (PaintSeed.unit(iy, ix, seed + face + 1) - 0.5) * 2 * jit
                let c = CGPoint(x: p.x * s + jx, y: p.y * s + jy)
                let pip = Path(ellipseIn: CGRect(x: c.x - d / 2, y: c.y - d / 2, width: d, height: d))
                // the hole's bottom sits up-left of the centre, its lit far wall down-right
                l.fill(pip, with: .radialGradient(Gradient(stops: face == 1 ? wildStops : pipStops),
                                                  center: CGPoint(x: c.x - d * 0.06, y: c.y - d * 0.08),
                                                  startRadius: 0, endRadius: d * 0.62))
                l.stroke(pip, with: .color(.black.opacity(0.45)), lineWidth: max(0.4, d * 0.06))
            }
            if drowned { l.fill(path, with: .color(Ink.sea.opacity(0.55))) }
        }
    }
}

/// The counting ring (`o.counts`): the face's outline 7% out, in the glow,
/// blurred, with a pale core.
struct CountRing: View {
    let size: CGFloat
    var body: some View {
        let g = size * 0.07, sw = max(1.5, size * 0.07), side = size + 2 * g
        let shape = RoundedRectangle(cornerRadius: side * Die.corner, style: .circular)
        ZStack {
            shape.stroke(Ink.glow, lineWidth: sw).blur(radius: size < 30 ? 1.2 : 2.2)
            shape.stroke(Ink.glow, lineWidth: sw)
            shape.stroke(Color(hex: 0xE9FFF7).opacity(0.9), lineWidth: sw * 0.35)
        }
        .frame(width: side, height: side)
        .allowsHitTesting(false)
    }
}
