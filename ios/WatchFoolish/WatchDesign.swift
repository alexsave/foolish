// WatchDesign.swift — the watch design language (docs/WATCHOS_APP_PLAN.md §3).
// Deliberately NOT the phone's wool/wood/fern: a true-black OLED canvas, token
// cards (value-first, suit as color), SF Rounded Bold, one brass accent. Built
// at 41/40mm first (§3, §12.7).

import SwiftUI

enum WColor {
    static let bg = Color.black                              // true black — OLED + always-on
    static let ink = Color.white                             // black suits render white on black
    static let red = Color(red: 0xFF/255, green: 0x45/255, blue: 0x3A/255)   // red suits #FF453A
    static let brass = Color(red: 0xD8/255, green: 0xB2/255, blue: 0x4A/255)  // the ONE accent
    static let dim = Color(white: 0.42)                      // counts / secondary
    static let faint = Color(white: 0.22)                    // gauge tracks, hairlines
}

enum WFont {
    // SF Rounded Bold throughout (§3): reads better than the phone's condensed
    // face at token size.
    static func token(_ pt: CGFloat) -> Font { .system(size: pt, weight: .bold, design: .rounded) }
    static func label(_ pt: CGFloat) -> Font { .system(size: pt, weight: .semibold, design: .rounded) }
}

/// suit 0..3 = spades, hearts, clubs, diamonds (same as FoolishKit Models).
enum Suit: Int, CaseIterable {
    case spades = 0, hearts = 1, clubs = 2, diamonds = 3
    var glyph: String { ["♠", "♥", "♣", "♦"][rawValue] }
    var isRed: Bool { self == .hearts || self == .diamonds }
    var color: Color { isRed ? WColor.red : WColor.ink }
}

/// A card: value 6…14 (11=J,12=Q,13=K,14=A) in the 36-card Durak deck.
struct Card: Identifiable, Equatable, Hashable {
    let suit: Suit
    let value: Int
    var id: String { "\(suit.rawValue):\(value)" }
    var label: String {
        switch value { case 14: return "A"; case 13: return "K"; case 12: return "Q"; case 11: return "J"; default: return "\(value)" }
    }
}

/// The token card — the atom of the whole watch UI (§3). Value-first; the suit
/// is the color plus a tiny glyph. Trump gets the brass ring (the one accent).
struct TokenCard: View {
    let card: Card
    var size: CGFloat = 30
    var selected: Bool = false
    var trump: Bool = false

    var body: some View {
        VStack(spacing: -2) {
            Text(card.label)
                .font(WFont.token(size))
                .foregroundStyle(card.suit.color)
            Text(card.suit.glyph)
                .font(WFont.token(size * 0.42))
                .foregroundStyle(card.suit.color)
        }
        .monospacedDigit()
        .frame(width: size * 1.35, height: size * 1.5)
        .background(
            RoundedRectangle(cornerRadius: 7, style: .continuous)
                .fill(selected ? Color(white: 0.12) : .clear)
        )
        .overlay(
            RoundedRectangle(cornerRadius: 7, style: .continuous)
                .strokeBorder(trump ? WColor.brass : (selected ? WColor.ink : .clear),
                              lineWidth: trump ? 1.5 : (selected ? 1.5 : 0))
        )
        .offset(y: selected ? -2 : 0)                // token lift on select (§5.2)
        .animation(.easeOut(duration: 0.12), value: selected)
    }
}
