// WatchDesign.swift — the watch design language (docs/WATCHOS_APP_PLAN.md §3).
// Deliberately NOT the phone's wool/wood/fern: a true-black OLED canvas, token
// cards (value nested inside a larger suit), SF Rounded Bold, one brass accent.

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

/// The token card — the atom of the whole watch UI. The value sits INSIDE a
/// larger suit glyph (same center, value on top): the suit is the shape, the
/// value is knocked into it (white on red suits, black on the white ones).
/// Compact and iconic; used everywhere, including the flipped trump.
struct TokenCard: View {
    let card: Card
    var size: CGFloat = 30
    var selected: Bool = false

    private var valueColor: Color { card.suit.isRed ? WColor.ink : WColor.bg }

    var body: some View {
        ZStack {
            Text(card.suit.glyph)
                .font(WFont.token(size * 1.5))
                .foregroundStyle(card.suit.color)
            Text(card.label)
                .font(.system(size: size * (card.value == 10 ? 0.5 : 0.62), weight: .heavy, design: .rounded))
                .foregroundStyle(valueColor)
                .monospacedDigit()
                .offset(y: size * 0.06)          // nudge into the suit's solid body
        }
        .frame(width: size * 1.2, height: size * 1.4)
        .background(
            RoundedRectangle(cornerRadius: 7, style: .continuous)
                .fill(selected ? Color(white: 0.16) : .clear)
        )
        .overlay(
            RoundedRectangle(cornerRadius: 7, style: .continuous)
                .strokeBorder(selected ? WColor.ink : .clear, lineWidth: 1.5)
        )
    }
}

/// A minimal upward sword (§ owner request): a pointed blade, a short crossguard,
/// grip + pommel. Drawn in a Canvas so it's monochrome, takes the tint, and reads
/// as a sword (not a plus) even at chip size — SF Symbols has no sword.
struct SwordIcon: View {
    var size: CGFloat = 16
    var color: Color = WColor.bg

    var body: some View {
        Canvas { ctx, sz in
            let w = sz.width, h = sz.height, cx = w / 2
            var blade = Path()                                   // pointed tip → down to the guard
            blade.move(to: CGPoint(x: cx, y: 0))
            blade.addLine(to: CGPoint(x: cx + w * 0.09, y: h * 0.16))
            blade.addLine(to: CGPoint(x: cx + w * 0.09, y: h * 0.60))
            blade.addLine(to: CGPoint(x: cx - w * 0.09, y: h * 0.60))
            blade.addLine(to: CGPoint(x: cx - w * 0.09, y: h * 0.16))
            blade.closeSubpath()
            ctx.fill(blade, with: .color(color))
            ctx.fill(Path(roundedRect: CGRect(x: cx - w * 0.30, y: h * 0.58, width: w * 0.60, height: h * 0.10),
                          cornerRadius: h * 0.05), with: .color(color))    // crossguard
            ctx.fill(Path(roundedRect: CGRect(x: cx - w * 0.055, y: h * 0.68, width: w * 0.11, height: h * 0.22),
                          cornerRadius: w * 0.05), with: .color(color))    // grip
            ctx.fill(Path(ellipseIn: CGRect(x: cx - w * 0.09, y: h * 0.88, width: w * 0.18, height: h * 0.12)),
                     with: .color(color))                                  // pommel
        }
        .frame(width: size, height: size)
    }
}
