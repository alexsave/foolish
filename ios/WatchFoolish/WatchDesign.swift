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
    /// Suit glyphs sit high in their line box; nudge the value onto each suit's
    /// visual center (spade/club have a stem below, so they need more lift).
    private var nudge: CGFloat {
        switch card.suit {
        case .spades:   return -size * 0.05
        case .clubs:    return -size * 0.04
        case .hearts:   return -size * 0.01
        case .diamonds: return  size * 0.00
        }
    }

    var body: some View {
        ZStack {
            Text(card.suit.glyph)
                .font(WFont.token(size * 1.4))
                .foregroundStyle(card.suit.color)
            Text(card.label)
                .font(.system(size: size * (card.value == 10 ? 0.5 : 0.58), weight: .heavy, design: .rounded))
                .foregroundStyle(valueColor)
                .monospacedDigit()
                .offset(y: nudge)
        }
        .frame(width: size * 1.55, height: size * 1.5)   // generous — the wide ♥/♣ must not clip
        .background(
            RoundedRectangle(cornerRadius: 7, style: .continuous)
                .fill(selected ? Color(white: 0.18) : .clear)
        )
        .overlay(
            RoundedRectangle(cornerRadius: 7, style: .continuous)
                .strokeBorder(selected ? WColor.ink : .clear, lineWidth: 1.5)
        )
    }
}

/// A shield outline for the defender seat (§ owner request — a shield, not a
/// circle): rounded top, straight shoulders, curving to a point at the bottom.
struct ShieldShape: Shape {
    func path(in rect: CGRect) -> Path {
        var p = Path()
        let r = rect.width * 0.22
        p.move(to: CGPoint(x: rect.minX, y: rect.minY + r))
        p.addQuadCurve(to: CGPoint(x: rect.minX + r, y: rect.minY), control: CGPoint(x: rect.minX, y: rect.minY))
        p.addLine(to: CGPoint(x: rect.maxX - r, y: rect.minY))
        p.addQuadCurve(to: CGPoint(x: rect.maxX, y: rect.minY + r), control: CGPoint(x: rect.maxX, y: rect.minY))
        p.addLine(to: CGPoint(x: rect.maxX, y: rect.minY + rect.height * 0.42))
        p.addQuadCurve(to: CGPoint(x: rect.midX, y: rect.maxY),
                       control: CGPoint(x: rect.maxX, y: rect.maxY - rect.height * 0.12))
        p.addQuadCurve(to: CGPoint(x: rect.minX, y: rect.minY + rect.height * 0.42),
                       control: CGPoint(x: rect.minX, y: rect.maxY - rect.height * 0.12))
        p.closeSubpath()
        return p
    }
}

/// Text arced over the top of a circle (§ owner request — usernames curve around
/// the seat). Each character is placed on the arc of `radius` and rotated tangent,
/// spread symmetrically about 12 o'clock.
struct ArcText: View {
    let text: String
    var radius: CGFloat = 18
    var color: Color = WColor.dim
    var fontSize: CGFloat = 9
    var step: Double = 20        // degrees between characters

    var body: some View {
        let chars = Array(text.prefix(9))
        let n = chars.count
        return ZStack {
            ForEach(0..<n, id: \.self) { i in
                let deg = (Double(i) - Double(n - 1) / 2) * step
                let rad = deg * .pi / 180
                Text(String(chars[i]))
                    .font(.system(size: fontSize, weight: .semibold, design: .rounded))
                    .foregroundStyle(color)
                    .rotationEffect(.degrees(deg))
                    .offset(x: radius * CGFloat(sin(rad)), y: -radius * CGFloat(cos(rad)))
            }
        }
        .frame(width: radius * 2 + fontSize * 2, height: radius * 2 + fontSize * 2)
    }
}

/// Player names arced along the big seat ring (§ owner request): each name spans
/// a small arc centered on its seat's angle, just outside the seat circle. Names
/// on the bottom half flip so they stay upright. Placed once at the ring center.
struct RingNames: View {
    struct Item: Identifiable { let id: Int; let angle: Double; let name: String; let color: Color }
    let items: [Item]
    let radius: CGFloat
    var fontSize: CGFloat = 9
    var step: Double = 0.17        // radians between characters

    var body: some View {
        ZStack {
            ForEach(items) { item in
                let chars = Array(item.name.prefix(9))
                let n = chars.count
                let flipped = sin(item.angle) > 0.05        // lower half → read upright
                ForEach(0..<n, id: \.self) { i in
                    let delta = (Double(i) - Double(n - 1) / 2) * step
                    let a = flipped ? item.angle - delta : item.angle + delta
                    Text(String(chars[i]))
                        .font(.system(size: fontSize, weight: .semibold, design: .rounded))
                        .foregroundStyle(item.color)
                        .rotationEffect(.radians(flipped ? a - .pi / 2 : a + .pi / 2))
                        .offset(x: radius * CGFloat(cos(a)), y: radius * CGFloat(sin(a)))
                }
            }
        }
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
