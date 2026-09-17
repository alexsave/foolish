// CollapseRuler.swift — the debug ruler the collapse was measured with.
//
// Round 10d measured the auto-collapse by drawing a ruler on the LIVE surface
// and reading the bands back per frame (see `MessagesRootView.follow`'s comment,
// and `MessagesViewController`'s round-10b note, where the ruler is what proved
// the flying rect in the films was the staged bubble's snapshot and not our
// view: it carried no ruler lines). The ruler itself was never committed, so
// every re-measurement started by rebuilding it. This is it, kept.
//
// WHY A RULER AND NOT A COLOUR PROBE. The collapse is composited by Messages
// from snapshots of our view, over featureless wool. "Where is the top of our
// box in this frame" is not answerable from the board's own pixels - wool looks
// the same everywhere, and the one landmark that is not wool (the hand) is
// exactly the thing whose position is in question. A ruler puts an unambiguous,
// non-wool, non-card marker on the box's own two edges and a measurable pitch
// between them, so a frame answers three questions at once: where the box top
// is, where the box bottom is, and whether the imagery was SCALED on its way to
// the screen (a snapshot stretched to a different height reads as a band pitch
// that is not 10pt).
//
// It is attached to the surface's SIZED BOX, not to the screen, so it measures
// `boxHeight` - the number the tween actually moves - rather than the drawer.
//
// PALETTE, and why these colours. Pure magenta / cyan / yellow / red / green:
// the only fully-saturated primaries on screen. The wool and wood are warm and
// desaturated, a card face is near-achromatic, so no band can be confused for
// board content by the same colour tests `msgui.py` already uses. They also
// survive h264 4:2:0 chroma subsampling, which a subtle palette would not.
//
// DEBUG ONLY, and behind `dev.ruler` on top of that: a filmed run wants the
// ruler, every other run does not. The release branch below is `EmptyView`, so
// the call site in `MessagesRootView` stays unconditional.

import SwiftUI

#if DEBUG || SOLO_TESTING

public struct CollapseRuler: View {
    /// Band height in points. The whole read is "count bands from the red bar",
    /// so this is the measurement's resolution as well as its scale check.
    public static let band: CGFloat = 10
    /// The banded strip's width, wide enough that a column median is stable
    /// under compression and narrow enough not to cover the seat badges.
    public static let strip: CGFloat = 18
    /// The full-width edge bars. 4pt is a whole pixel at every scale and still
    /// reads as an edge rather than as a band.
    public static let edge: CGFloat = 4

    /// THE RED BAR MARKS THE DRAWER'S TOP EDGE, NOT THE RAW BOX.
    ///
    /// Under the slide the two stopped being the same thing. The box is laid out
    /// compact and pushed down by a layer animation so its BOTTOM edge - and the
    /// hand and buttons on it - never move; the table cards, deck, discard and
    /// opponent ring then take that push back off themselves, so they keep
    /// riding the drawer's descending top edge exactly as they always did (see
    /// CollapseLayer). A red bar drawn on the box's own top would therefore mark
    /// a line nothing is drawn at, and it would report a 524pt teleport at the
    /// flip that no pixel on screen performs - measured, and it was 274,576 of a
    /// 274,594 jerk score whose every other frame summed to 18.
    ///
    /// So the top assembly - the bar, the bands counted from it and the clock -
    /// rides a collapse layer at fraction 0, the deck's and the discard's share:
    /// the whole of the slide taken back, at the composite rate. The red bar
    /// is then the drawer's own top edge as the render server placed it in that
    /// frame, which makes it the reference every other bar is judged against.
    /// (It used to carry the SwiftUI-rate cancellation the table group had,
    /// which measured the plumbing's staleness rather than the drawer.)
    public init() {}

    public var body: some View {
        if MessageDevBoard.rulerOn {
            GeometryReader { geo in
                let n = max(1, Int((geo.size.height / Self.band).rounded(.up)))
                ZStack(alignment: .topLeading) {
                    // The top assembly, on the drawer's edge (see `init`).
                    ZStack(alignment: .topLeading) {
                        // The banded strip, counted from the BOX TOP: band 0 is
                        // red, every tenth band (100pt) is yellow, the rest
                        // alternate. Bands are placed absolutely rather than
                        // stacked so a partial last band is simply clipped by
                        // the box.
                        ForEach(0..<n, id: \.self) { i in
                            Self.colour(i)
                                .frame(width: Self.strip, height: Self.band)
                                .offset(y: CGFloat(i) * Self.band)
                        }
                        // The top edge, full width: red = the drawer's top.
                        Self.pure(1, 0, 0)
                            .frame(width: geo.size.width, height: Self.edge)
                        // The clock, immediately under the top bar.
                        CollapseClock()
                            .offset(y: Self.edge)
                    }
                    .frame(width: geo.size.width, height: geo.size.height,
                           alignment: .topLeading)
                    .collapseLayer(fraction: 0)
                    // The bottom edge: green = the bottom of the box, which is
                    // where the hand and the buttons sit. On the main tree, so
                    // it takes the whole of the hosting layer's push, as they do.
                    Self.pure(0, 1, 0)
                        .frame(width: geo.size.width, height: Self.edge)
                        .offset(y: geo.size.height - Self.edge)
                }
                .frame(width: geo.size.width, height: geo.size.height,
                       alignment: .topLeading)
            }
            .allowsHitTesting(false)
        }
    }

