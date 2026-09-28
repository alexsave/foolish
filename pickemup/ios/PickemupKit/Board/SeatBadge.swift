// COPIED from foolish/ios/FoolishKit/Boards/FSeatBadge.swift at c3d99192 - replaced by lift step S12
//
// foolish's badge (a 12pt name over a fan of 28 x 40 backs at a 10pt step,
// 112 wide) with the role row replaced by the 40pt slot this game needs
// (U12): LAST, Caught you!, Wrong call or OUT, and nothing the app computes.
//
// NO COUNT, EVER (D22, U6): the fan is always the kernel's PK_FAN_BACKS
// backs while the game is played, whatever the seat holds, and no numeral is
// drawn on it. At the end the reveal rows are the hand, face up.
//
// The fan is the Caught you! target (U13): pressed to .95 with an amber ring
// and a red tip while staged. Whose turn it is shows as a brass bar under
// the fan and the name in brass (U5).

import CPickemup
import SwiftUI

struct SeatBadge: View {
    let seat: Int
    let name: String
    /// Face-up cards at the end reveal; nil while the game is played.
    let revealed: [Int]?
    let isTurn: Bool
    let stamp: TableModel.Stamp?
    let calling: Bool
    /// The deal has not reached this seat yet: an empty fan (grid "Start").
    var fanEmpty = false
    /// At the end reveal, how many of `revealed` have turned face up so far.
    var revealShown: Int?
    let onTapFan: () -> Void

    @Environment(\.pkFX) private var fx

    private let nameMax: CGFloat = 96
    private var backs: Int { fanEmpty ? 0 : revealed?.count ?? PK_FAN_BACKS }
    private var step: CGFloat { PkLayout.fanStep(backs: backs) }
    private var fanWidth: CGFloat { PkLayout.fanCard.width + step * CGFloat(max(backs - 1, 0)) }

    var body: some View {
        VStack(spacing: 2) {
            Text(name)
                .font(.system(size: 12, weight: .semibold))
                .onFeltText(isTurn ? FColor.win : FColor.textPrimary)
                .lineLimit(1)
                .truncationMode(.tail)
                .frame(maxWidth: nameMax)
                .minimumScaleFactor(0.7)
                .pkAnchor("seat.\(seat)")
            fan
                .frame(width: max(fanWidth, PkLayout.fanCard.width), height: 44)
                .overlay(alignment: .bottom) {
                    // the turn bar; while a turn moves, the old one fades out
                    // and the new one in (grid "Turn moves")
                    let bar = fx["bar.\(seat)"]?.bar
                    if isTurn || bar != nil {
                        RoundedRectangle(cornerRadius: 2).fill(FColor.win)
                            .frame(height: 3).padding(.horizontal, 3).offset(y: 1)
                            .shadow(color: FColor.win.opacity(0.7), radius: 4)
                            .opacity(bar ?? 1)
                    }
                }
                .overlay(alignment: .top) {
                    // grid "Play a skip": UI.html `.skipbar`, wiping left to right
                    if let wipe = fx["fan.\(seat)"]?.slash {
                        RoundedRectangle(cornerRadius: 2).fill(FColor.red)
                            .frame(height: 4)
                            .padding(.horizontal, -5)
                            .mask(alignment: .leading) {
                                GeometryReader { g in Rectangle().frame(width: g.size.width * wipe) }
                            }
                            .rotationEffect(.degrees(-9))
                            .shadow(color: .black.opacity(0.6), radius: 1.5, y: 1)
                            .offset(y: 18)
                    }
                }
                .background {
                    let ring = fx["ring.\(seat)"]?.ring
                    if calling || ring != nil {
                        RoundedRectangle(cornerRadius: 10)
                            .fill(FColor.amber.opacity(0.12))
                            .overlay(RoundedRectangle(cornerRadius: 10).strokeBorder(FColor.amber, lineWidth: 1.5))
                            .padding(.horizontal, -9).padding(.top, -7).padding(.bottom, -3)
                            .opacity(ring ?? 1)
                    }
                }
                .overlay(alignment: .top) {
                    let ring = fx["ring.\(seat)"]?.ring
                    if calling || ring != nil { CatchTip().offset(y: -34).opacity(ring ?? 1) }
                }
                .scaleEffect(calling ? 0.95 : 1)
                .contentShape(Rectangle())
                .onTapGesture { onTapFan() }
                .pkAnchor("fan.\(seat)")
                .accessibilityElement(children: .ignore)
                .accessibilityLabel(Pk.words(PK_API_W_SPOKEN_FAN, seat))
                .accessibilityAddTraits(.isButton)
        }
        // THE STAMP HANGS UNDER THE FAN AND MOVES NOTHING (IOS_DECISIONS I45):
        // in the VStack its empty slot collapsed, so a stamp appearing
        // re-centred the badge and jumped the name and fan 21pt, and the
        // drawer's band (pk_lay_table_scale) could not know where the fan was
        .overlay(alignment: .bottom) {
            StampSlot(stamp: stamp)
                .frame(width: 112, height: 40, alignment: .top)
                .pkAnchor("slot.\(seat)")
                .offset(y: 42)
        }
        .frame(width: 112)
    }

