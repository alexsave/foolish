// Chrome.swift - the status corner with its staged-turn strip (U4, U10), the
// pill row (U9), the left squares or the Last card! pill (U11), and the toast.
//
// The pills never animate (foolish's rule) and sit in a ROW: Draw holds the
// trailing slot whenever it is legal and never moves; which verb stands in
// which slot is the kernel's layout (pk_lay_pills). Every word is the
// kernel's table.

import CPickemup
import SwiftUI

struct StatusCorner: View {
    let headline: String
    let subline: String
    let strip: TableModel.Strip
    let onUnsay: () -> Void

    var body: some View {
        // UI.html: .strip margin 5 above and 1 below, .ss margin-top 3
        VStack(alignment: .leading, spacing: 0) {
            Text(headline)
                .font(.system(size: 15, weight: .bold))
                .onFeltText()
                .lineLimit(2)
                .fixedSize(horizontal: false, vertical: true)
            if !strip.isEmpty {
                StagedStrip(strip: strip, onUnsay: onUnsay)
                    .pkAnchor("strip")
                    .padding(.top, 5).padding(.bottom, 1)
            }
            if !subline.isEmpty {
                Text(subline)
                    .font(.system(size: 12, weight: .medium))
                    .onFeltText(Color(hex: 0xCFD8CF))
                    .lineLimit(3)
                    .fixedSize(horizontal: false, vertical: true)
                    .padding(.top, 3)
            }
        }
        .frame(width: 128, alignment: .topLeading)
        .pkAnchor("status")
    }
}

/// U10: chips only - a back with a count for the draws, a riffle for a
/// reshuffle, a mini card for the play, the call word for Last card! (tap to
/// take it back) and the catch word on a call. The words are in the bubble's
/// caption.
struct StagedStrip: View {
    let strip: TableModel.Strip
    let onUnsay: () -> Void

    var body: some View {
        HStack(spacing: 4) {
            if strip.draws > 0 {
                PkCard(card: nil, size: CGSize(width: 12, height: 17), chip: true)
                Text(Pk.words(PK_API_W_STRIP_DRAWS, strip.draws))
                    .font(.system(size: 11, weight: .heavy)).foregroundColor(Color(hex: 0xFFF1C9))
            }
            if strip.reshuffled {
                RiffleMark()
                    .stroke(Color(hex: 0xE3C985), style: StrokeStyle(lineWidth: 2 * 14 / 24, lineCap: .round, lineJoin: .round))
                    .frame(width: 14, height: 14)
            }
            if let card = strip.played {
                if strip.draws > 0 || strip.reshuffled {
                    // UI.html's middle-dot separator, drawn: no glyph in a view is a word
                    Circle().fill(Color(hex: 0x9C8B5E)).frame(width: 3, height: 3)
                        .accessibilityHidden(true)
                }
                PkCard(card: card, size: CGSize(width: 12, height: 17), chosen: strip.chosen, chip: true)
            }
            if strip.said {
                Text(Pk.string("CALL_WORD"))
                    .font(.system(size: 11.5, weight: .semibold)).foregroundColor(Color(hex: 0xE9DCB5))
                    .onTapGesture { onUnsay() }
            }
            if strip.called != nil {
                Text(Pk.string("CAUGHT_WORD"))
                    .font(.system(size: 11.5, weight: .semibold)).foregroundColor(Color(hex: 0xE9DCB5))
            }
        }
        .lineLimit(1)
        .padding(.leading, 6).padding(.trailing, 9)
        .frame(height: 24)
        .background(Capsule().fill(Color(red: 8 / 255, green: 22 / 255, blue: 15 / 255).opacity(0.72)))
        .overlay(Capsule().strokeBorder(Color(red: 216 / 255, green: 178 / 255, blue: 74 / 255).opacity(0.55)))
        .fixedSize()
    }
}

/// The pill row, trailing; 96-wide wood, 8 apart, trailing edge 12 in; its
/// row is the kernel's zone (PK_ZONE_PILLS).
struct PillRow: View {
    let trailing: PkLayout.Pill
    let leading: PkLayout.Pill
    /// The drawer's column: the leading pill above the trailing one (O10).
    var stacked = false
    let onDraw: () -> Void
    let onPlay: () -> Void
    let onPass: () -> Void
    let onUndo: () -> Void

    static let width: CGFloat = 96

    var body: some View {
        Group {
            if stacked {
                HStack(spacing: 0) {
                    Spacer(minLength: 0)
                    VStack(spacing: PkLayout.pillStackGap) {
                        pill(leading)
                        pill(trailing)
                    }
                }
            } else {
                HStack(spacing: 8) {
                    Spacer(minLength: 0)
                    pill(leading)
                    pill(trailing)
                }
                .frame(height: PkLayout.pillHeight)
            }
        }
        .padding(.trailing, 12)
        .transaction { $0.animation = nil }
        .pkAnchor("pills")
    }

    @ViewBuilder private func pill(_ p: PkLayout.Pill) -> some View {
        switch p {
        case .none:
            EmptyView()
        case .draw:
            WoodButton(title: Pk.string("BTN_DRAW"), width: Self.width, action: onDraw).pkAnchor("pill.draw")
        case .play:
            WoodButton(title: Pk.string("BTN_PLAY"), width: Self.width, action: onPlay).pkAnchor("pill.play")
        case .pass:
            WoodButton(title: Pk.string("BTN_PASS"), width: Self.width, action: onPass).pkAnchor("pill.pass")
        case .undo:
            WoodButton(title: Pk.string("BTN_UNDO"), width: Self.width, action: onUndo).pkAnchor("pill.undo")
        }
    }
}

/// The left side: the rules square, or the Last card! pill in its place
/// while I may say it (U11), amber lettering on the same wood.
struct LeftChrome: View {
    let maySay: Bool
    let onSay: () -> Void
    let onRules: () -> Void

    var body: some View {
        HStack(spacing: 16) {
            if maySay {
                WoodButton(title: Pk.string("BTN_SAY"), width: PillRow.width, ink: Color(hex: 0xFFE7A6), action: onSay)
                    .pkAnchor("pill.say")
            } else {
                SquareButton(systemImage: "book.fill", accessibility: Pk.string("BTN_RULES"), action: onRules)
                    .pkAnchor("squares")
            }
            Spacer(minLength: 0)
        }
        .padding(.leading, 12)
        .frame(height: PkLayout.pillHeight)
        .transaction { $0.animation = nil }
    }
}

/// "That card doesn't match" and its kin: a dark capsule above the hand.
struct Toast: View {
    let text: String
    var body: some View {
        Text(text)
            .font(.system(size: 12.5, weight: .semibold))
            .foregroundColor(FColor.card)
            .padding(.horizontal, 12).padding(.vertical, 8)
            .background(Capsule().fill(Color.black.opacity(0.62)))
            .fixedSize()
            .allowsHitTesting(false)
    }
}

/// The strip's reshuffle mark, UI.html `svg.rf` (24-unit box): two opposed
/// hooked arrows, `M4 7h11l-3-3 M20 17H9l3 3`.
private struct RiffleMark: Shape {
    func path(in r: CGRect) -> Path {
        let s = r.width / 24
        func p(_ x: CGFloat, _ y: CGFloat) -> CGPoint { CGPoint(x: r.minX + x * s, y: r.minY + y * s) }
        var path = Path()
        path.move(to: p(4, 7)); path.addLine(to: p(15, 7)); path.addLine(to: p(12, 4))
        path.move(to: p(20, 17)); path.addLine(to: p(9, 17)); path.addLine(to: p(12, 20))
        return path
    }
}
