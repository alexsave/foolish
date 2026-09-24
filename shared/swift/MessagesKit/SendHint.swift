// SendHint - the staged-but-unsent reminder for a Messages app: the blue arrow
// that bobs under Messages' own Send button, its caption, the alignment guide
// the two share and the white outline that keeps both legible on a busy
// surface.
//
// WHY IT IS SHARED. Every Messages game stages a bubble and then depends on a
// human finding the Send arrow in the compose bar above the drawer - and a
// bubble left in the field stalls the whole thread. The look, the bob and the
// fuse were tuned over many rounds on device; a second product gets all of it
// by passing its caption.
//
// WHAT A PRODUCT SUPPLIES: the caption (its own string table - this file has
// no words), where the Send button's axis sits from the screen's trailing edge
// and from the hint's container's, and the fuse (how long a staged bubble
// waits before the hint appears). Nothing here takes a controller or a game:
// the whole surface is two Bools (`staged`, `visible`) and a restart key.
//
// A caption that must follow a language change is the PRODUCT's job to
// re-supply: this view re-resolves nothing by itself, so the caller observes
// whatever holds its language and passes the new string down, which is what
// puts the language into this view's inputs.

import SwiftUI

/// The shared axis the send reminder's arrow and caption align on - the
/// vertical line under Messages' Send button. The arrow always centres on it;
/// the caption centres on it too UNLESS that would push it past the screen's
/// right edge (a Russian caption did exactly that, running off screen), in
/// which case its own guide shifts so it hugs the edge instead.
private extension HorizontalAlignment {
    enum SendAxis: AlignmentID {
        static func defaultValue(in d: ViewDimensions) -> CGFloat {
            d[HorizontalAlignment.center]
        }
    }
    static let sendAxis = HorizontalAlignment(SendAxis.self)
}

/// HOW THE HINT IS INKED. The sister product's felt takes the send blue with a
/// ring of `outline` round it; light paper takes the reverse (uttt, owner
/// 2026-09-23): WHITE glyph and words with the send blue as the ring, so the
/// hint reads as Messages' own Send circle turned inside out, and a ring the
/// colour of the paper never sits on the paper.
public enum SendHintInk: Equatable {
    /// Send-blue glyph and words, ringed in `outline`.
    case blue(outline: Color)
    /// White glyph and words, ringed in the send blue.
    case white
}

/// A WHITE outline stamped around a glyph or a label, so it carries on a busy
/// surface whatever the texture is doing underneath it.
///
/// Neither SF Symbols nor text have an outline mode, so the ring is the content
/// itself drawn in white at 8 compass offsets underneath the coloured original.
/// At these radii the eight stamps overlap into a solid edge; fewer leave scallops
/// at the diagonals. `radius` is in points and is deliberately shared by the send
/// hint's arrow and its caption - two different outline weights on one object
/// that moves as one reads as a mistake rather than as emphasis.
public struct SendHintRing<Content: View>: View {
    public var radius: CGFloat = SendHintMetrics.ringRadius
    /// The ring's colour: white for the sister product's felt, black for a
    /// product on light paper (uttt, owner 2026-09-23).
    public var color: Color = .white
    @ViewBuilder public var content: () -> Content

    public init(radius: CGFloat = SendHintMetrics.ringRadius, color: Color = .white,
                @ViewBuilder content: @escaping () -> Content) {
        self.radius = radius
        self.color = color
        self.content = content
    }

    public var body: some View {
        ZStack {
            ForEach(0..<8, id: \.self) { i in
                let a = CGFloat(i) * .pi / 4
                content().foregroundColor(color)
                    .offset(x: radius * cos(a), y: radius * sin(a))
            }
            content()
        }
    }
}

/// The staged-but-unsent reminder. An up-arrow in the exact glyph Messages' own
/// Send button carries (SF Symbols `arrow.up` - the same symbol the compose
/// bar's circle draws in white), in the same system blue that circle is filled
/// with, bobbing on a sine wave; under it, at the BOTTOM of the arrow's travel,
/// a caption in heavy type. The arrow only travels UP from its resting spot, so
/// its rest position - directly above the caption - is the bottom of the wave.
public struct SendHintArrow: View {
    /// The words under the arrow, in the product's own language.
    let caption: String
    /// The caption's face before `.heavy` is applied on top of it.
    let font: Font
    /// Where the send axis sits from the SCREEN's trailing edge: the caption
    /// may extend at most this, less a 4pt margin, to the right of it.
    let screenAxis: CGFloat
    /// True while the reminder is not actually on screen (fuse not elapsed, or
    /// the drawer is expanded): freezes the TimelineView so an invisible arrow
    /// doesn't burn frames inside a Messages extension.
    var paused = false
    /// The inks (`SendHintInk`): the glyph's and the ring's colours.
    let ink: SendHintInk
    @Environment(\.colorScheme) private var scheme