    private var fan: some View {
        let n = backs
        let mid = Double(max(n - 1, 0)) / 2
        return ZStack {
            ForEach(0..<n, id: \.self) { i in
                let up = revealed != nil && i < (revealShown ?? n)
                PkCard(card: up ? revealed?[i] : nil, size: PkLayout.fanCard, fullFace: true)
                    .pkFX("fan.\(seat).\(i)")
                    .offset(x: CGFloat(Double(i) - mid) * step)
            }
        }
    }
}

/// The red "Caught you!" tip over a staged catch (U13).
struct CatchTip: View {
    var body: some View {
        Text(Pk.string("CAUGHT_WORD"))
            .font(.system(size: 11.5, weight: .bold))
            .foregroundColor(.white)
            .padding(.horizontal, 9).padding(.vertical, 6)
            .background(RoundedRectangle(cornerRadius: 9).fill(FColor.red))
            .shadow(color: .black.opacity(0.5), radius: 5, y: 3)
            .fixedSize()
            .allowsHitTesting(false)
    }
}

/// LAST (cream), Caught you! (red), Wrong call (grey), OUT (brass): U12.
struct StampSlot: View {
    let stamp: TableModel.Stamp?

    var body: some View {
        if let stamp {
            let (word, fill, ink): (String, Color, Color) = {
                switch stamp {
                case .last:   return (Pk.string("STAMP_LAST"), Color(hex: 0xF3EAD6), Color(hex: 0x2A2210))
                case .caught: return (Pk.string("CAUGHT_WORD"), FColor.red, .white)
                case .wrong:  return (Pk.string("STAMP_WRONG"), Color(hex: 0x5B5F66), Color(hex: 0xEEEEEE))
                case .out:    return (Pk.string("STAMP_OUT"), FColor.win, Color(hex: 0x241804))
                }
            }()
            // UI.html `.said`: one face for all four (800 11px, .06em), a 5pt
            // tail pointing up at the fan, 2pt below the slot's top (`.rrow`)
            Text(word)
                .font(.system(size: 11, weight: .heavy))
                .tracking(11 * 0.06)
                .foregroundColor(ink)
                .padding(.horizontal, 9).padding(.vertical, 6)
                .background(RoundedRectangle(cornerRadius: 9).fill(fill))
                .overlay(alignment: .top) { StampTail().fill(fill).frame(width: 10, height: 5).offset(y: -5) }
                .shadow(color: .black.opacity(0.45), radius: 4, y: 2)
                .fixedSize()
                .padding(.top, 2)
        }
    }
}

/// The stamp's speech tail, UI.html `.said::before`: a 10 x 5 triangle, point up.
private struct StampTail: Shape {
    func path(in r: CGRect) -> Path {
        var p = Path()
        p.move(to: CGPoint(x: r.midX, y: r.minY))
        p.addLine(to: CGPoint(x: r.maxX, y: r.maxY))
        p.addLine(to: CGPoint(x: r.minX, y: r.maxY))
        p.closeSubpath()
        return p
    }
}
