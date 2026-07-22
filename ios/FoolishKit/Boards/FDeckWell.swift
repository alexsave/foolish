// FDeckWell.swift — the stock, matching the web's DeckAndFlipped
// (src/components/GameDisplay/DeckAndFlipped.tsx): a leaning stack of landscape
// card backs with the remaining count centred ON the pile, and the flipped
// trump hanging below it (upright, tucked under the stack). When the stock and
// the flipped card are gone, the bare trump-suit glyph takes their place.
//
// Web-layout parity per IOS_APP_DESIGN §17.10.

import SwiftUI

public struct FDeckWell: View {
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

    public var body: some View {
        ZStack(alignment: .topLeading) {
            // The flipped trump: tucked under the stack (peeking out below) when
            // the stock is still there, or — once the stock is drawn out — the
            // sole piece of content, flush at the same inset as everything else.
            //
            // Note 1: while the stock is showing, the flipped card must sit
            // CENTRED under it, the way the remaining-count badge is centred ON
            // it — not flush-left against the shared inset like every other
            // state here. The stock's landscape footprint is `stackVisualWidth`
            // (66pt) wide; the flipped card is `cardW` (46pt) wide, so centring
            // it needs an extra (66-46)/2 = 10pt beyond the shared inset — same
            // number as `rotationNudge`, since both are "half the width a
            // rotated card gained." Once the stock empties (deckCount == 0)
            // there is no stack to centre under, so the flipped card drops back
            // to the plain shared inset like the trump glyph fallback below it —
            // this centring is conditional on deckCount, not a permanent shift
            // of the flipped card's home position.
            if hasFlipped, let flipped {
                let centerUnderStack = deckCount > 0 ? (stackVisualWidth - cardW) / 2 : 0
                FCard(card: flipped, trump: true, size: CGSize(width: cardW, height: cardH))
                    .offset(x: inset + centerUnderStack, y: inset + (deckCount > 0 ? peek : 0))
                    .zIndex(0)
            }

            if deckCount > 0 {
                deckStack.zIndex(1)
            } else if !hasFlipped, let trumpSuit {
                // Stock and flip both gone — show the trump suit glyph.
                Text(trumpSuit.glyph)
                    .font(.system(size: 44))
                    .foregroundColor(FColor.suitColor(trumpSuit))
                    .offset(x: inset, y: inset)
            }
        }
        .frame(width: 92, height: 108, alignment: .topLeading)
        // Publish the deck's rect so draw flights (deck→hand) have a source.
        .background(GeometryReader { g in
            Color.clear.preference(key: DeckFrameKey.self, value: g.frame(in: .named(boardSpace)))
        })
        .accessibilityElement(children: .ignore)
        .accessibilityLabel("\(deckCount) cards left in the deck" +
            (trumpSuit != nil ? ", trump \(["spades","hearts","clubs","diamonds"][trumpSuit!.rawValue])" : ""))
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
            ZStack {
                Text("\(badgeTotal)")
                    .font(.system(size: 17, weight: .bold))
                    .foregroundColor(.white)
                    .shadow(color: .black.opacity(0.8), radius: 1, x: 1, y: 1)
            }
            .frame(width: stackVisualWidth, height: cardW)
            .offset(x: inset, y: inset - CGFloat(stackLayers))
        }
    }
}
