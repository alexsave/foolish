// FCard.swift — a single card, matching the WEB client's CardFace
// (src/components/GameDisplay/CardFace.tsx): a stylized minimalist card — a white
// rectangle with a 2px black border, two corner indices (rank over suit) and one
// large centre suit glyph in Georgia serif. Red suits are red, black suits black.
// Selection recolors the border RED and does not lift or scale (web parity). The
// dragged source card fades to 0.3. Trump is NOT marked per-card (the web shows
// trump only at the deck well); the `trump` flag is kept for API compatibility.
// Back is the procedural fern; VoiceOver reads "seven of spades" etc.

import SwiftUI

public struct FCard: View {
    public let card: Card?          // nil ⇒ face-down (use `backSeed`)
    public var selected: Bool
    public var disabled: Bool       // dimmed + locked (C1 in-flight affordance)
    public var dragging: Bool       // the source card while a drag is in flight (web: opacity 0.3)
    public var trump: Bool          // kept for API; web marks trump at the deck, not per card
    public var backSeed: UInt64
    public var size: CGSize

    public init(card: Card?, selected: Bool = false, disabled: Bool = false,
                dragging: Bool = false, trump: Bool = false, backSeed: UInt64 = 1,
                size: CGSize = CGSize(width: 50, height: 70)) {
        self.card = card
        self.selected = selected
        self.disabled = disabled
        self.dragging = dragging
        self.trump = trump
        self.backSeed = backSeed
        self.size = size
    }

    // Web tokens (variables.css): white face, red #dc2626 / black suits, black
    // border, selected border pure red, 5px radius (relative here so small cards
    // stay proportional).
    private static let faceWhite = Color.white
    private static let redSuit = Color(hex: 0xDC2626)
    private static let blackSuit = Color(hex: 0x0A0A0A)
    private static let selRed = Color(hex: 0xE0201C)
    private var radius: CGFloat { min(5, size.width * 0.1) }
    private var thin: Bool { size.width < 40 }   // web thin-card fallback (<40px wide)

    public var body: some View {
        Group {
            if let card, !card.isHidden {
                face(card)
            } else {
                back
            }
        }
        .frame(width: size.width, height: size.height)
        .opacity(disabled ? 0.5 : (dragging ? 0.3 : 1))
        .accessibilityElement()
        .accessibilityLabel(a11yLabel)
        .accessibilityAddTraits(selected ? .isSelected : [])
    }

    // MARK: face

    @ViewBuilder private func face(_ card: Card) -> some View {
        let suit = card.suit ?? .spades
        let color = suit.isRed ? Self.redSuit : Self.blackSuit
        RoundedRectangle(cornerRadius: radius)
            .fill(Self.faceWhite)
            .overlay { if thin { thinCenter(card, color: color) } else { centerGlyph(suit, color: color) } }
            .overlay(alignment: .topLeading) { if !thin { corner(card, suit: suit, color: color) } }
            .overlay(alignment: .bottomTrailing) {
                if !thin { corner(card, suit: suit, color: color).rotationEffect(.degrees(180)) }
            }
            .overlay(border)
    }

    // Big centre suit glyph — 32/50 of the width (web parity).
    private func centerGlyph(_ suit: Suit, color: Color) -> some View {
        Text(suit.glyph)
            .font(.custom("Georgia", size: size.width * 0.62).weight(.bold))
            .foregroundColor(color)
    }

    // Corner index: rank (20/50 w) over suit (14/50 w), Georgia bold (web parity).
    // Leading-aligned so the suit's left edge tracks the rank's left edge instead
    // of centring under it — a wide rank ("10") no longer shifts the suit glyph
    // sideways (standard card-index behavior). The bottom-right corner mirrors
    // correctly since it rotates this whole VStack 180°.
    private func corner(_ card: Card, suit: Suit, color: Color) -> some View {
        VStack(alignment: .leading, spacing: -size.width * 0.04) {
            Text(CardRank.label(card.v))
                .font(.custom("Georgia", size: size.width * 0.40).weight(.bold))
            Text(suit.glyph)
                .font(.custom("Georgia", size: size.width * 0.28).weight(.bold))
        }
        .foregroundColor(color)
        .padding(.leading, size.width * 0.08)
        .padding(.top, size.width * 0.04)
        .fixedSize()
    }

    // Thin fallback (<40px): drop the corners, show a centred rank+suit stack.
    // ONLY here is the glyph enlarged over the web - the compressed rank/suit is a
    // known legibility miss on the web too (note left in src/.../CardFace.tsx), so
    // a narrow card reads bigger while normal cards stay pixel-for-pixel web-parity.
    private func thinCenter(_ card: Card, color: Color) -> some View {
        VStack(spacing: 0) {
            Text(CardRank.label(card.v)).font(.custom("Georgia", size: size.width * 0.56).weight(.bold))
            Text((card.suit ?? .spades).glyph).font(.custom("Georgia", size: size.width * 0.56).weight(.bold))
        }
        .foregroundColor(color)
    }

    private var border: some View {
        RoundedRectangle(cornerRadius: radius)
            .strokeBorder(selected ? Self.selRed : Self.blackSuit,
                          lineWidth: selected ? 2.5 : 2)
    }

    // MARK: back — dark-red fill with a lighter-red border (fern dropped for now;
    // it kept rendering wrong). A clean placeholder per the owner.
    //
    // `fileprivate` (not `private`) so `FCountChip` below — a different type in
    // this same file — can match these exactly rather than guessing a second
    // pair of hex values that could drift from the real card back.
    fileprivate static let backFill = Color(hex: 0x8B0000)     // dark red
    fileprivate static let backEdge = Color(hex: 0xDC2626)     // lighter red
    private var back: some View {
        RoundedRectangle(cornerRadius: radius)
            .fill(Self.backFill)
            .overlay(
                RoundedRectangle(cornerRadius: radius)
                    .strokeBorder(selected ? Self.selRed : Self.backEdge, lineWidth: selected ? 2.5 : 1.5)
            )
    }

    private var a11yLabel: String {
        guard let card, !card.isHidden, let suit = card.suit else { return FStrings.t("ios.a11y.facedown") }
        return FStrings.spokenCard(card.v, suit) + (trump ? ", " + FStrings.t("ios.a11y.trumpmark") : "")
    }
}

/// THE deck/hand/discard count numeral — one style, three call sites
/// (`FDeckWell`, `FSeatBadge`, `FDiscardPile`), so a count can never look like
/// two different things on one board.
///
/// Round-5 m9 first asked for a dark plate behind the digit ("give them black
/// backgrounds like the cards in hand") because a bare white numeral on the
/// saturated red backs read like an iOS unread badge. Rendered on device the
/// plate was worse than the problem — the owner's call on seeing it: "drop the
/// black background for the card counts, it doesn't look good at all." So the
/// numeral is bare again, and the badge-y read is answered by WEIGHT and a
/// hard shadow instead of a plate: heavy white digits carry on the card backs
/// without borrowing the shape of a notification.
public struct FCountChip: View {
    let text: String
    let font: Font
    public init(_ text: String, font: Font = .system(size: 15, weight: .bold)) {
        self.text = text
        self.font = font
    }
    public var body: some View {
        Text(text)
            .font(font)
            .foregroundColor(.white)
            // A tight, opaque shadow (not a soft glow): it separates the digit
            // from the red beneath it at every size the three call sites use.
            .shadow(color: .black.opacity(0.85), radius: 1.5, x: 0, y: 1)
    }
}
