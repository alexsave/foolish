// FDeckWell.swift — the stock, matching the web's DeckAndFlipped
// (src/components/GameDisplay/DeckAndFlipped.tsx): a leaning stack of landscape
// card backs with the remaining count centred ON the pile, and the flipped
// trump hanging below it (upright, tucked under the stack). When the stock and
// the flipped card are gone, the bare trump-suit glyph takes their place.
//
// Web-layout parity per IOS_APP_DESIGN §17.10.

import SwiftUI

public struct FDeckWell: View {
    /// Only the bare trump glyph below needs it: a spade or club drawn in the
    /// light board's near-black ink disappears into a dark weave.
    @Environment(\.colorScheme) private var scheme
    public let deckCount: Int
    public let flipped: Card?
    public let hasFlipped: Bool
    public let trumpSuit: Suit?
    public var backSeed: UInt64

    public init(deckCount: Int, flipped: Card?, hasFlipped: Bool, trumpSuit: Suit?, backSeed: UInt64 = 42) {
        self.deckCount = deckCount
        self.flipped = flipped
        self.hasFlipped = hasFlipped
        self.trumpSuit = trumpSuit
        self.backSeed = backSeed
    }

    // The badge counts the flipped card too (web badgeTotal = deck + flipped).
    private var badgeTotal: Int { deckCount + ((hasFlipped && flipped != nil) ? 1 : 0) }
    private var stackLayers: Int { min(max(deckCount, 0), 6) }

    /// Every state anchors to the SAME top-leading inset (note 14: equal left/top
    /// distance from the board's edges), instead of the old centred layout that
    /// needed a per-call-site magic offset tuned only for the stacked state.
    private let inset: CGFloat = FSpace.s
    /// How far the flipped trump peeks out below the stack when both are showing
    /// (the old centred layout's `offset(y: 34)`, now relative to the shared
    /// top-leading anchor instead of the container's centre). Tuned so roughly a
    /// third of the flipped card's own height (66pt) tucks UNDER the stock's
    /// bottom card rather than just touching it edge-to-edge — the stock's
    /// rotated bottom card reaches down to `inset + cardW` (owner review,
    /// batch 11/note 1): overlap = (inset + cardW) - (inset + peek) =
    /// cardW - peek, so peek=20 hides cardW-20=26pt (~39%) of the flipped card.
    private let peek: CGFloat = 20
    /// One stock card, portrait, before it is laid landscape in the stack.
    private let cardW: CGFloat = 46
    private let cardH: CGFloat = 66
    /// `deckStack`'s cards are rotated 90°, so a `cardW`×`cardH` portrait box
    /// reads as `cardH`×`cardW` landscape on screen. `.rotationEffect` pivots on
    /// the view's own centre WITHOUT changing its reported (pre-rotation) layout
    /// size, so lining up the ROTATED visual edge with a target point needs the
    /// pre-rotation offset nudged by half the width/height that rotation swaps —
    /// this is that nudge, shared by every rotated-card calculation below (the
    /// stack's own bottom-card anchor, and note 1's flipped-card centring under
    /// it, both reduce to the same `(cardH - cardW) / 2`).
    private var rotationNudge: CGFloat { (cardH - cardW) / 2 }
    /// The landscape stock's visual width once rotated — what note 1 centres the
    /// flipped card under.
    private var stackVisualWidth: CGFloat { cardH }

    // The two anchors round-4 note 6 pins down, as STATICS so a test can assert
    // they are constants and not functions of `deckCount` — the whole content of
    // "position should be constant relative to the top left corner throughout
    // the game" is that neither of these may ever take the count as an input.
    // (Both are in FDeckWell's own top-leading space, i.e. relative to the same
    // corner the owner is measuring from.)

    /// Where the flipped trump's top-left corner sits, in every state.
    public static let flippedOrigin = CGPoint(x: FSpace.s + (66 - 46) / 2, y: FSpace.s + 20)
    /// Where the stock's BOTTOM card's rotated top-left corner sits, in every
    /// state. Cards above it lean up-and-left off this one fixed card; it never
    /// moves, so a shrinking deck drains toward this corner instead of sliding.
    public static let bottomCardOrigin = CGPoint(x: FSpace.s, y: FSpace.s)

    /// The bare trump mark's glyph size (round-5 m1 raised it from 44).
    static let markSize: CGFloat = 60

