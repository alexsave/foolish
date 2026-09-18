// SendHint — the staged-but-unsent reminder (round-8 #3 / round-9): the blue
// arrow that bobs under Messages' own Send button, its caption, the alignment
// guide the two share and the white outline that keeps both legible on wool.
//
// Lifted out of MessageTableView.swift, where it sat below the board that
// overlays it. Nothing here takes a controller, a GameView or a card: the whole
// surface is two Bools (`staged`, `visible`) and a trailing inset, which is why
// it can be looked at on its own and why it belongs on its own.

import SwiftUI


/// Round-8 #3: the shared axis the send reminder's arrow and caption align on
/// - the vertical line under Messages' Send button. The arrow always centres
/// on it; the caption centres on it too UNLESS that would push it past the
/// board's right edge (a Russian caption did exactly that, running off
/// screen), in which case its own guide shifts so it hugs the edge instead.
private extension HorizontalAlignment {
    enum SendAxis: AlignmentID {
        static func defaultValue(in d: ViewDimensions) -> CGFloat {
            d[HorizontalAlignment.center]
        }
    }
    static let sendAxis = HorizontalAlignment(SendAxis.self)
}

/// A WHITE outline stamped around a glyph or a label, so it carries on the
/// wool whatever the weave is doing underneath it.
///
/// Neither SF Symbols nor text have an outline mode, so the ring is the content
/// itself drawn in white at 8 compass offsets underneath the coloured original.
/// At these radii the eight stamps overlap into a solid edge; fewer leave scallops
/// at the diagonals. `radius` is in points and is deliberately shared by the send
/// hint's arrow and its caption - two different outline weights on one object
/// that moves as one reads as a mistake rather than as emphasis.
struct WhiteRing<Content: View>: View {
    var radius: CGFloat = 1.6
    @ViewBuilder var content: () -> Content

    var body: some View {
        ZStack {
            ForEach(0..<8, id: \.self) { i in
                let a = CGFloat(i) * .pi / 4
                content().foregroundColor(.white)
                    .offset(x: radius * cos(a), y: radius * sin(a))
            }
            content()
        }
    }
}

/// Round-8 #3 / round-9: the staged-but-unsent reminder. An up-arrow in the
/// exact glyph Messages' own Send button carries (SF Symbols `arrow.up`, bold -
/// the same symbol the compose bar's circle draws in white), in the same system
/// blue that circle is filled with, bobbing on a sine wave; under it, at the
/// BOTTOM of the arrow's travel, a caption in the action buttons' own text
/// treatment (owner: "larger and bulder, like the rest action button texts").
/// The arrow only travels UP from its resting spot, so its rest position -
/// directly above the caption - is the bottom of the wave.
struct SendHintReminder: View {
    /// Re-render this view when a setting changes (see FPrefs) - the house
    /// pattern, shared with FActionBar and SettingsHelpSquares.
    ///
    /// THE BUG THIS FIXES (owner, Eva's test pass): "send hint text did not
    /// change back to english when I changed to chinese then to english.
    /// switched back after some time though." `FStrings.t` is a plain function
    /// call, so a view's caption is only re-resolved when SwiftUI re-evaluates
    /// its body - and this view's stored properties (`paused`) do not change
    /// when the language does, so SwiftUI correctly skipped it. "After some
    /// time" is whatever unrelated change finally forced a rebuild, which is
    /// the signature of this whole class of bug: the string is not stale, the
    /// BODY is. Observing the settings object is what puts the language back
    /// into this view's inputs.
    @ObservedObject private var prefs = FPrefs.shared
    /// True while the reminder is not actually on screen (fuse not elapsed, or
    /// the drawer is expanded): freezes the TimelineView so an invisible arrow
    /// doesn't burn frames inside a Messages extension.
    var paused = false
    @Environment(\.colorScheme) private var scheme

    /// Messages fills its Send circle with the system blue, so match it
    /// exactly: UIKit systemBlue's resolved values (#007AFF light, #0A84FF
    /// dark), written out as literals so FoolishKit needs no UIKit import.
    private var sendBlue: Color {
        scheme == .dark ? Color(red: 0x0A / 255, green: 0x84 / 255, blue: 0xFF / 255)
                        : Color(red: 0x00 / 255, green: 0x7A / 255, blue: 0xFF / 255)
    }

    // ROUND 16 (owner): "make the send button hint arrow more obvious. Make the
    // arrow taller, a bit more width, and make it move faster and in a larger
    // range. It should be pretty clear that you aren't meant to hit the hint
    // arrow, but the actual iMessage send arrow (that is above the collapsed
    // view)." The four numbers below are that sentence.
    //
    // The arrow is drawn RESIZABLE at an explicit width x height rather than at
    // a font size, because "taller AND a bit wider" are two numbers and a font
    // size is one - the glyph is deliberately stretched ~20% taller than the
    // symbol's own proportions, which also makes it read as an arrow POINTING
    // somewhere rather than as a button you press.
    static let arrowSize = CGSize(width: 21, height: 29)   // was ~15 x 17 (font 16 bold)
    /// Peak-to-trough travel. Nearly 3x the old 5pt: the whole point is that the
    /// eye follows it UP, off this view and onto Messages' own Send button.
    static let bobTravel: CGFloat = 14
    /// Seconds per bob. Was 1.5 - slow enough to read as decoration.
    static let bobPeriod: Double = 0.85
    /// Where the hint RESTS, measured down from the top of the container it is
    /// laid into (the board's own top inset). NEGATIVE: round 16 lifts the whole
    /// hint out of the board and into the drawer's top margin - owner: "take the
    /// entire send hint div and just move it up".
    ///
    /// It is also smaller than `bobTravel`, so the crest rides higher still.
    /// Raising the REST position is the only way to get closer to Messages' Send
    /// button: growing the travel alone just pushes the rest position down,
    /// since the crest is pinned by whatever headroom is left above it.
    ///
    /// The only thing in that margin is the grabber, which is CENTRED while this
    /// hint hugs the trailing edge, so there is nothing up there to collide
    /// with; the drawer's rounded corner is the real ceiling and the crest still
    /// clears it (measured on device: the tip stops ~8pt short of the edge).
    static let crestRoom: CGFloat = -9

