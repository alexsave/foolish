// FCard.swift — a single card, matching the WEB client's CardFace
// (src/components/GameDisplay/CardFace.tsx): a stylized minimalist card — a white
// rectangle with a 2px black border, two corner indices (rank over suit) and one
// large centre suit glyph in Georgia serif. Red suits are red, black suits black.
// Selection recolors the border RED and does not lift or scale (web parity). The
// dragged source card fades to 0.3. Trump is NOT marked per-card (the web shows
// trump only at the deck well); the `trump` flag is kept for API compatibility.
// Back is the procedural fern; VoiceOver reads "seven of spades" etc.
//
// DARK MODE inverts the face and nothing else (round-7, the owner's brief:
// "make cards black with red suit and value for hearts and diamonds, white
// otherwise. White outline"). Geometry, type, sizes, the thin-card fallback and
// the selection behaviour are all scheme-independent — only three colours move,
// and they move as a PAIR of ramps in `Ink` below so a dark card cannot end up
// with, say, a light-mode border.

import SwiftUI

public struct FCard: View {
    /// Read once, at the top of the card, and threaded into `ink` — the face,
    /// the glyphs and the border must agree, and three separate environment
    /// reads is three chances for them not to.
    @Environment(\.colorScheme) private var scheme
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
    /// Round-7 #3: the dark-mode outline. The owner's round-6 spec said white;
    /// round-7 revised it to "dark gray". Deliberately darker than the dark
    /// wool field (~0x5D5D62) so a black card still has a visible edge against
    /// the board, and lighter than the 0x0A0A0A face so the outline reads at
    /// all - a gray that matched either surface would erase one of the two
    /// edges. Tunable in one place if it wants nudging.
    private static let darkBorder = Color(hex: 0x3E3E44)

    /// The three colours a card face is made of, as ONE value per scheme.
    ///
    /// Light is the web's card, unchanged. Dark is its inversion: the ink
    /// colour becomes the FACE and white stands in for the black suits, which
    /// is precisely what "black cards, white otherwise, white outline" asks
    /// for. `red` brightens a step in dark mode (#DC2626 → #EF4444) because
    /// the web's red was chosen against a white face - on black it is a
    /// 4.4:1 muddy maroon, where the brighter red is 5.6:1 and still reads as
    /// the same red at a glance beside a light-mode card in the transcript.
    private struct Ink {
        let face: Color
        let red: Color      // hearts + diamonds
        let black: Color    // clubs + spades (white, in dark mode)
        let border: Color

        static let light = Ink(face: FCard.faceWhite, red: FCard.redSuit,
                               black: FCard.blackSuit, border: FCard.blackSuit)
        static let dark = Ink(face: FCard.blackSuit, red: Color(hex: 0xEF4444),
                              black: .white, border: FCard.darkBorder)
    }

    private var ink: Ink { scheme == .dark ? .dark : .light }

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
        let color = suit.isRed ? ink.red : ink.black
        RoundedRectangle(cornerRadius: radius)
            .fill(ink.face)
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

    // The outline. Selection still recolors it red in BOTH schemes: red on a
    // black face against a walnut board is the only saturated thing on the
    // card, so it still reads as "this one is picked" - and keeping the
    // selected colour scheme-independent means the one affordance a player
    // hunts for during a drag does not change identity when they toggle
    // appearance mid-game.
    private var border: some View {
        RoundedRectangle(cornerRadius: radius)
            .strokeBorder(selected ? Self.selRed : ink.border,
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
