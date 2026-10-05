// Cup.swift - a plain dice cup, mouth down: a tapered body, a rounded
// crown and a lip band at the mouth.
//
// PRODUCT-NEUTRAL (a later lift into shared/swift is possible). No theme and
// no published product's cup art (DECISIONS O2): a generic silhouette in
// warm dark brown with one highlight, drawn at any width, `aspect` tall.

import SwiftUI

public struct Cup: View {
    public var width: CGFloat
    public init(width: CGFloat = 44) { self.width = width }

    /// Height over width.
    public static let aspect: CGFloat = 1.1

    static let body1 = Color(hex: 0x5A3A24)
    static let body2 = Color(hex: 0x2E1C11)
    static let lip = Color(hex: 0x7A5234)

    public var body: some View {
        let h = width * Self.aspect
        ZStack {
            CupShape()
                .fill(LinearGradient(colors: [Self.body1, Self.body2], startPoint: .leading, endPoint: .trailing))
            // one soft highlight down the left flank
            CupShape()
                .fill(LinearGradient(colors: [.white.opacity(0.22), .clear],
                                     startPoint: .leading, endPoint: UnitPoint(x: 0.45, y: 0.5)))
            CupShape()
                .stroke(Color.black.opacity(0.45), lineWidth: max(0.5, width * 0.025))
            LipShape()
                .fill(Self.lip)
                .overlay(LipShape().stroke(Color.black.opacity(0.45), lineWidth: max(0.5, width * 0.02)))
        }
        .frame(width: width, height: h)
        .shadow(color: .black.opacity(0.45), radius: width * 0.08, y: width * 0.05)
        .accessibilityHidden(true)
    }
}

/// The body: narrow at the crown, wide at the mouth, the crown rounded.
public struct CupShape: Shape {
    public init() {}
    public func path(in r: CGRect) -> Path {
        let topInset = r.width * 0.2
        let crown = r.height * 0.12
        var p = Path()
        p.move(to: CGPoint(x: r.minX, y: r.maxY))
        p.addLine(to: CGPoint(x: r.minX + topInset, y: r.minY + crown))
        p.addQuadCurve(to: CGPoint(x: r.maxX - topInset, y: r.minY + crown),
                       control: CGPoint(x: r.midX, y: r.minY - crown * 0.6))
        p.addLine(to: CGPoint(x: r.maxX, y: r.maxY))
        p.closeSubpath()
        return p
    }
}

/// The dice count on a cup: a bone disc with the number (the bubble's
/// picture; on the table the crowns carry the kernel's own).
struct CountBadge: View {
    let n: Int
    var body: some View {
        Text(verbatim: "\(n)")
            .font(.system(size: 12, weight: .heavy).monospacedDigit())
            .foregroundColor(FColor.ink)
            .frame(width: 20, height: 20)
            .background(Circle().fill(FColor.card))
            .overlay(Circle().strokeBorder(Color.black.opacity(0.3), lineWidth: 1))
            .shadow(color: .black.opacity(0.4), radius: 1.5, y: 1)
    }
}

/// The lip band at the mouth.
struct LipShape: Shape {
    func path(in r: CGRect) -> Path {
        let band = r.height * 0.14
        return Path(roundedRect: CGRect(x: r.minX - r.width * 0.03, y: r.maxY - band,
                                        width: r.width * 1.06, height: band),
                    cornerRadius: band * 0.35)
    }
}
