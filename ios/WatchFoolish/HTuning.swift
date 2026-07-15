// HTuning.swift — every layout number in Option H, in one place, with the live face
// beside it.
//
// THE TUNING LOOP: open this file, hit ⌥⌘↩ for the canvas, edit a number below, save.
// The previews at the bottom of THIS file redraw on the right — Xcode's canvas only shows
// previews declared in the file you are looking at, which is why they live here and not
// only in Previews.swift. No app UI, nothing to run, no kernel: the fixtures are static.
//
// The layout views own no magic numbers; they read these constants. Nothing in this file
// knows how the screen is arranged, and the screen knows nothing about tuning.
//
// All values are watchOS points on the 40 mm baseline (162 × 197 pt) and scale
// proportionally on bigger faces. The face is ~3 pt from overflowing top-to-bottom: the
// lane (§4.6.1) is the tightest column, so `laneFocusY` + `ring2Down` is the number that
// pushes the bottom item off the screen.

import SwiftUI

enum HTuning {

    // MARK: Header — InfoLine (label line over values, right-aligned under the clock)
    static let headerX: CGFloat = 104        // centre of the 3-column block
    static let headerY: CGFloat = 40
    static let headerColGap: CGFloat = 6
    static let colFlip: CGFloat = 26         // column widths; labels + values share them
    static let colDeck: CGFloat = 22
    static let colDisc: CGFloat = 30         // fits the word DISCARD
    static let headerRowGap: CGFloat = 1     // label baseline → value row
    static let labelSize: CGFloat = 7        // == captionSize by design; see §4.6.1
    static let valueSize: CGFloat = 12
    static let flipGlyphSize: CGFloat = 10   // the flipped card, while it exists
    static let trumpGlyphSize: CGFloat = 13  // the bare suit, once it's drawn

    // MARK: Seat strip
    static let stripY: CGFloat = 64
    static let stripCellMax: CGFloat = 18    // caps at 8 seats; 22 was G's value
    static let stripGap: CGFloat = 3
    static let stripCountSize: CGFloat = 12.5
    static let stripCellAspect: CGFloat = 1.2
    static let shieldW: CGFloat = 0.92       // × cell
    static let shieldH: CGFloat = 1.0        // × cell; > ~1.05 buries the self underline
    /// "You are here" is carried by WEIGHT — colour is reserved for state, and weight is a
    /// real font axis that needs no tricks and survives being 12.5 pt. (Outlining was tried
    /// and dropped: SwiftUI cannot stroke `Text` before watchOS 11's `textRenderer(_:)`.)
    ///
    /// Keep these a SHORT step apart on SF's weight axis:
    ///
    ///     ultraLight · thin · light · regular · medium · semibold · bold · heavy · black
    ///
    /// bold/regular is 3 stops — enough to find your seat at a glance. Tried and rejected:
    /// heavy/light (5 stops — reads as two different typefaces) and semibold/regular
    /// (2 — invisible). semibold/light is the other 3-stop pair, centred lighter, so the
    /// other seats recede instead of yours stepping forward; it also thins the *coloured*
    /// states, which is the reason to prefer this pair if the green tally must stay loud.
    /// The roster reads `stripSelfWeight` too, so both screens stay in step.
    static let stripSelfWeight: Font.Weight = .bold
    static let stripOtherWeight: Font.Weight = .regular

    // MARK: Table list — the pairs, cover ▸ attack
    static let tableX: CGFloat = 54.5        // centre of the column
    static let tableY: CGFloat = 136.5
    static let tableW: CGFloat = 93
    static let tableH: CGFloat = 121
    static let tableGlyph: CGFloat = 17
    static let tableCellW: CGFloat = 26
    static let tableColGap: CGFloat = 5
    static let tableArrowSize: CGFloat = 13
    static let tableArrowW: CGFloat = 11
    static let tableRowH: CGFloat = 25
    static let tableRowGap: CGFloat = 4
    static let tableVisibleRows = 4          // past this the list scrolls
    static let tableResolvedOpacity: CGFloat = 0.62
    static let tableOptimisticOpacity: CGFloat = 0.45
    static let tableFadeInset: CGFloat = 0.07 // scroll edge fade, fraction of height
    static let tableEmptySize: CGFloat = 18   // the "—" when there's no table
    /// Cards deal in from the direction the play came from — attacks up from the bottom,
    /// covers down from the top. This spring is that flight.
    static let tableDealResponse: Double = 0.32
    static let tableDealDamping: Double = 0.78
    /// How fast cards LEAVE the table. Short on purpose: a pickup moves them into your
    /// hand, so a lingering exit shows the same card on the table and in the lane at once.
    static let tableClearFade: Double = 0.14

