// Cup.swift - the dice cup seen from straight above, flat (UI.html
// SHAPE.cupTop; DECISIONS I27): the lobby's roster carries one per seat. The
// table's cups are the kernel's 3D bodies; this is the same verdigris cup
// for the places that are not the table.
//
// The side falls away toward the outer edge, the crown (72% of the radius)
// takes the one cold light from the top-left, the crust grows on the side,
// and the count is stamped on the crown in the roman.

import SwiftUI

public struct Cup: View {
    public var width: CGFloat
    public var count: Int?
    public var seed: Int
    public init(width: CGFloat = 44, count: Int? = nil, seed: Int = 0) {
        self.width = width
        self.count = count
        self.seed = seed
    }

    /// The crown's radius over the mouth's.
    static let crown: CGFloat = 0.72

    public var body: some View {
        let R = width / 2, rc = R * Self.crown
        ZStack {
            // the side: the verdigris, darkening toward the outer edge where it falls away
            VerdFill(seed: seed + 41)
                .overlay(RadialGradient(stops: [.init(color: .black.opacity(0.1), location: 0.55),
                                                .init(color: .black.opacity(0.4), location: 0.85),
                                                .init(color: .black.opacity(0.7), location: 1)],
                                        center: .center, startRadius: 0, endRadius: R))
                .clipShape(Circle())
            // the crown: its own slice, under the light
            VerdFill(seed: seed + 42)
                .overlay(RadialGradient(stops: [.init(color: .white.opacity(0.28), location: 0),
                                                .init(color: .white.opacity(0), location: 0.55),
                                                .init(color: .black.opacity(0.38), location: 1)],
                                        center: UnitPoint(x: 0.36, y: 0.32), startRadius: 0, endRadius: rc * 1.2))
                .clipShape(Circle())
                .frame(width: 2 * rc, height: 2 * rc)
            Circle().stroke(Color.black.opacity(0.45), lineWidth: max(0.6, width * 0.02)).frame(width: 2 * rc, height: 2 * rc)
            Circle().stroke(Color.black.opacity(0.6), lineWidth: max(0.8, width * 0.03))
            if let count {
                ZStack {
                    Text(verbatim: "\(count)").font(FType.serif(rc * 1.05)).foregroundStyle(Color(hex: 0x0A0D0B).opacity(0.85)).offset(y: 0.8)
                    Text(verbatim: "\(count)").font(FType.serif(rc * 1.05)).foregroundStyle(Color(hex: 0xE8ECD8).opacity(0.9))
                }
            }
        }
        .frame(width: width, height: width)
        .shadow(color: .black.opacity(0.55), radius: 2, y: 2)
        .accessibilityHidden(true)
    }
}
