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
    /// THE TRUMP SLOT, IN TWO PARTS AND THREE STATES. `hasFlipped` is "the
    /// flipped trump still EXISTS"; `flipped` is "and here it is, in the well".
    /// The pair therefore says one of three things, and the body draws a
    /// different thing for each:
    ///
    ///   hasFlipped, flipped = card   the trump is under the stock -> draw it
    ///   hasFlipped, flipped = nil    it has left the stock and is IN THE AIR
    ///                                -> draw nothing; the flight layer has it
    ///   !hasFlipped                  it landed somewhere -> the bare glyph
    ///
    /// The middle state is the one 1.1(55) needed and the pair could not say.
    /// The count and the trump do not change hands at the same MOMENT: a stock
    /// releases its cards at DEPARTURE (a card in the air is no longer in the
    /// pile, so the badge drops and the pile thins as it leaves), but the bare
    /// glyph means "there is no trump card anywhere any more" and can only be
    /// true once the card has LANDED. Drawing the glyph while the card is still
    /// flying renders the same card twice - once as itself, once as its own
    /// absence. Owner, 1.1(55): "if there still is a flipped card on board,
    /// don't show Trump indicator yet! Only after flipped is gone do you show
    /// it."
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
    private var stackLayers: Int { Self.layers(for: deckCount) }

    /// THE LEAN: how far each card above the bottom one steps left and up.
    /// Named rather than inlined because `maxLayers`' bound is derived FROM
    /// them - a change here changes how many layers fit, and the test that
    /// pins the ceiling reads these two so it cannot go on agreeing.
    static let leanX: CGFloat = 1
    static let leanY: CGFloat = 2

    /// THE HIGHEST THE PILE MAY LEAN, and the geometry that decides it.
    ///
    /// The stack's bottom card is pinned at the shared inset and every card
    /// above it steps 1pt left and 2pt up (see `deckStack`), so layer `i` puts
    /// its own top-left corner at (16 - i, 22 - 2i) measured from the BOARD's
    /// top-left corner - 8pt of board padding plus this well's own 8pt inset,
    /// 14pt of board padding plus the same 8 going down. The board surface is a
    /// rounded rectangle of radius 30 (measured off a rendered frame, twice,
    /// exactly), so near that corner the space runs out along an arc centred at
    /// (30, 30). A card's own corner is rounded too (`FCard.radius`, 4.6pt at
    /// this size), so the card fits while the centre of ITS corner - at
    /// (20.6 - i, 26.6 - 2i) - stays within 30 - 4.6 = 25.4pt of (30, 30):
    ///
    ///     i = 5  ->  19.7pt   (what shipped: 5.7pt of slack)
    ///     i = 6  ->  21.8pt   (3.6pt)
    ///     i = 7  ->  23.9pt   (1.5pt)
    ///     i = 8  ->  26.1pt   OVER - the top card's corner crosses the board's
    ///
    /// So eight layers is the ceiling, and it is a ceiling on the PICTURE, not
    /// on the count: `layers(for:)` maps the count onto it.
    static let maxLayers = 8

    /// HOW MANY CARD BACKS A STOCK OF `n` DRAWS.
    ///
    /// It used to be `min(n, 6)`, which is a straight line for the last six
    /// cards of the game and a flat line for every card before them. A 2-player
    /// deal leaves 23 in the stock, so the pile drew six leaning cards from 23
    /// down to 6 and only then began to shrink - the whole game at one
    /// thickness. The owner, 1.1(55): "the deck visual does not change as it
    /// deals? seems to be an identical amount of cards diagonally spaced no
    /// matter what the number says. the 'height' of the deck should correspond
    /// to the number of cards in the deck".
    ///
    /// Eight layers cannot resolve twenty-three cards, so the curve spends them
    /// where they are read:
    ///
    ///   * 0...6 EXACTLY, one layer per card. This is the endgame, and the end
    ///     of the game is the moment a player counts: one card must look like
    ///     one card, two like two. (Six is a full hand - "can everyone still
    ///     refill?" is a question about small numbers.)
    ///   * 7...11 -> 7, 12 and up -> 8. Nobody counts fifteen leaning cards;
    ///     what a deep stock has to say is "thick", and what it must never do
    ///     is contradict the badge, which it cannot - the pile is an
    ///     approximation of a number, never a different number.
    ///
    /// Monotonic by construction, so the pile can only ever thin as the deck
    /// drains. Nine distinct thicknesses over a game where there used to be
    /// two. Which counts share one:
    ///
    ///     n      0  1  2  3  4  5  6  7..11  12+
    ///     layers 0  1  2  3  4  5  6    7     8
    ///
    /// THE PICTURE IS DRIVEN BY THE SAME HELD COUNT THE BADGE IS. `deckCount`
    /// is whatever `MessageTableView.shownDeckCount` says - the count-freeze
    /// while a sequence opens, then each step's own board as its cards LEAVE
    /// the stock - so the two renderings of one number step down together and
    /// cannot disagree by a frame. (`FSeatBadge` learned the same lesson one
    /// component over: a badge reading 11 over six visible card backs.)
    static func layers(for n: Int) -> Int {
        let deck = max(n, 0)
        if deck <= 6 { return deck }
        return deck <= 11 ? 7 : maxLayers
    }

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

    #if DEBUG
    /// WHAT THE WELL IS ACTUALLY DRAWING, on every paint that changes it.
    ///
    /// The pixels cannot answer this at the one instant that matters. A dealt
    /// card is spawned ON TOP of the stock (correctly - that is where it comes
    /// from), so it covers the badge in exactly the frames where the badge and
    /// the pile have to be shown agreeing with each other. Rather than move the
    /// card or thin the well to suit the instrument, the well says out loud what
    /// it drew, and the frames corroborate it a few frames later once the flight
    /// has cleared. (The one time this repo instrumented the other way round it
    /// built a rig oracle whose baseline hid the defect it existed to catch.)
    ///
    /// `badge` is the number on the chip, `layers` the number of leaning card
    /// backs under it, `flipped` the trump identity the well is drawing beneath
    /// the stock (or `-`). Deduped, so a run of identical paints is one line.
    private static var lastTrace = ""
    private func trace() {
        let line = "deckwell deck=\(deckCount) badge=\(badgeTotal) layers=\(stackLayers)"
            + " flipped=" + (hasFlipped ? (flipped?.identity ?? "in-flight") : "-")
        if line != Self.lastTrace { Self.lastTrace = line; AnimLog.say(line) }
    }
    #endif

    public var body: some View {
        ZStack(alignment: .topLeading) {
            #if DEBUG
            let _ = trace()
            #endif
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
            //
            // …and it draws NOTHING while the trump is in the air (hasFlipped
            // with no card - see the type doc). That is not a fallthrough: the
            // card exists, the flight layer is carrying it, and the well must
            // neither hold a second copy of it nor put up the bare glyph that
            // means it is gone.
            if hasFlipped, let flipped {
                FCard(card: flipped, trump: true, size: CGSize(width: cardW, height: cardH))
                    .offset(x: Self.flippedOrigin.x, y: Self.flippedOrigin.y)
                    .zIndex(0)
            }

            if deckCount > 0 {
                deckStack
                    // A LAYER LEAVES THE WAY A CARD LEAVES: in one frame.
                    // Cards on this board never fade, and a leaning back that
                    // dissolved as the count ticked would be the one that did.
                    // The layer count only ever changes on the beat a real card
                    // leaves the stock (`shownDeckCount`), so there is nothing
                    // to interpolate towards - and this keeps whatever
                    // transaction happens to be in flight from inventing one.
                    .animation(nil, value: stackLayers)
                    .zIndex(1)
            } else if !hasFlipped, let trumpSuit {
                // Stock and flip both gone, and GONE MEANS LANDED - `hasFlipped`
                // is held until the trump's flight arrives, so this branch can
                // never draw underneath a trump that is still crossing the
                // board. See the type doc on `flipped`.
                //
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
                    .offset(x: inset + rotationNudge - CGFloat(i) * Self.leanX,
                            y: inset - rotationNudge - CGFloat(i) * Self.leanY)
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