    /// Where `suit`'s INK begins inside its own text box, at `size`.
    ///
    /// Round 16 ("the trump indicator is a bit low; the distance from top and
    /// the distance from left should be equal; play around with this for
    /// different suits as they can be funny"). A `Text` lays out a LINE, not a
    /// glyph: its box carries the font's whole ascent above the ink and the
    /// glyph's left side bearing beside it. Offsetting that box by the shared
    /// inset therefore left the mark 9.7pt from the left and 27.7pt from the
    /// top - and since Georgia gives each suit its own ink height and bearing,
    /// each of the four was wrong by its own amount (measured: ♠ 9.67/27.67,
    /// ♥ 10.00/27.00, ♣ 9.67/27.00, ♦ 10.00/27.33). Subtracting this puts the
    /// INK on the inset, so all four sit square in the corner.
    ///
    /// Read off the real font rather than baked as four constants, so the mark
    /// stays square if the size or the typeface ever changes; the numbers it
    /// produces are pinned against the rendered pixels in TrumpGlyphTests.
    static func markInkOrigin(_ suit: Suit, size: CGFloat = markSize) -> CGPoint {
        let base = UIFont(name: "Georgia", size: size) ?? .systemFont(ofSize: size)
        let bold = base.fontDescriptor.withSymbolicTraits(.traitBold) ?? base.fontDescriptor
        let asked = UIFont(descriptor: bold, size: size)
        // Georgia has no suit glyphs of its own, so what actually DRAWS here is
        // whatever CoreText substitutes for the character. Measuring the asked-
        // for font instead simply fails (no glyph), which is how the first cut
        // of this silently changed nothing at all.
        var chars = Array(suit.glyph.utf16)
        let font = CTFontCreateForString(asked, suit.glyph as CFString,
                                         CFRange(location: 0, length: chars.count))
        var glyphs = [CGGlyph](repeating: 0, count: chars.count)
        guard CTFontGetGlyphsForCharacters(font, &chars, &glyphs, chars.count) else { return .zero }
        let ink = CTFontGetBoundingRectsForGlyphs(font, .horizontal, &glyphs, nil, glyphs.count)
        // CoreText measures ink from the baseline, y UP; a Text box measures
        // from its top, y DOWN, with the baseline one ascent below that top.
        // The line's ascent is the ASKED-FOR font's (Georgia sets the line box;
        // the substitute only fills it), which is why this pairs `asked` with
        // the substitute's ink rather than taking both from one of them.
        return CGPoint(x: ink.minX, y: asked.ascender - ink.maxY)
    }

    public var body: some View {
        ZStack(alignment: .topLeading) {
            // The flipped trump: tucked under the stack (peeking out below) when
            // the stock is still there, or — once the stock is drawn out — the
            // sole piece of content, flush at the same inset as everything else.
            //
            // Note 1: the flipped card sits CENTRED under the stock's landscape
            // footprint, the way the remaining-count badge is centred ON it —
            // not flush-left against the shared inset like the trump glyph.
            //
            // Round 4 note 6: that position is now UNCONDITIONAL. It used to
            // drop back to the plain inset the moment the stock emptied, which
            // meant the flipped card visibly hopped 10pt left and 20pt up on
            // the draw that took the deck to zero - "the flipped card jumps
            // slightly when the deck finishes; its position should be constant
            // relative to the top left corner throughout the game." Nothing
            // about where the flipped card LIVES depends on how many cards are
            // left above it, so nothing here reads deckCount any more.
            if hasFlipped, let flipped {
                FCard(card: flipped, trump: true, size: CGSize(width: cardW, height: cardH))
                    .offset(x: Self.flippedOrigin.x, y: Self.flippedOrigin.y)
                    .zIndex(0)
            }

            if deckCount > 0 {
                deckStack.zIndex(1)
            } else if !hasFlipped, let trumpSuit {
                // Stock and flip both gone — the bare suit glyph is now the
                // ONLY trump indicator left on the board (round-5 m1: "bare
                // glyph can be bigger" — at the old 44pt it was the sliver-of-
                // a-card problem all over again, just unlabeled). 44 → 60,
                // still well inside the 92×108 well at the same shared inset,
                // plus a dark shadow so a light suit colour still holds on
                // the wool weave behind it (the same contrast problem M10
                // names for text applies to a lone glyph too).
                // GEORGIA, like every suit on a card face (FCard.centerGlyph /
                // corner / thinCenter). This was the board's one suit drawn in
                // the SYSTEM font, and the two typefaces do not draw the same
                // shape - SF's heart is narrow and straight-shouldered where
                // Georgia's is round and full - so the trump mark and the trump
                // cards under it read as two different suits ("upper right trump
                // suit icon should match shape of card suits icon"). Same size
                // as before; only the face changes.
                let ink = Self.markInkOrigin(trumpSuit)
                Text(trumpSuit.glyph)
                    .font(.custom("Georgia", size: Self.markSize).weight(.bold))
                    .foregroundColor(FColor.suitColor(trumpSuit, scheme: scheme))
                    .shadow(color: .black.opacity(0.6), radius: 2, x: 0, y: 1)
                    // Round 16: inset the glyph's INK, not its text box - see
                    // `markInkOrigin`. Everything else in this corner already
                    // anchors on the ink/edge it looks like it anchors on.
                    .offset(x: inset - ink.x, y: inset - ink.y)
            }
        }
        .frame(width: 92, height: 108, alignment: .topLeading)
        // Publish the deck's rect so draw flights (deck→hand) have a source.
        .background(GeometryReader { g in
            Color.clear.preference(key: DeckFrameKey.self, value: g.frame(in: .named(boardSpace)))
        })
        .accessibilityElement(children: .ignore)
        // Round-5 m2: was a hard-coded English sentence — every visible string
        // in the app goes through FStrings, this label didn't.
        .accessibilityLabel(FStrings.t("ios.a11y.deck", ["n": "\(deckCount)"]) +
            (trumpSuit != nil ? ", " + FStrings.t("ios.a11y.trump", ["suit": FStrings.spokenSuit(trumpSuit!)]) : ""))
    }

