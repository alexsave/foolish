// DiceFace.swift - one die, drawn (T10: no image assets, so no art to license).
//
// A rounded square of foolish's card bone with pips as ink circles on the
// classic 3 x 3 grid (T12). Value 0 is UNKNOWN (T11: a die being rerolled by a
// bubble not sent yet): the same square, dimmed, with a dashed inner outline
// and no pips, so it reads as "not rolled yet" and never as a value. A kept
// die carries a brass ring outside its edge.

import SwiftUI

public struct DiceFace: View {
    public let value: Int
    public var kept = false
    public var side: CGFloat = DiceFace.side

    /// T12: a die on the tray is 52pt, pickemup's widest hand card.
    public static let side: CGFloat = 52
    /// Corner radius, pip diameter and ring, as fractions of the side (T12).
    public static let cornerFraction: CGFloat = 0.2
    public static let pipFraction: CGFloat = 0.18
    public static let ringWidth: CGFloat = 3
    public static let ringGap: CGFloat = 3

    public init(value: Int, kept: Bool = false, side: CGFloat = DiceFace.side) {
        self.value = value
        self.kept = kept
        self.side = side
    }

    /// The pip centres for `value`, in unit coordinates of the face (0...1):
    /// none for 0 (unknown) or anything outside 1...6.
    public static func pips(_ value: Int) -> [CGPoint] {
        let lo: CGFloat = 0.27, mid: CGFloat = 0.5, hi: CGFloat = 0.73
        let c = CGPoint(x: mid, y: mid)
        let tl = CGPoint(x: lo, y: lo), tr = CGPoint(x: hi, y: lo)
        let bl = CGPoint(x: lo, y: hi), br = CGPoint(x: hi, y: hi)
        let ml = CGPoint(x: lo, y: mid), mr = CGPoint(x: hi, y: mid)
        switch value {
        case 1: return [c]
        case 2: return [tr, bl]
        case 3: return [tr, c, bl]
        case 4: return [tl, tr, bl, br]
        case 5: return [tl, tr, c, bl, br]
        case 6: return [tl, tr, ml, mr, bl, br]
        default: return []
        }
    }

    public var body: some View {
        let radius = side * Self.cornerFraction
        let shape = RoundedRectangle(cornerRadius: radius, style: .continuous)
        ZStack {
            shape.fill(FColor.card)
                .overlay(shape.strokeBorder(Color.black.opacity(0.25), lineWidth: 1))
                .shadow(color: .black.opacity(0.45), radius: 3, y: 2)
            if value == 0 {
                RoundedRectangle(cornerRadius: radius * 0.7, style: .continuous)
                    .strokeBorder(FColor.ink.opacity(0.35), style: StrokeStyle(lineWidth: 1.5, dash: [4, 3]))
                    .padding(side * 0.16)
            } else {
                Pips(value: value, diameter: side * Self.pipFraction)
            }
        }
        .opacity(value == 0 ? 0.55 : 1)
        .frame(width: side, height: side)
        .overlay {
            if kept {
                RoundedRectangle(cornerRadius: radius + Self.ringGap, style: .continuous)
                    .strokeBorder(FColor.win, lineWidth: Self.ringWidth)
                    .shadow(color: FColor.win.opacity(0.6), radius: 3)
                    .padding(-(Self.ringGap + Self.ringWidth))
            }
        }
        .accessibilityElement(children: .ignore)
        .accessibilityValue(value == 0 ? "?" : "\(value)")
    }
}

/// The pips of one face, one Circle each.
struct Pips: View {
    let value: Int
    let diameter: CGFloat

    var body: some View {
        GeometryReader { g in
            ForEach(Array(DiceFace.pips(value).enumerated()), id: \.offset) { _, p in
                Circle()
                    .fill(FColor.ink)
                    .frame(width: diameter, height: diameter)
                    .position(x: p.x * g.size.width, y: p.y * g.size.height)
            }
        }
    }
}
