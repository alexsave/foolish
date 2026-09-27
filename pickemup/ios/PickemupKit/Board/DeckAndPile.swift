// COPIED from foolish/ios/FoolishKit/Boards/FDeckWell.swift at c3d99192 - replaced by lift step S12
//
// The deck: foolish's well turned portrait (U3) - 50 x 70 backs leaning 1
// left and 2 up a layer, foolish's layer counts (pk_lay_deck_layers), and the
// count ("27 left", D22) centred on its top layer the way foolish centres its
// count on the stock. No trump, no flipped card. The buried start cards peek
// from under its lower edge, face up and tilted (U16), for as long as the
// kernel says they are still there.
//
// The pile: the top card 82 x 115 at -3 degrees over up to three dimmed
// backs, on the live suit's halo (UI.html `.mid`, `.halo2`). A played wild
// carries its chosen suit as a band along its foot (U15).

import CPickemup
import SwiftUI

struct DeckStack: View {
    let count: Int
    let label: String
    let buried: [Int]
    /// Brass dashed band while a card dragged off the deck is over the hand.
    var lifted = false

    static let leanX: CGFloat = 1
    static let leanY: CGFloat = 2

    var body: some View {
        let layers = PkLayout.deckLayers(count)
        let size = PkLayout.deckSize
        ZStack(alignment: .topLeading) {
            ForEach(Array(buried.enumerated()), id: \.offset) { i, card in
                PkCard(card: card, size: size, fullFace: true)
                    .brightness(-0.08)
                    .rotationEffect(.degrees(i == 0 ? 9 : -7))
                    .offset(x: i == 0 ? 10 : -6, y: 18 + CGFloat(i) * 4)
            }
            ForEach(0..<layers, id: \.self) { i in
                PkCard(card: nil, size: size)
                    .offset(x: -CGFloat(i) * Self.leanX, y: -CGFloat(i) * Self.leanY)
                    .opacity(lifted && i == layers - 1 ? 0.55 : 1)
            }
            if layers == 0 {
                RoundedRectangle(cornerRadius: 5)
                    .strokeBorder(Color.white.opacity(0.18), style: StrokeStyle(lineWidth: 1.5, dash: [5, 4]))
                    .frame(width: size.width, height: size.height)
            }
            Text(label)
                .font(.system(size: 12.5, weight: .bold))
                .foregroundColor(.white)
                .shadow(color: .black.opacity(0.95), radius: 1, y: 1)
                .shadow(color: .black.opacity(0.8), radius: 3)
                .lineLimit(1)
                .fixedSize()
                .frame(width: size.width, height: size.height)
                .offset(x: -CGFloat(max(layers - 1, 0)) * Self.leanX, y: -CGFloat(max(layers - 1, 0)) * Self.leanY)
                .pkAnchor("deckn")
        }
        .frame(width: size.width, height: size.height, alignment: .topLeading)
        .pkAnchor("deck")
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(Pk.words(PK_API_W_SPOKEN_DECK))
        .accessibilityAddTraits(.isButton)
    }
}

struct PileView: View {
    let top: Int?
    let stackCount: Int
    let liveSuit: Int
    /// The top card is a wild whose suit was chosen: its band.
    let chosen: Int?
    /// A wild waiting on the picker sits here face up, halo clear (U14).
    let pending: Int?
    var hot = false

    private static let under: [(dx: CGFloat, dy: CGFloat, deg: Double)] = [(-6, 4, -8), (5, -2, 6), (2, 3, -3)]

    var body: some View {
        let size = PkLayout.pileSize
        ZStack {
            if pending == nil {
                Circle()
                    .fill(RadialGradient(colors: [SuitInk.halo(liveSuit), .clear], center: .center,
                                         // CSS `circle` is farthest-corner: 85 * sqrt 2, clear at 64% of it
                                         startRadius: 0, endRadius: 85 * 1.414 * 0.64))
                    .frame(width: 170, height: 170)
                    .pkAnchor("halo")
            }
            ForEach(0..<min(max(stackCount - 1, 0), 3), id: \.self) { i in
                PkCard(card: nil, size: size)
                    .brightness(-0.3)
                    .rotationEffect(.degrees(Self.under[i].deg))
                    .offset(x: Self.under[i].dx, y: Self.under[i].dy)
            }
            if let card = pending ?? top {
                PkCard(card: card, size: size, fullFace: true, chosen: pending == nil ? chosen : nil)
                    .rotationEffect(.degrees(-3))
            }
            if hot {
                RoundedRectangle(cornerRadius: 7)
                    .fill(FColor.win.opacity(0.12))
                    .overlay(RoundedRectangle(cornerRadius: 7)
                        .strokeBorder(FColor.win, style: StrokeStyle(lineWidth: 2.5, dash: [6, 4])))
                    .frame(width: size.width + 20, height: size.height + 19)
                    .allowsHitTesting(false)
            }
        }
        .frame(width: 170, height: 170)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(Pk.words(PK_API_W_SPOKEN_STACK))
    }
}

/// The top-right direction box (78 x 68 at y -3), absent at two players (D13).
struct DirectionBox: View {
    let word: String
    let clockwise: Bool

    var body: some View {
        VStack(spacing: 4) {
            DirectionArrow()
                .stroke(Color(hex: 0xB7D8D0), style: StrokeStyle(lineWidth: 3.4 * 34 / 40, lineCap: .round))
                .overlay(DirectionHead().fill(Color(hex: 0xB7D8D0)))
                .frame(width: 34, height: 34)
                .scaleEffect(x: clockwise ? 1 : -1)
            Text(word.uppercased())
                .font(.system(size: 8.5, weight: .bold))
                .tracking(1.2)
                .foregroundColor(Color(hex: 0x8FADA6))
                .lineLimit(1)
                .minimumScaleFactor(0.7)
        }
        .frame(width: 78, height: 68)
        .background(RoundedRectangle(cornerRadius: 9).fill(Color(red: 6 / 255, green: 20 / 255, blue: 18 / 255).opacity(0.4)))
        .overlay(RoundedRectangle(cornerRadius: 9).strokeBorder(Color(red: 140 / 255, green: 190 / 255, blue: 178 / 255).opacity(0.16)))
        .pkAnchor("dir")
    }
}

/// UI.html's arrow: an arc of radius 14 about (20, 20) in a 40-unit box.
private struct DirectionArrow: Shape {
    func path(in r: CGRect) -> Path {
        let s = r.width / 40
        var p = Path()
        p.addArc(center: CGPoint(x: 20 * s, y: 20 * s), radius: 14 * s,
                 startAngle: .degrees(-90), endAngle: .degrees(-90 - 327), clockwise: true)
        return p
    }
}

private struct DirectionHead: Shape {
    func path(in r: CGRect) -> Path {
        let s = r.width / 40
        var p = Path()
        p.move(to: CGPoint(x: 13.5 * s, y: 1.8 * s))
        p.addLine(to: CGPoint(x: 22.5 * s, y: 6.2 * s))
        p.addLine(to: CGPoint(x: 14.6 * s, y: 12.4 * s))
        p.closeSubpath()
        return p
    }
}