    /// Band `i`'s colour. Pure primaries only - see the file note.
    static func colour(_ i: Int) -> Color {
        if i == 0 { return pure(1, 0, 0) }              // the box top itself
        if i % 10 == 0 { return pure(1, 1, 0) }         // every 100pt
        return i % 2 == 0 ? pure(0, 1, 1) : pure(1, 0, 1)
    }

    /// WHAT ELSE IS MARKED, AND WHY AS SQUARES.
    ///
    /// The two edge bars measure the BOX. That stopped being enough the moment
    /// the collapse started treating two groups differently, so round 47 added
    /// a full-width bar through the table cards and one through the first
    /// opponent. Then the table got squares of its own (`tableSquare`), and the
    /// bar ran straight through every one of them. Owner: "I think the lines are
    /// messing with the squares. Have one or the other not both. Or maybe turn
    /// the lines into squares themselves."
    ///
    /// So both marks are squares now. The table is marked by its pairs' squares
    /// alone - `.table` draws nothing, it only lifts the layer - and the first
    /// opponent carries one MAGENTA square at its centre: the one colour the
    /// table's squares never use, now that no magenta bar is left to collide
    /// with. `tween.py` reads the table's line off the pairs' squares and the
    /// opponent's off this one.
    ///
    /// An overlay and not a preference on purpose: a preference is delivered a
    /// layout pass later, which would measure the plumbing rather than the card,
    /// and lag is the whole question here.
    public enum Mark: Hashable {
        /// The table cards: marked by the pairs' own squares.
        case table
        /// One opponent's card view - the first, so there is only ever one.
        case opponent
    }

    /// Pair `i`'s square: cyan, yellow, green, repeating. The reader tells
    /// repeated colours apart by position.
    static func tableSquareColour(_ i: Int) -> Color {
        switch i % 3 {
        case 0: return pure(0, 1, 1)
        case 1: return pure(1, 1, 0)
        default: return pure(0, 1, 0)
        }
    }
    static let squareSide: CGFloat = 12
    /// The first opponent's square. Magenta: no table pair is ever magenta.
    static let opponentSquareColour = pure(1, 0, 1)

    /// A LITERAL sRGB colour, never `Color.red` and friends: the system colours
    /// are dynamic (red is 255,59,48 in light and 255,69,58 in dark) and the
    /// whole point of this palette is that a frame can be classified by channel
    /// without knowing which appearance the run was filmed in.
    static func pure(_ r: Double, _ g: Double, _ b: Double) -> Color {
        Color(.sRGB, red: r, green: g, blue: b, opacity: 1)
    }
}


/// A per-frame CLOCK, drawn as a binary strip a parser can read off a filmed
/// frame without OCR.
///
/// WHY THIS EXISTS. The ruler answers "where were the box's edges in this
/// frame"; it cannot answer "WHEN was this frame drawn". The video's own
/// presentation timestamps are the recorder's clock, not the app's, and a
/// variable-rate recording writes a frame when the SCREEN changes - which is
/// not the same as when our view last rendered.
///
/// That difference is the point. Messages composites this collapse from
/// SNAPSHOTS of our view (see `MessagesViewController`'s round-10b note, where
/// the ruler proved the flying rect was a snapshot and not our live view). If
/// the host is interpolating stale snapshots through the transition, our clock
/// STOPS ADVANCING on exactly those frames while the geometry keeps moving.
/// A frame whose clock repeats is a frame we did not draw - and no amount of
/// tuning an animation curve can fix a frame the app never rendered.
///
/// `TimelineView(.animation)` is what makes it a real per-frame value: it
/// re-evaluates on every display refresh, so the strip changes 60 times a
/// second when we are genuinely rendering and freezes when we are not.
///
/// FORMAT. 14 cells, most significant first, white = 1 and black = 0, so the
/// strip reads as milliseconds modulo 16384 (16.4s - far longer than any
/// collapse). White and black rather than the ruler's primaries because this
/// strip sits over board content and needs the largest possible luminance
/// separation after h264 chroma subsampling. The cell is 12pt so a column
/// median is stable at every scale.
struct CollapseClock: View {
    static let bits = 14
    static let cell: CGFloat = 12

