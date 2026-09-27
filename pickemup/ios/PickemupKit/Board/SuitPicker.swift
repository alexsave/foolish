// SuitPicker.swift - U14: four 60pt tiles, shape and colour and word, popped
// out of the wild on the pile to the compass points, over a .55 scrim, with
// an x. Picking dismisses it and only then is the wild played (D17); the x or
// a tap on the scrim cancels, and nothing was staged.
//
// Modal on the pile and never travels (UI.html anchor `picker`). No sheet
// and no presentation change is asked of Messages. The tiles' pop, ring and
// collapse are the flight layer's (motion grid "Play a wild"); this draws the
// open picker at rest.

import SwiftUI

struct SuitPicker: View {
    /// The pile's centre in the board space.
    let centre: CGPoint
    let onPick: (Int) -> Void
    let onCancel: () -> Void

    /// circles north, triangles east, squares south, diamonds west.
    static let reach = CGSize(width: 96, height: 104)
    private static let spots: [CGSize] = [CGSize(width: 0, height: -1), CGSize(width: 1, height: 0),
                                          CGSize(width: 0, height: 1), CGSize(width: -1, height: 0)]

    var body: some View {
        ZStack(alignment: .topLeading) {
            Color(red: 3 / 255, green: 10 / 255, blue: 9 / 255).opacity(0.55)
                .contentShape(Rectangle())
                .onTapGesture { onCancel() }
                .pkAnchor("scrim")
            ForEach(0..<4, id: \.self) { suit in
                let d = Self.spots[suit]
                Button { Haptics.fire(.drop); onPick(suit) } label: {
                    VStack(spacing: 2) {
                        SuitMark(suit: suit, ink: .white).frame(width: 26, height: 26)
                        Text(Pk.string("SUIT_\(suit)"))
                            .font(.system(size: 9.5, weight: .bold))
                            .tracking(9.5 * 0.02)
                            .foregroundColor(.white)
                            .lineLimit(1)
                            .minimumScaleFactor(0.7)
                    }
                    .frame(width: 60, height: 60)
                    .background(RoundedRectangle(cornerRadius: 14).fill(SuitInk.color(suit)))
                    // UI.html `.pt`: inset 0 1px 0 rgba(255,255,255,.2), the tile's lit top edge
                    .overlay(RoundedRectangle(cornerRadius: 14)
                        .subtracting(RoundedRectangle(cornerRadius: 14).offset(x: 0, y: 1))
                        .fill(Color.white.opacity(0.2)))
                    .shadow(color: .black.opacity(0.55), radius: 10, y: 8)
                }
                .buttonStyle(FPressStyle())
                .position(x: centre.x + d.width * Self.reach.width, y: centre.y + d.height * Self.reach.height)
                .pkAnchor("picker.\(suit)")
            }
            Button(action: onCancel) {
                Text("\u{00D7}")
                    .font(.system(size: 15, weight: .semibold))
                    .foregroundColor(Color(hex: 0xCFE0DA))
                    .frame(width: 30, height: 30)
                    .background(Circle().fill(Color(red: 20 / 255, green: 30 / 255, blue: 28 / 255).opacity(0.92)))
                    .overlay(Circle().strokeBorder(Color(red: 190 / 255, green: 215 / 255, blue: 220 / 255).opacity(0.3)))
            }
            .position(x: centre.x + Self.reach.width, y: centre.y - Self.reach.height)
        }
        .pkAnchor("picker")
    }
}
