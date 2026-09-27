// Die.swift - one six-sided die face, 1 to 6, at any size.
//
// PRODUCT-NEUTRAL (a later lift into shared/swift is possible): it knows a
// die's faces and nothing about this game's rules beyond the one visual
// convention that 1s are wild (R2), shown as a deep red pip so a 1 reads
// apart at a glance without shouting. Which dice COUNT for a bid is never
// decided here: the caller passes `highlight` from the model.
//
// Readable at 22pt: the pips are a tenth of the side (4.4pt across at 22pt),
// and the 3 x 3 pip grid sits at a quarter of the side from the centre, so
// no two pips touch at any size.

import SwiftUI

public struct Die: View {
    /// 1 to 6; anything else draws a blank face (a die not yet known).
    public let face: Int
    public var size: CGFloat
    /// The model says this die counts for the bid on the table (a reveal).
    public var highlight: Bool
    /// The model says this die does not count (a reveal).
    public var dimmed: Bool

    public init(face: Int, size: CGFloat = 32, highlight: Bool = false, dimmed: Bool = false) {
        self.face = face
        self.size = size
        self.highlight = highlight
        self.dimmed = dimmed
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
    /// A pip's diameter as a fraction of the side; the lone 1 is larger.
    static func pipDiameter(_ face: Int) -> CGFloat { face == 1 ? 0.24 : 0.2 }
    static let corner: CGFloat = 0.22

    /// The wild face's pip: the deep red of foolish's card edge (round 44).
    static let wildInk = FColor.deepRed
    static let ink = FColor.ink

    public var body: some View {
        let shape = RoundedRectangle(cornerRadius: size * Self.corner, style: .continuous)
        ZStack {
            shape.fill(FColor.card)
            // a soft lower edge so the face reads as a cube's top
            shape.strokeBorder(Color.black.opacity(0.18), lineWidth: max(0.5, size * 0.03))
            Canvas { ctx, s in
                let d = s.width * Self.pipDiameter(face)
                let ink = face == 1 ? Self.wildInk : Self.ink
                for p in Self.pips(face) {
                    let r = CGRect(x: p.x * s.width - d / 2, y: p.y * s.height - d / 2, width: d, height: d)
                    ctx.fill(Path(ellipseIn: r), with: .color(ink))
                }
            }
        }
        .frame(width: size, height: size)
        .overlay {
            if highlight {
                shape.strokeBorder(FColor.win, lineWidth: max(1.5, size * 0.08))
                    .padding(-size * 0.06)
            }
        }
        .shadow(color: .black.opacity(0.35), radius: size * 0.06, y: size * 0.04)
        .opacity(dimmed ? 0.45 : 1)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(Text(verbatim: face >= 1 && face <= 6 ? "\(face)" : ""))
    }
}