    /// The leaning stock: cards fan up-and-left from a FIXED bottom card (i=0,
    /// unshifted), not centred in a box sized for the tallest fan. Notes 1/10/14
    /// (and the live clarification that prompted this rewrite): the anchor
    /// point that must sit flush at the shared (inset, inset) — same as every
    /// other FDeckWell state — is the BOTTOM card's own rotated top-left corner,
    /// not some union-of-all-layers bounding box. A single-card stock (deckCount
    /// == 1) is the clearest test of that: it should read exactly like the
    /// flipped-only or trump-glyph-only states, flush at the inset, with equal
    /// top and left margins — the old box-centred layout instead put it ~6pt
    /// right and ~7pt down of that shared anchor. Cards above the bottom one
    /// are free to fan up and left past the nominal frame edge (by design — see
    /// the per-layer `-i`/`-i*2` stagger below); they were never meant to nestle
    /// neatly inside a box, only to lean off the one fixed card underneath them.
    private var deckStack: some View {
        ZStack(alignment: .topLeading) {
            ForEach(0..<stackLayers, id: \.self) { i in
                FCard(card: nil, backSeed: backSeed, size: CGSize(width: cardW, height: cardH))
                    .rotationEffect(.degrees(90))
                    // rotationNudge un-does the rotation's size swap so i=0 (no
                    // further stagger) lands its rotated top-left exactly on
                    // (inset, inset); i>0 then leans up-left from that fixed card.
                    .offset(x: inset + rotationNudge - CGFloat(i),
                            y: inset - rotationNudge - CGFloat(i * 2))
            }
            // Badge: centred over the BOTTOM card's own rotated footprint — a
            // `stackVisualWidth`×`cardW` (66×46) box flush at the same
            // (inset, inset) anchor the bottom card itself is flush at — riding
            // up with `-stackLayers` as more cards stack on top of it. Wrapping
            // in a same-sized box and letting ITS default `.center` alignment
            // place the digits avoids having to know the rendered text size up
            // front (exactly what the old box-centred layout got for free; this
            // box is just re-sized and re-anchored to the real bottom card
            // instead of the old artificial 78×60 union box).
            // Round-5 m9: this numeral used to float bare on the stock's own
            // red card backs — a white digit on saturated red reads as an iOS
            // unread badge for what is neutral count info. `FCountChip` backs
            // it with a near-black fill + the card backs' subdued edge red.
            ZStack {
                FCountChip("\(badgeTotal)", font: .system(size: 17, weight: .bold))
            }
            .frame(width: stackVisualWidth, height: cardW)
            .offset(x: inset, y: inset - CGFloat(stackLayers))
        }
    }
}
