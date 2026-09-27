// Scorecard.swift - one seat's card: thirteen rows in two halves (T4), the
// numbers-half sum and its bonus line, and the total.
//
// THE LAYOUT IS OURS, NOT THE BRANDED CARD'S (LEGAL.md, T14): the two halves
// stand SIDE BY SIDE as two columns on a bone paper panel, numbers on the
// left with the sum and the bonus under them, combinations on the right with
// the total under them. The branded card is one tall column with a "how to
// score" column beside it; this has no such column and no branded words.
//
// Every number is the kernel's: a filled row shows its score, an open row on
// my card shows what the dice on the tray would score there (TallyTable's
// preview, which is the kernel's), dimmed, and a tap on it scores there, which
// is how a turn ends (T3). A staged score (the draft, not sent) shows in brass.
// Another seat's card is the same view with no preview and no taps.

import SwiftUI

public struct ScoreRow: Equatable, Identifiable, Sendable {
    public enum State: Equatable, Sendable {
        /// Scored in an earlier turn.
        case filled(Int)
        /// Scored by the draft in the input field, not sent yet.
        case staged(Int)
        /// Still open: what the tray would score here, when I may score.
        case open(preview: Int?)
    }

    public let category: Category
    public let name: String
    public let state: State
    /// A tap scores the tray here.
    public let tappable: Bool
    public var id: Int { category.rawValue }

    /// The card's rows, numbers half then combinations half. `preview` is in
    /// Category order; `canScore` is the kernel's verdict for this phone and
    /// this card (false for anybody else's card).
    public static func rows(card: CardModel, preview: [Int?], draft: DraftKind, canScore: Bool,
                            name: (Category) -> String) -> [ScoreRow] {
        Category.allCases.map { c in
            let slot = card.score(c)
            let state: State
            if case .score(let d) = draft, d == c, let v = slot {
                state = .staged(v)
            } else if let v = slot {
                state = .filled(v)
            } else {
                state = .open(preview: canScore ? preview[safe: c.rawValue] ?? nil : nil)
            }
            let open: Bool = { if case .open = state { return true } else { return false } }()
            return ScoreRow(category: c, name: name(c), state: state, tappable: canScore && open)
        }
    }
}

public struct Scorecard: View {
    public let card: CardModel
    public let rows: [ScoreRow]
    public let words: (TallyString) -> String
    public let onPick: ((Category) -> Void)?

    /// T14: one row is 24pt, the panel's padding 10, the column gap 12.
    public static let rowHeight: CGFloat = 24
    public static let padding: CGFloat = 10
    public static let columnGap: CGFloat = 12

    public init(card: CardModel, rows: [ScoreRow], words: @escaping (TallyString) -> String,
                onPick: ((Category) -> Void)?) {
        self.card = card
        self.rows = rows
        self.words = words
        self.onPick = onPick
    }

    public var body: some View {
        HStack(alignment: .top, spacing: Self.columnGap) {
            VStack(spacing: 0) {
                ForEach(rows.filter { $0.category.isNumbers }) { row($0) }
                Divider().overlay(FColor.ink.opacity(0.35))
                line(words(.numbersSum), "\(card.numbersSum)", bold: false)
                line(words(.bonus), card.bonus.map { "\($0)" } ?? "-", bold: false)
            }
            Rectangle().fill(FColor.ink.opacity(0.2)).frame(width: 1)
            VStack(spacing: 0) {
                ForEach(rows.filter { !$0.category.isNumbers }) { row($0) }
                Divider().overlay(FColor.ink.opacity(0.35))
                line(words(.total), "\(card.total)", bold: true)
            }
        }
        .padding(Self.padding)
        .background(RoundedRectangle(cornerRadius: 8).fill(FColor.card))
        .overlay(RoundedRectangle(cornerRadius: 8).strokeBorder(Color.black.opacity(0.25), lineWidth: 1))
        .shadow(color: .black.opacity(0.4), radius: 6, y: 3)
        .tbAnchor("card")
    }

    @ViewBuilder private func row(_ r: ScoreRow) -> some View {
        let (text, ink, weight): (String, Color, Font.Weight) = {
            switch r.state {
            case .filled(let v):  return ("\(v)", FColor.ink, .heavy)
            case .staged(let v):  return ("\(v)", Color(hex: 0x8A6A12), .heavy)
            case .open(let p):    return (p.map { "\($0)" } ?? "", FColor.ink.opacity(0.4), .semibold)
            }
        }()
        let content = HStack(spacing: 4) {
            Text(r.name)
                .font(.system(size: 13, weight: .semibold))
                .foregroundStyle(FColor.ink.opacity(r.tappable || isFilled(r) ? 1 : 0.7))
                .lineLimit(1)
                .minimumScaleFactor(0.75)
            Spacer(minLength: 2)
            Text(text)
                .font(.system(size: 14, weight: weight).monospacedDigit())
                .foregroundStyle(ink)
        }
        .frame(height: Self.rowHeight)
        .background {
            if case .staged = r.state {
                RoundedRectangle(cornerRadius: 4).fill(FColor.win.opacity(0.25)).padding(.horizontal, -4)
            }
        }
        .contentShape(Rectangle())
        .tbAnchor("row.\(r.category.rawValue)")
        if r.tappable, let onPick {
            Button { Haptics.fire(.drop); onPick(r.category) } label: { content }
                .buttonStyle(FPressStyle())
                .accessibilityLabel("\(r.name) \(text)")
        } else {
            content.accessibilityElement(children: .combine)
        }
    }

    private func isFilled(_ r: ScoreRow) -> Bool {
        if case .open = r.state { return false }
        return true
    }

    private func line(_ label: String, _ value: String, bold: Bool) -> some View {
        HStack {
            Text(label).font(.system(size: 12.5, weight: bold ? .heavy : .semibold))
            Spacer(minLength: 2)
            Text(value).font(.system(size: 14, weight: .heavy).monospacedDigit())
        }
        .foregroundStyle(FColor.ink.opacity(bold ? 1 : 0.8))
        .frame(height: Self.rowHeight)
    }
}
