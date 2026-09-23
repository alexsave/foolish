import CUttt
import SwiftUI

/// The play surface, at the two sizes Messages gives it.
///
/// MESSAGES CHOOSES THE SIZE AND THIS FILE ONLY RECOGNISES IT. Collapsed is
/// 340 points tall - 323 while the compose field holds the first responder -
/// and the width is the phone's. Expanded is the screen less about 126 points
/// of Messages chrome: 541 on an SE, 726 on a 16, 748 on a 16 Pro, 830 on a
/// Pro Max. Nothing here asks to be any size; it is handed one and lays out
/// inside it, which is why the only number this file computes is the board's.
public struct UtttGameScreen: View {
    @StateObject private var model: UtttModel
    private let door: Uttt.Door
    private let onDoor: () -> Void

    /// `door` is the kernel's answer for this board (utm_door) - at the end of
    /// a game, Again. It stands in the expanded view only (docs/UI.html 08:
    /// "starting a game from the strip you land on by accident is how you
    /// start a game by accident").
    public init(model: UtttModel, door: Uttt.Door = .none, onDoor: @escaping () -> Void = {}) {
        _model = StateObject(wrappedValue: model)
        self.door = door
        self.onDoor = onDoor
    }

    /// The margin of the rules sheet, the same on every edge.
    private static let margin: CGFloat = 13

    private static let label = Color(red: 0.541, green: 0.522, blue: 0.467) // #8a8577
    private static let ink   = Color(red: 0.114, green: 0.106, blue: 0.087) // #1d1b16
    private static let blue  = Color(red: 0.145, green: 0.216, blue: 0.420) // #25376b

    /// THE DOOR OPENS ON THE SAME SHEET. Not a modal over a dimmed board:
    /// the drawer is already a piece of paper in a small box, and a card
    /// floating over it would be the only thing in the app that is not drawn
    /// on the napkin.
    @State private var rulesOpen = false

    public var body: some View {
        UtttSheet {
            if rulesOpen {
                UtttRulesSheet { rulesOpen = false }
                    .padding(Self.margin)
                    .transition(.opacity)
            } else {
                UtttDrawerSheet { size, from in sheet(size, from: from) }
            }
        }
        .animation(.easeInOut(duration: 0.18), value: rulesOpen)
    }

    // MARK: one layout, from compact to expanded

    /// EVERY NUMBER ON THIS SHEET IS THE KERNEL'S, for the height the drawer
    /// is at (`Uttt.sheet`, uttt_anim.c). There were two layouts here once and
    /// a threshold between them, so a collapse was a swap; then one layout of
    /// lerps that still moved the board off the sheet's centre whenever words
    /// sat above it. docs/UI.html, "What holds which edge": the header holds
    /// the top, the doors the bottom, and the board THE CENTRE - "348 to 214
    /// is a scale, not a slide". So the board's centre is the sheet's at every
    /// height, and everything else is placed at an edge around it.
    private func sheet(_ size: CGSize, from: CGFloat?) -> some View {
        /* AT THE END THE STRIP CARRIES THE VERDICT (UI.html 08: "the verdict
         * and the board"), in the right column beside the board, over the
         * rulebook - the board is as large as the sheet allows and the words
         * wrap into the room it leaves. While the game runs the strip carries
         * no words - the wash says it - and the headline fades in with the
         * band. */
        let end = Uttt.over != .none
        let hint = model.pending
        let L = Uttt.sheet(.play, size: size, words: end, hint: hint)
        /* THROUGH AN AUTO-COLLAPSE (CollapseSlide) the sheet is laid out at
         * the compact height and pushed; each rider below walks the path the
         * layout would have walked, as a function of the push `s` - the
         * drawer is `size.height + s` tall - from the kernel's own layout. */
        let at = { (s: CGFloat) -> UtiSheet in
            Uttt.sheet(.play, size: CGSize(width: size.width, height: size.height + s),
                       words: end, hint: hint)
        }
        /* The band's words are hidden on the strip, so through a slide they
         * are set as the slide's first frame had them and fade on their layer. */
        let B = from.map { Uttt.sheet(.play, size: CGSize(width: size.width, height: $0),
                                      words: end, hint: hint) } ?? L
        let r = UtttRuler.on
        let hpad = CGFloat(L.hpad), vpad = CGFloat(L.vpad)
        let icon = CGFloat(L.icon)

#if DEBUG
        if r { UtttLog.note("sheet-play", String(format: "h %.1f from %.1f board y %.1f side %.1f", size.height, from ?? -1, L.board.1, L.board.2)) }
#endif
        return board
            .boardRide(L, touches: true, at: at)
            .overlay(alignment: .topLeading) {
                /* THE HEADER HOLDS THE TOP, and its two parts ride apart
                 * (indicator), since the mark is a size of the drawer's
                 * height and the label over it is not. */
                indicator(icon: icon, lead: CGFloat(L.icon_lead), at: at, L: L)
                    .frame(width: max(CGFloat(L.col), icon + 2), alignment: .leading)
                    .padding(.leading, hpad)
                    .padding(.top, vpad + CGFloat(L.icon_top))
            }
            /* THE WORDS TWICE, in the column beside the ink and in the
             * band, each shown only where it fits (uttt_sheet) - so a drag
             * crossfades them and never squeezes them to "Wai...". */
            .overlay(alignment: .topLeading) {
                ZStack(alignment: .topLeading) {
                    words(column: true, r: false)
                        .inWords(L, alignment: .topTrailing)
                        .wordsRide(column: true, L, from: B, at: at)
                    words(column: false, r: r)
                        .inBand(B, alignment: .topTrailing)
                        .wordsRide(column: false, L, from: B, at: at)
                }
            }
            .overlay(alignment: .bottomTrailing) {
                HStack(alignment: .center, spacing: 10) {
                    if let title = UtttDoorButton.title(self.door), L.door_alpha > 0 {
                        UtttDoorButton(title: title, height: CGFloat(L.door), act: onDoor)
                            .motionSquare(.violet, on: r)
                            .opacity(Double(L.door_alpha))
                    }
                    UtttRulebookButton(side: CGFloat(L.door)) { rulesOpen = true }
                        .motionSquare(.blue, on: r)
                }
                .padding(.trailing, hpad)
                .padding(.bottom, vpad)
            }
    }