    public init(caption: String, font: Font = SendHint.captionFont,
                screenAxis: CGFloat = SendHint.axisFromScreenTrailing, paused: Bool = false,
                outline: Color = .white) {
        self.init(caption: caption, font: font, screenAxis: screenAxis, paused: paused,
                  ink: .blue(outline: outline))
    }

    public init(caption: String, font: Font = SendHint.captionFont,
                screenAxis: CGFloat = SendHint.axisFromScreenTrailing, paused: Bool = false,
                ink: SendHintInk) {
        self.caption = caption
        self.font = font
        self.screenAxis = screenAxis
        self.paused = paused
        self.ink = ink
    }

    /// The glyph's colour and the ring's, for this appearance.
    private var fill: Color {
        if case .white = ink { return .white }
        return sendBlue
    }
    private var outline: Color {
        switch ink {
        case .blue(let o): return o
        case .white:       return sendBlue
        }
    }

    /// Messages fills its Send circle with the system blue, so match it
    /// exactly: UIKit systemBlue's resolved values (#007AFF light, #0A84FF
    /// dark), written out as literals so this file needs no UIKit import.
    private var sendBlue: Color {
        scheme == .dark ? Color(red: 0x0A / 255, green: 0x84 / 255, blue: 0xFF / 255)
                        : Color(red: 0x00 / 255, green: 0x7A / 255, blue: 0xFF / 255)
    }

    // "Make the arrow taller, a bit more width, and make it move faster and in
    // a larger range. It should be pretty clear that you aren't meant to hit the
    // hint arrow, but the actual iMessage send arrow (that is above the
    // collapsed view)." The four numbers below are that sentence.
    //
    // The arrow is drawn RESIZABLE at an explicit width x height rather than at
    // a font size, because "taller AND a bit wider" are two numbers and a font
    // size is one - the glyph is deliberately stretched ~20% taller than the
    // symbol's own proportions, which also makes it read as an arrow POINTING
    // somewhere rather than as a button you press.
    public static let arrowSize = SendHintMetrics.arrowSize
    /// Peak-to-trough travel: the whole point is that the eye follows it UP,
    /// off this view and onto Messages' own Send button.
    public static let bobTravel = SendHintMetrics.bobTravel
    /// Seconds per bob. 1.5 read as decoration.
    public static let bobPeriod = SendHintMetrics.bobPeriod
    /// Where the hint RESTS, measured down from the top of the container it is
    /// laid into. NEGATIVE: the whole hint is lifted out of the board and into
    /// the drawer's top margin.
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
    public static let crestRoom = SendHintMetrics.crestRoom

    public var body: some View {
        // The bob is a TimelineView-driven pure sine of wall-clock time, not a
        // `repeatForever` @State animation - the stateful kind is silently
        // CANCELLED whenever an ancestor re-renders inside a no-animation
        // transaction, which is why "sometimes the send arrow doesn't go up and
        // down". A value computed fresh every frame cannot be cancelled.
        //
        // The WHOLE hint rides the wave, caption included. With the caption
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
                // A WHITE stroke around the blue arrow AND the caption - see
                // `SendHintRing`. The two ride one wave and read as one object,
                // so an outlined arrow over bare blue text looked half-drawn.
                SendHintRing(color: outline) { arrow }
                    .alignmentGuide(.sendAxis) { d in d[HorizontalAlignment.center] }
                SendHintRing(color: outline) {
                    Text(caption)
                        .font(font).fontWeight(.heavy)
                        .fixedSize()
                }
                // Centred on the arrow, CLAMPED to the screen: the axis sits
                // `screenAxis` from the screen's right edge, so the caption may
                // extend at most `screenAxis - 4` right of it (a 4pt screen
                // margin). A caption wider than centring allows shifts left to
                // hug the edge instead of running off it. Measured on the RING,
                // which is what lays out - it is the text's own width either
                // way, since the stamps are offsets, not padding.
                .alignmentGuide(.sendAxis) { d in
                    max(d[HorizontalAlignment.center], d.width - (screenAxis - 4))
                }
            }
            .offset(y: -lift)
        }
        .foregroundColor(fill)
        .accessibilityHidden(true)   // decorative; the staged state reads elsewhere
    }
}

