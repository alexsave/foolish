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

    public init() {}

    public var body: some View {
        if MessageDevBoard.rulerOn {
            GeometryReader { geo in
                let n = max(1, Int((geo.size.height / Self.band).rounded(.up)))
                ZStack(alignment: .topLeading) {
                    // The banded strip, counted from the BOX TOP: band 0 is red,
                    // every tenth band (100pt) is yellow, the rest alternate.
                    // Bands are placed absolutely rather than stacked so a
                    // partial last band is simply clipped by the box.
                    ForEach(0..<n, id: \.self) { i in
                        Self.colour(i)
                            .frame(width: Self.strip, height: Self.band)
                            .offset(y: CGFloat(i) * Self.band)
                    }
                    // The two edges, full width: these are what a frame is read
                    // for. Red = the top of our box, green = the bottom.
                    Self.pure(1, 0, 0)
                        .frame(width: geo.size.width, height: Self.edge)
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

    /// A LITERAL sRGB colour, never `Color.red` and friends: the system colours
    /// are dynamic (red is 255,59,48 in light and 255,69,58 in dark) and the
    /// whole point of this palette is that a frame can be classified by channel
    /// without knowing which appearance the run was filmed in.
    static func pure(_ r: Double, _ g: Double, _ b: Double) -> Color {
        Color(.sRGB, red: r, green: g, blue: b, opacity: 1)
    }
}

#else

/// Release builds have no ruler; the call site stays unconditional.
public struct CollapseRuler: View {
    public init() {}
    public var body: some View { EmptyView() }
}

#endif