    /// The headline and the line under it, in one of the two places.
    private func words(column: Bool, r: Bool) -> some View {
        VStack(alignment: .trailing, spacing: 3) {
            headlineView(column: column)
                .motionSquare(.yellow, on: r)
                .accessibilityElement(children: .ignore)
                .accessibilityLabel(Uttt.say(.headlineSpoken))
                .accessibilityAddTraits(.isHeader)
            /* The line under it: where you sent them, or at the end
             * the winning line spoken (docs/UI.html 04, 06, 07). */
            if !model.subline.isEmpty {
                Text(model.subline)
                    .font(.system(size: 14))
                    .foregroundStyle(UtttInk.muted)
                    .multilineTextAlignment(.trailing)
                    .wordsWrap(model.subline, column: column)
                    .motionSquare(.lime, on: r)
            }
        }
    }

    /// THE BAR'S LINE, with the other side drawn rather than spelled.
    ///
    /// The mark is sized to the CAP HEIGHT of the type beside it, not to the
    /// line box, or it sits low and reads as a separate object; its middle
    /// sits on the middle of the lower-case words beside it.
    ///
    /// IN THE STRIP'S COLUMN words-only lines WRAP ("You win" over two lines
    /// rather than a board a size smaller), and a line with a drawn mark in it
    /// scales down to the column instead, since a mark cannot break a line.
    @ViewBuilder private func headlineView(column: Bool) -> some View {
        let ink = Uttt.over == .none ? Self.ink : Self.blue
        switch model.headline {
        case .text(let t):
            headlineText(t, ink)
                .multilineTextAlignment(.trailing)
                .wordsWrap(t, column: column)
        case .mark(let before, let m, let after):
            HStack(alignment: .firstTextBaseline, spacing: 0) {
                if !before.isEmpty { headlineText(before, ink) }
                UtttMarkIcon(mark: m, seed: model.seed &* 31 &+ 7)
                    .frame(width: 21, height: 21)
                    /* CENTRED ON THE WORDS' X-HEIGHT (owner, 2026-09-23: the
                     * mark and "wins" were not centred on each other). The
                     * words beside a mark are lower case ("wins", "to play",
                     * "Waiting on"), so their middle is half the x-height
                     * above the baseline, not half the cap height. */
                    .alignmentGuide(.firstTextBaseline) { $0.height / 2 + Self.xHeight / 2 }
                if !after.isEmpty { headlineText(after, ink) }
            }
            .lineLimit(1)
            .minimumScaleFactor(0.5)
        }
    }

    private static let xHeight = UIFont.systemFont(ofSize: 21, weight: .bold).xHeight

    private func headlineText(_ t: String, _ ink: Color) -> some View {
        Text(t)
            .font(.system(size: 21, weight: .bold))
            .tracking(-0.315)              // -.015em
            .foregroundStyle(ink)
    }


    // MARK: the pieces

    private var board: some View {
        UtttLiveBoard(clock: model.clock, positionKey: model.positionKey,
                      onTap: { model.tap(at: $0) })
    }

    /// THE SIDE INDICATOR IS A DRAWN MARK, not a glyph - the same X that is
    /// about to land on the board, out of the same pen. Setting it in a font
    /// made it the only thing in the frame that did not come off the nib.
    ///
    /// THROUGH AN AUTO-COLLAPSE THE LABEL AND THE MARK RIDE APART
    /// (CollapseSlide): the mark is 34 points on the strip and 46 open
    /// (`icon`), so riding the pair as one layer made the mark jump to its
    /// compact size in the slide's first frame (7pt, filmed). The mark scales
    /// about its top left and moves with the header; the label, centred over
    /// the mark, moves across by half its change of size.
    private func indicator(icon: CGFloat, lead: CGFloat,
                           at: @escaping (CGFloat) -> UtiSheet, L: UtiSheet) -> some View {
        VStack(spacing: 0) {
            // Two lines, set on a 9.5-point body - line-height 1, so they read
            // as one two-line label rather than two labels.
            VStack(spacing: -2.8) {
                line(Uttt.say(.youAre1))
                line(Uttt.say(.youAre2))
            }
            .collapseRide { s in
                let A = at(s)
                return CollapseRidePose(dy: CGFloat(A.icon_top - L.icon_top),
                                        dx: CGFloat(A.icon - L.icon) / 2)
            }
            UtttMarkIcon(mark: model.you, seed: model.seed &+ 4)
                .frame(width: icon, height: icon)
                .motionSquare(.orange, on: UtttRuler.on)
                .collapseRide { s in
                    let A = at(s)
                    return CollapseRidePose(
                        dy: CGFloat(A.icon_top + A.icon_lead - L.icon_top - L.icon_lead),
                        scale: L.icon > 0 ? CGFloat(A.icon / L.icon) : 1,
                        pivot: .zero)
                }
                .padding(.top, lead)
        }
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(Uttt.say(.youAreSpoken))
    }

    private func line(_ s: String) -> some View {
        Text(s)
            .font(.system(size: 9.5, weight: .semibold))
            .tracking(1.9)                     // .2em
            .textCase(.uppercase)
            .foregroundStyle(Self.label)
    }
}