/// The reminder's LIFECYCLE, for every surface that stages a sendable bubble.
/// Owns the fuse and the fade: `staged` (or a new `restart` key) restarts the
/// fuse, and a send or a cancel - `staged` going false - hides it at once;
/// `visible` gates rendering to the collapsed drawer without disturbing the
/// fuse. `centerFromTrailing` is where the send-button axis sits measured from
/// THIS container's trailing edge. Purely decorative: it never eats a tap.
public struct SendHint: View {
    /// Messages' Send circle sits inside the compose field's right end with
    /// the drawer chrome inset around it, its centre ~42pt from the screen
    /// edge (measured off a real device screenshot; a first guess of ~24 read
    /// the field as nearly full-bleed).
    public static let axisFromScreenTrailing = SendHintMetrics.axisFromScreenTrailing
    /// How long a staged bubble sits unsent before the hint appears.
    public static let defaultFuse = SendHintMetrics.defaultFuse
    /// The caption's face: the system's 15pt semibold, made heavy on top - the
    /// action buttons' own text treatment ("larger and bolder").
    public static let captionFont: Font = .system(size: SendHintMetrics.captionSize, weight: .semibold, design: .default)

    let staged: Bool
    let visible: Bool
    let caption: String
    let font: Font
    let screenAxis: CGFloat
    let centerFromTrailing: CGFloat
    let fuse: Double
    let restart: Int
    let ink: SendHintInk
    /// Hide with no fade: a send, a cancel or a drawer starting to grow takes
    /// the hint down in the frame it happens (uttt; the sister product fades).
    let hidesAtOnce: Bool
    @State private var shown = false

    /// - Parameters:
    ///   - restart: a new value restarts the fuse even though `staged` stayed
    ///     true - a replacement bubble staged over the last one.
    public init(staged: Bool, visible: Bool, caption: String,
                font: Font = SendHint.captionFont,
                screenAxis: CGFloat = SendHint.axisFromScreenTrailing,
                centerFromTrailing: CGFloat = SendHint.axisFromScreenTrailing,
                fuse: Double = SendHint.defaultFuse, restart: Int = 0,
                outline: Color = .white) {
        self.init(staged: staged, visible: visible, caption: caption, font: font,
                  screenAxis: screenAxis, centerFromTrailing: centerFromTrailing,
                  fuse: fuse, restart: restart, ink: .blue(outline: outline), hidesAtOnce: false)
    }

    public init(staged: Bool, visible: Bool, caption: String,
                font: Font = SendHint.captionFont,
                screenAxis: CGFloat = SendHint.axisFromScreenTrailing,
                centerFromTrailing: CGFloat = SendHint.axisFromScreenTrailing,
                fuse: Double = SendHint.defaultFuse, restart: Int = 0,
                ink: SendHintInk, hidesAtOnce: Bool) {
        self.staged = staged
        self.visible = visible
        self.caption = caption
        self.font = font
        self.screenAxis = screenAxis
        self.centerFromTrailing = centerFromTrailing
        self.fuse = fuse
        self.restart = restart
        self.ink = ink
        self.hidesAtOnce = hidesAtOnce
    }

    private struct Fuse: Hashable { let staged: Bool; let restart: Int }

    public var body: some View {
        // Down in the same frame as the send when it hides at once, not a
        // runloop later when the fuse task has noticed.
        let on = shown && visible && (staged || !hidesAtOnce)
        SendHintArrow(caption: caption, font: font, screenAxis: screenAxis, paused: !on,
                      ink: ink)
            .alignmentGuide(.trailing) { d in
                d[HorizontalAlignment.sendAxis] + centerFromTrailing
            }
            // Where the arrow rests, and so how high the crest reaches: the
            // crest is pulled up out of this container and into the drawer's
            // top margin, toward Messages' own Send button. See
            // SendHintArrow.crestRoom for why it is smaller than the travel.
            .padding(.top, SendHintArrow.crestRoom)
            .opacity(on ? 1 : 0)
            // BOTH directions animate - a one-way withAnimation faded it in but
            // let a style change snap it off (or on) instantly. Scoped to `on`
            // so nothing else rides it.
            .animation(on || !hidesAtOnce ? .easeInOut(duration: 0.35) : nil, value: on)
            .allowsHitTesting(false)
            .task(id: Fuse(staged: staged, restart: restart)) {
                // A restart hides what a previous fuse showed; a fresh stage
                // (false -> true) finds it hidden already.
                shown = false
                guard staged else { return }
                try? await Task.sleep(nanoseconds: UInt64(fuse * 1_000_000_000))
                guard !Task.isCancelled, staged else { return }
                shown = true
            }
    }
}