    var body: some View {
        // Round-9: the bob is a TimelineView-driven pure sine of wall-clock
        // time, not a `repeatForever` @State animation - the stateful kind is
        // silently CANCELLED whenever an ancestor re-renders inside a
        // no-animation transaction (this board carries several), which is why
        // "sometimes the send arrow doesn't go up and down". A value computed
        // fresh every frame cannot be cancelled.
        //
        // ROUND 16: the WHOLE hint rides the wave, caption included (owner:
        // "lets make the text move up and down so it looks less like a button
        // and more like an arrow to the imessage button"). With the caption
        // pinned and only the arrow moving, the pair read as a label with a
        // fidgeting icon - a control. Moving together, they read as one object
        // travelling toward the Send button above, which is the whole message.
        TimelineView(.animation(minimumInterval: nil, paused: paused)) { ctx in
            let t = ctx.date.timeIntervalSinceReferenceDate
            // Rest at 0, crest at -bobTravel: (1-cos) starts the wave at rest
            // and only ever lifts away from it.
            let lift = Self.bobTravel / 2 * (1 - cos(t * 2 * .pi / Self.bobPeriod))
            let arrow = Image(systemName: "arrow.up")
                .resizable()
                .frame(width: Self.arrowSize.width, height: Self.arrowSize.height)
            VStack(alignment: .sendAxis, spacing: 3) {
                // Round-10 #3 (owner): a WHITE stroke around the blue arrow so
                // it carries on the wool - see `WhiteRing`. ROUND 22 puts the
                // SAME ring on the caption (owner: "give the moving send text
                // (with the arrow) a white stroke too"): the two ride one wave
                // and read as one object, so an outlined arrow over bare blue
                // text left the pair looking half-drawn - and it was the text,
                // sitting lower and on busier weave, that needed it more.
                WhiteRing { arrow }
                    .alignmentGuide(.sendAxis) { d in d[HorizontalAlignment.center] }
                WhiteRing {
                    Text(FStrings.t("ios.msg.sendhint"))
                        .font(FType.title(15)).fontWeight(.heavy)
                        .fixedSize()
                }
                // Centred on the arrow, CLAMPED to the screen: the axis sits
                // `sendHintCenterFromScreenTrailing` (42) from the screen's
                // right edge, so the caption may extend at most 38pt right of
                // it (a 4pt screen margin). A caption wider than centring
                // allows shifts left to hug the edge instead of running off it
                // (the ru caption did exactly that). Measured on the RING, which
                // is what lays out now - it is the text's own width either way,
                // since the stamps are offsets, not padding.
                .alignmentGuide(.sendAxis) { d in
                    max(d[HorizontalAlignment.center],
                        d.width - (MessageTableView.sendHintCenterFromScreenTrailing - 4))
                }
            }
            .offset(y: -lift)
        }
        .foregroundColor(sendBlue)
        .accessibilityHidden(true)   // decorative; the staged state already reads via Undo
    }
}

/// Round-9: the reminder's LIFECYCLE, shared by every surface that stages a
/// sendable bubble (the board's moves AND the lobby's join/invite/start - the
/// owner: "the send arrow should show up for all things where you stage and can
/// send"). Owns the 3-second fuse and the fade: `staged` restarts the fuse
/// whenever the staged state flips (send/undo hides it at once); `visible`
/// gates rendering to the collapsed view without disturbing the fuse.
/// `centerFromTrailing` is where the send-button axis sits measured from THIS
/// container's trailing edge (the board is inset 8 from the screen, the lobby
/// overlay is full-bleed). Purely decorative: it never eats a tap.
struct StagedSendHint: View {
    /// Re-render this view when a setting changes (see FPrefs). Only the
    /// OBSERVATION matters - the strings still come from FStrings.t and the
    /// table surface still comes from FTextures.
    @ObservedObject private var prefs = FPrefs.shared
    let staged: Bool
    let visible: Bool
    var centerFromTrailing: CGFloat = MessageTableView.sendHintCenterFromScreenTrailing
    @State private var shown = false

    var body: some View {
        let on = shown && visible
        SendHintReminder(paused: !on)
            .alignmentGuide(.trailing) { d in
                d[HorizontalAlignment.sendAxis] + centerFromTrailing
            }
            // Where the arrow rests, and so how high the crest reaches: round 16
            // pulls the crest up out of this container and into the drawer's top
            // margin, toward Messages' own Send button. See
            // SendHintReminder.crestRoom for why it is smaller than the travel.
            .padding(.top, SendHintReminder.crestRoom)
            .opacity(on ? 1 : 0)
            // Round-10 #2 ("fade it out"): BOTH directions animate - the old
            // one-way withAnimation faded it in but let a style change snap it
            // off (or on) instantly. Scoped to `on` so nothing else rides it.
            .animation(.easeInOut(duration: 0.35), value: on)
            .allowsHitTesting(false)
            .task(id: staged) {
                guard staged else { shown = false; return }
                try? await Task.sleep(nanoseconds: 3_000_000_000)
                guard !Task.isCancelled, staged else { return }
                shown = true
            }
    }
}