    // MARK: Fisheye lane — the hand
    static let laneX: CGFloat = 131          // centre; hugs the crown edge
    static let laneW: CGFloat = 56
    static let laneFocusY: CGFloat = 132
    static let focusSize: CGFloat = 27       // Glyph size; suit ≈ ×1.34 = 36 pt
    static let ring1Size: CGFloat = 12
    static let ring1Up: CGFloat = 30
    static let ring1Down: CGFloat = 41       // > Up: the caption lives in this gap
    static let ring1Opacity: Double = 0.9
    static let ring2Size: CGFloat = 12       // == ring1Size: graded by opacity alone
    static let ring2Up: CGFloat = 48
    static let ring2Down: CGFloat = 60
    static let ring2Opacity: Double = 0.6    // 0.45 crushed the rank away
    static let checkScale: CGFloat = 1.10    // × ring size — the GOOD ✓
    static let arrowScale: CGFloat = 1.05    // × ring size — the pickup ↓
    /// The lane tracks the crown continuously; this spring is only how it comes to REST on
    /// a card once you let go (and how a tap glides to a card). Lower response = snappier.
    static let laneSettleResponse: Double = 0.26
    static let laneSettleDamping: Double = 0.85
    /// Top of the lane's single tap target. Below the seat strip, so a tap meant for the
    /// strip (→ roster) is never eaten by the lane.
    static let laneTapTop: CGFloat = 78

    // MARK: Action caption
    static let captionY: CGFloat = 158
    static let captionW: CGFloat = 62
    static let captionSize: CGFloat = 7
    static let captionFade: Double = 0.12      // in/out as the lane settles / starts moving

    // MARK: Chooser overlay
    static let chooserIcon: CGFloat = 30
    static let chooserIconTight: CGFloat = 21  // used past `chooserTightAfter` items
    static let chooserTightAfter = 3
    static let chooserGap: CGFloat = 10
    static let chooserCaptionGap: CGFloat = 3
    static let chooserArrowScale: CGFloat = 1.05
    static let scrimOpacity: CGFloat = 0.96
    static let chooserFade: CGFloat = 0.18     // present animation, seconds
    static let chooserScaleFrom: CGFloat = 0.94

    // MARK: Card glyph — the atom (affects every card everywhere)
    static let glyphSuitScale: CGFloat = 1.34  // suit font ÷ Glyph size
    static let glyphRankScale: CGFloat = 0.56
    static let glyphRank10Scale: CGFloat = 0.48 // "10" is two chars; needs its own
    static let glyphFrameW: CGFloat = 1.5
    static let glyphFrameH: CGFloat = 1.46
    static let glyphRankY: CGFloat = 0.54      // rank's optical centre, × height
    static let dimSuitWhite: CGFloat = 0.43    // dim suit gray; do NOT fade instead
    static let focusRingWidth: CGFloat = 1.25

    // MARK: Reject glow
    static let glowWidth: CGFloat = 4
    static let glowBlur: CGFloat = 5
    static let glowDuration: CGFloat = 0.6

    // MARK: Shield travel (§4.6.1 — the wrap animation)
    static let shieldSpringResponse: Double = 0.34
    static let shieldSpringDamping: Double = 0.72
    static let shieldExitDuration: Double = 0.17
    static let shieldEnterDuration: Double = 0.22
}

// MARK: - The canvas, beside the numbers
//
// Static fixtures (`Deal`, defined in Previews.swift) — no kernel, no bots, no deal, so
// these are instant and show exactly the state named. Previews.swift has the fuller set
// (11-card hand, 7-pair scroll, heads-up, escapees, game over); these are the four worth
// having in your eyeline while you push the constants above around.
//
// Xcode shows one at a time — use the canvas's preview picker to switch, or pin one.

/// The everyday board: a covered pair, an open attack, two seats voted, one escaped.
/// Watch the header, strip, table column and lane all move as you edit.
#Preview("① table · 8p") {
    TableScreen(game: WatchGame(preview: Deal.eight(
        defender: 3, firstAttacker: 2,
        battles: [Deal.battle("Qs", "Ks"), Deal.battle("Kc")],
        good: [4, 6]),
        legal: Deal.attackAll(Deal.hand("6h Ah Kh 6s 10c Jd"))),
        onOpenRoster: {})
}

/// Every seat colour at once — red opener (only while the table is empty), orange
/// defender, green voted, dark gray escaped, white the rest; YOU are seat 0, marked by
/// `stripSelfWeight` vs `stripOtherWeight`. This is the one to watch for the strip.
#Preview("② strip · all colours") {
    TableScreen(game: WatchGame(preview: Deal.eight(
        defender: 3, firstAttacker: 2, good: [4, 6]),
        legal: Deal.attackAll(Deal.hand("6h Ah Kh 6s 10c Jd"))),
        onOpenRoster: {})
}

/// You defend three attacks: the lane's terminal is the red ↓ and reads PICKUP. Good for
/// the lane ring offsets (`laneFocusY`, `ring1*`, `ring2*`) and `captionY`.
#Preview("③ lane · defending") {
    TableScreen(game: WatchGame(preview: Deal.eight(
        viewerHand: "Ac Ks 7s 6h 10c", defender: 0,
        battles: [Deal.battle("8c", "Jc"), Deal.battle("Kc"), Deal.battle("Kd")]),
        legal: [Move(type: .pickup)]),
        onOpenRoster: {})
}

/// The chooser — for `chooserIcon`, `chooserGap`, `captionSize`, `scrimOpacity`.
#Preview("④ chooser") {
    TableScreen(game: WatchGame(preview: Deal.eight(
        defender: 0, battles: [Deal.battle("9s"), Deal.battle("9c")])),
        onOpenRoster: {})
        .overlay { ChooserOverlay(spec: .demo, onCover: { _ in }, onPass: { _ in }, onClose: {}) }
}
