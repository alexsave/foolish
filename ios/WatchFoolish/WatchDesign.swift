// WatchDesign.swift — the watch's design language (docs/WATCHOS_LAYOUT.md §4.6.1). A
// true-black OLED canvas; the card GLYPH is the atom (a large suit shape with the rank
// knocked into it). Colour is reserved for game state and nothing else — see WColor.
// SF Rounded throughout.

import SwiftUI

enum WColor {
    static let bg    = Color.black                                   // pure black — OLED + always-on

    // Card glyph (§2 Card Glyph Component)
    static let suitBlack = Color(hex: 0xF2F2F4)                      // ♠ ♣ render near-white on black
    static let suitRed   = Color(hex: 0xE8352E)                      // ♥ ♦
    static let suitDim   = Color(hex: 0x6E6E73)                      // any suit, no action available
    static let inkOnBlack = Color(hex: 0x111111)                     // rank on a black-suit glyph
    static let inkOnRed   = Color.white                              // rank on a red-suit glyph

    // MARK: Player state — every colour means exactly one thing (owner review).
    //
    // There is no "you" colour: gold used to mean you, which made it the one hue that said
    // nothing about the game. You are marked by WEIGHT instead — your count is heavy, every
    // other seat's is light — which frees colour to carry state alone.
    //
    //   red    the opener, while the bout is still WAITING on their opening attack
    //   orange defender (also the shield)
    //   green  said GOOD (these counts are the vote tally)
    //   gray   escaped
    //   white  everyone else
    //
    // Red is transient by design: it clears to white the instant they attack, so it reads
    // as "we're waiting on them", not as a permanent role. Derived from public state —
    // `firstAttacker` + an empty table — because the bridge only exposes `awaitingAttack`
    // for the viewer's own seat (`ios_api.c:172`).
    //
    // Precedence: out ▸ defender ▸ good ▸ opening ▸ plain. (The defender can never say
    // GOOD, so orange and green cannot collide — `game.c:844`; and a seat that owes the
    // opening attack cannot have voted, so red and green cannot collide either —
    // `legal.c:377-384`.)
    static let attacker = Color(hex: 0xFF453A)                       // opener, pre-attack
    static let defender = Color(hex: 0xFF9F0A)                       // defender + shield
    static let green    = Color(hex: 0x30D158)                       // said GOOD
    static let out      = Color(white: 0.26)                         // escaped
    static let plain    = Color(hex: 0xF2F2F4)                       // everyone else

    // Non-state accents
    static let blue   = Color(hex: 0x0A84FF)                         // PASS ↑
    static let red    = Color(hex: 0xFF453A)                         // pickup ↓ / reject glow

    // Greys
    static let info   = Color(hex: 0xB8B8BE)                         // InfoLine
    static let seat   = Color(hex: 0x98989E)                         // captions
    static let arrow  = Color(hex: 0x5A5A5E)                         // cover▸attack arrow
    static let dim    = Color(white: 0.42)                           // secondary text
    static let faint  = Color(white: 0.22)                           // hairlines, inactive dots
}

extension Color {
    init(hex: UInt32) {
        self.init(red: Double((hex >> 16) & 0xFF) / 255,
                  green: Double((hex >> 8) & 0xFF) / 255,
                  blue: Double(hex & 0xFF) / 255)
    }
}

enum WFont {
    /// The smallest type in the app, and the ONE size for every chrome word: the header's
    /// column labels (FLIP/DECK/DISC) and every action verb, on the table and in the
    /// chooser alike (owner review). Plain system SF — no custom face.
    static let caption = Font.system(size: 7)

    static func token(_ pt: CGFloat) -> Font { .system(size: pt, weight: .bold, design: .rounded) }
    static func label(_ pt: CGFloat) -> Font { .system(size: pt, weight: .semibold, design: .rounded) }
    static func heavy(_ pt: CGFloat) -> Font { .system(size: pt, weight: .heavy, design: .rounded) }
}

/// SwiftUI color for a suit (from FoolishKit's Models.Suit). Presentation only.
extension Suit {
    var glyphColor: Color { isRed ? WColor.suitRed : WColor.suitBlack }
}

/// The card glyph — the atom of the whole watch UI (§2 Card Glyph Component). A large
/// suit glyph is the shape; the rank string is knocked into it at ~54 % height, heavy.
/// Rank is dark on a black suit, white on a red suit. `focused` draws the 1.25 pt white
/// outline; `dim` fades illegal / no-action items.
struct Glyph: View {
    let card: Card
    var size: CGFloat = 30
    var focused: Bool = false
    var dim: Bool = false


    private var suit: Suit { card.suit ?? .spades }

