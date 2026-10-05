// StageHUD.swift - what lies FLAT over the turned table (DECISIONS I25): the
// bid plate and the shelf, each at the frame the stage's HUD gives, never
// turned. The places are the kernel's; the words are the model's; the plate
// is the study's (I27).

import SwiftUI

extension CnStageHudSnap {
    /// A HUD rectangle (x y w h, points) as a CGRect; .null for a short array.
    static func rect(_ a: [Double]) -> CGRect {
        a.count == 4 ? CGRect(x: a[0], y: a[1], width: a[2], height: a[3]) : .null
    }

    var plateRect: CGRect? { hasPlate == 1 ? Self.rect(plate) : nil }
    var shelfRect: CGRect? { hasShelf == 1 ? Self.rect(shelf) : nil }
}

extension View {
    /// Put this view at a HUD rectangle (the drawer's points, top-left origin).
    func at(_ r: CGRect) -> some View {
        frame(width: r.width, height: r.height).position(x: r.midX, y: r.midY)
    }
}

/// The bid plate (`.plate`, 160 by 56 on a tall board): the verdigris plate
/// with its four rivets, the bevel ring and the barnacle crust in its corner;
/// the bid in the serif at 26 beside its face as a bone die at 30 (20 and 24
/// on a plate under 140 wide, the short board's).
struct BidPlate: View {
    let text: String
    let face: Int?
    var seed = 7

    /// The one-line sizes tried in turn: the study's 26 (20 on a narrow plate)
    /// and two steps down, none under the roman's 15.5 floor (the Type tab).
    static func sizes(narrow: Bool) -> [CGFloat] { narrow ? [20, 17.5, 15.5] : [26, 22, 18, 15.5] }

    var body: some View {
        GeometryReader { geo in
            let narrow = geo.size.width < 140
            ZStack {
                PlateMetal(seed: seed) { BronzeTint(top: 0.12, mid: 0.4, foot: 0.38) }
                PlateCrust()
                    .frame(width: 120, height: 48)
                    .position(x: -10 + 60, y: geo.size.height + 14 - 24)
                    .clipShape(RoundedRectangle(cornerRadius: 5, style: .circular))
                ForEach(0..<4, id: \.self) { i in
                    Rivet().position(x: i % 2 == 0 ? 8.5 : geo.size.width - 8.5, y: i < 2 ? 8.5 : geo.size.height - 8.5)
                }
                HStack(spacing: 10) {
                    // the study's size when the words fit on one line (a bid
                    // always does); a longer line (the reveal's tally) steps
                    // down, then takes two lines, never cut
                    ViewThatFits(in: .horizontal) {
                        ForEach(Self.sizes(narrow: narrow), id: \.self) { s in
                            Text(text).font(FType.serif(s)).bidInk().lineLimit(1).fixedSize()
                        }
                        Text(text).font(FType.serif(15.5)).bidInk().lineLimit(2).minimumScaleFactor(0.8)
                    }
                    .layoutPriority(1)
                    if let face { Die(face: face, size: narrow ? 24 : 30, seed: 80) }
                }
                .padding(.horizontal, 12)
            }
            .frame(width: geo.size.width, height: geo.size.height)
        }
        .accessibilityElement(children: .combine)
    }
}