    var body: some View {
        TimelineView(.animation) { ctx in
            let ms = Int((ctx.date.timeIntervalSince1970 * 1000).rounded())
                     & ((1 << Self.bits) - 1)
            HStack(spacing: 0) {
                ForEach(0..<Self.bits, id: \.self) { i in
                    let on = (ms >> (Self.bits - 1 - i)) & 1 == 1
                    Rectangle()
                        .fill(on ? CollapseRuler.pure(1, 1, 1)
                                 : CollapseRuler.pure(0, 0, 0))
                        .frame(width: Self.cell, height: Self.cell)
                }
            }
        }
        .allowsHitTesting(false)
    }
}

/// Where a marker bar wants to be, carried UP the tree as an anchor.
///
/// An anchor preference and not an overlay on the view itself, because an
/// overlay is drawn at its parent's place in the stack and the parents here are
/// drawn before the deck and the discard - so the bars came out underneath them
/// (owner: "increase z index of the horizontal lines as much as you can").
/// An anchor preference is resolved in the SAME layout pass by
/// `overlayPreferenceValue`, unlike `onPreferenceChange`, which lands a pass
/// later - and a bar that reports where its view was last frame is exactly the
/// measurement error this is here to find.
public struct CollapseMarkKey: PreferenceKey {
    public static let defaultValue: [CollapseRuler.Mark: Anchor<CGPoint>] = [:]
    public static func reduce(value: inout [CollapseRuler.Mark: Anchor<CGPoint>],
                              nextValue: () -> [CollapseRuler.Mark: Anchor<CGPoint>]) {
        value.merge(nextValue()) { a, _ in a }
    }
}

public extension View {
    /// A mark on this view: a magenta square at the centre of the first
    /// opponent; nothing extra for the table, whose pairs carry their own
    /// squares. Nil asks for none, which is how a list of seats marks only its
    /// first.
    ///
    /// AN OVERLAY ON THE VIEW, and it went the long way round to get back here.
    /// Anchor preferences resolve in the same layout pass and draw above
    /// everything, which is exactly what was wanted - but the table's bar never
    /// appeared at all through that route while the opponent's did, and a
    /// measurement instrument that silently reports nothing is worse than one
    /// drawn in the wrong order. An overlay is the boring version: it cannot
    /// miss, because it IS the view's own geometry, and the stacking is fixed
    /// instead by `.zIndex` at the call site.
    @ViewBuilder
    func collapseMark(_ mark: CollapseRuler.Mark?) -> some View {
        if let mark {
            overlay {
                if MessageDevBoard.rulerOn && mark == .opponent {
                    CollapseRuler.opponentSquareColour
                        .frame(width: CollapseRuler.squareSide, height: CollapseRuler.squareSide)
                        .allowsHitTesting(false)
                }
            }
            // ABOVE THE DECK AND THE DISCARD, which are drawn after these two in
            // the board's stack and were covering the marks. Only ever raised
            // when the ruler is on, so a shipping board stacks as it always did.
            .zIndex(MessageDevBoard.rulerOn ? 50 : 0)
        } else {
            self
        }
    }

    /// A small square at the centre of a table pair's slot, in a colour
    /// per pair, so a filmed take can say where every pair was in every frame
    /// (`ios/Tools/rig/lib/tablesquares.py`). The horizontal bars cannot: a
    /// throw-in re-centres the row SIDEWAYS, and a bar through the table's
    /// centre does not move at all while both pairs jump 36pt left.
    ///
    /// On the SLOT, not on a card: the cards tilt as a cover lands, the slot
    /// does not, so this moves only when the layout does. Centre-centre. Cyan,
    /// yellow and pure green, never magenta, which is the opponent's square.
    /// Nil asks for none (a pair whose attack card is not on the table yet).
    @ViewBuilder
    func tableSquare(_ index: Int?) -> some View {
        if let index, MessageDevBoard.rulerOn {
            overlay(alignment: .center) {
                CollapseRuler.tableSquareColour(index)
                    .frame(width: CollapseRuler.squareSide, height: CollapseRuler.squareSide)
                    .allowsHitTesting(false)
            }
        } else {
            self
        }
    }

    /// The same lift, for the collapse LAYER a marked view is hosted on: the
    /// mark's own `zIndex` orders it inside that host, where it has no
    /// siblings, and it is the host that has to come out above the deck.
    func collapseMarkLift() -> some View {
        zIndex(MessageDevBoard.rulerOn ? 50 : 0)
    }
}

#else

/// Release builds have no ruler; the call site stays unconditional.
public struct CollapseRuler: View {
    public init() {}
    public var body: some View { EmptyView() }
    public enum Mark: Hashable { case table, opponent }
}

public struct CollapseMarkKey: PreferenceKey {
    public static let defaultValue: [CollapseRuler.Mark: Anchor<CGPoint>] = [:]
    public static func reduce(value: inout [CollapseRuler.Mark: Anchor<CGPoint>],
                              nextValue: () -> [CollapseRuler.Mark: Anchor<CGPoint>]) {}
}

public extension View {
    func collapseMark(_ mark: CollapseRuler.Mark?) -> some View { self }
    func collapseMarkLift() -> some View { self }
    func tableSquare(_ index: Int?) -> some View { self }
}


#endif