    /// Dimming desaturates the suit toward gray — it does NOT fade the whole glyph. Blanket
    /// opacity drags the suit AND the rank knocked out of it toward black together, so the
    /// two collapse into each other and the card loses its value (the exact failure §2 warns
    /// about for the table). Holding the suit at a mid gray keeps the rank readable at every
    /// ring of the lane.
    private var suitColor: Color { dim ? Color(white: HTuning.dimSuitWhite) : suit.glyphColor }
    private var rankColor: Color {
        if dim { return WColor.inkOnBlack }          // dark ink reads on the gray suit
        return suit.isRed ? WColor.inkOnRed : WColor.inkOnBlack
    }
    /// Suit glyphs sit high in their line box; nudge the rank onto each suit's optical
    /// centre (spade/club carry a stem below, so they want a touch more lift).
    private var nudge: CGFloat {
        switch suit {
        case .spades:   return -size * 0.05
        case .clubs:    return -size * 0.04
        case .hearts:   return -size * 0.01
        case .diamonds: return  0
        }
    }

    var body: some View {
        ZStack {
            Text(suit.glyph)
                .font(WFont.token(size * HTuning.glyphSuitScale))
                .foregroundStyle(suitColor)
            Text(CardRank.label(card.v))
                .font(.system(size: size * (card.v == 10 ? HTuning.glyphRank10Scale : HTuning.glyphRankScale),
                              weight: .heavy, design: .rounded))
                .foregroundStyle(rankColor)
                .monospacedDigit()
                // `nudge` is the per-suit optical correction; glyphRankY slides every rank
                // together (0.54 = today's centre, larger = lower).
                .offset(y: nudge + size * (HTuning.glyphRankY - 0.54))
        }
        .frame(width: size * HTuning.glyphFrameW, height: size * HTuning.glyphFrameH)  // roomy — wide ♥/♣ must not clip
        .overlay(
            RoundedRectangle(cornerRadius: size * 0.22, style: .continuous)
                .strokeBorder(.white, lineWidth: HTuning.focusRingWidth)
                .padding(1)
                .opacity(focused ? 1 : 0)
        )
    }
}

/// The defender's heater shield (§6). Spec reference path in a 40×48 viewBox:
///   M20 1 L38 8 L38 26 Q38 38 20 47 Q2 38 2 26 L2 8 Z
/// Rendered as a `Shape` scaled to the frame so it can stroke anywhere.
struct ShieldShape: Shape {
    func path(in rect: CGRect) -> Path {
        let sx = rect.width / 40, sy = rect.height / 48
        func p(_ x: CGFloat, _ y: CGFloat) -> CGPoint {
            CGPoint(x: rect.minX + x * sx, y: rect.minY + y * sy)
        }
        var path = Path()
        path.move(to: p(20, 1))
        path.addLine(to: p(38, 8))
        path.addLine(to: p(38, 26))
        path.addQuadCurve(to: p(20, 47), control: p(38, 38))
        path.addQuadCurve(to: p(2, 26),  control: p(2, 38))
        path.addLine(to: p(2, 8))
        path.closeSubpath()
        return path
    }
}

/// A minimal upward sword — used only on the New-game list. Drawn in a Canvas so it
/// takes the tint and reads as a sword (SF Symbols has none) even at chip size.
struct SwordIcon: View {
    var size: CGFloat = 16
    var color: Color = WColor.bg

    var body: some View {
        Canvas { ctx, sz in
            let w = sz.width, h = sz.height, cx = w / 2
            var blade = Path()
            blade.move(to: CGPoint(x: cx, y: 0))
            blade.addLine(to: CGPoint(x: cx + w * 0.09, y: h * 0.16))
            blade.addLine(to: CGPoint(x: cx + w * 0.09, y: h * 0.60))
            blade.addLine(to: CGPoint(x: cx - w * 0.09, y: h * 0.60))
            blade.addLine(to: CGPoint(x: cx - w * 0.09, y: h * 0.16))
            blade.closeSubpath()
            ctx.fill(blade, with: .color(color))
            ctx.fill(Path(roundedRect: CGRect(x: cx - w * 0.30, y: h * 0.58, width: w * 0.60, height: h * 0.10),
                          cornerRadius: h * 0.05), with: .color(color))
            ctx.fill(Path(roundedRect: CGRect(x: cx - w * 0.055, y: h * 0.68, width: w * 0.11, height: h * 0.22),
                          cornerRadius: w * 0.05), with: .color(color))
            ctx.fill(Path(ellipseIn: CGRect(x: cx - w * 0.09, y: h * 0.88, width: w * 0.18, height: h * 0.12)),
                     with: .color(color))
        }
        .frame(width: size, height: size)
    }
}
