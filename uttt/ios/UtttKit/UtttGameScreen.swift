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

    /// The sheet's margin, the same on every edge at both sizes.
    private static let margin: CGFloat = 13

    /// EXCEPT ABOVE AND BELOW THE COLLAPSED STRIP, where height is what runs
    /// out. Messages gives an iPhone SE about 231 points of drawer, not the
    /// 340 the design document measured on a taller phone, so on the smallest
    /// device the board is limited by the height and every point of vertical
    /// margin comes straight off it.
    private static let vmargin: CGFloat = 8

    /// TWO COLUMNS, ONE EITHER SIDE, in the collapsed strip. The left one
    /// carries "you are" and the right one carries nothing - it exists so the
    /// board sits in the middle of the SHEET rather than in the middle of what
    /// is left over, which are different places and the eye knows it.
    ///
    /// 38, not 46: the mark inside is 34 and the two stacked words are about
    /// 30, so the extra twelve points were air on both sides and the board is
    /// what wanted them.
    private static let column: CGFloat = 38

    /// The height the drawer is halfway open at - the middle of the handover,
    /// not a threshold any more. Nothing switches at it.
    private static let collapsedCeiling: CGFloat = 400

    /// The rules door, at each end. It stays on the collapsed strip because
    /// it is the only way to the rules.
    private static let doorCollapsed: CGFloat = 38

    private static let label = Color(red: 0.541, green: 0.522, blue: 0.467) // #8a8577
    private static let ink   = Color(red: 0.114, green: 0.106, blue: 0.086) // #1d1b16
    private static let blue  = Color(red: 0.145, green: 0.216, blue: 0.420) // #25376b

    /// THE DOOR OPENS ON THE SAME SHEET. Not a modal over a dimmed board:
    /// the drawer is already a piece of paper in a small box, and a card
    /// floating over it would be the only thing in the app that is not drawn
    /// on the napkin.
    @State private var rulesOpen = false

    public var body: some View {
        UtttSheet {
            GeometryReader { geo in
                if rulesOpen {
                    UtttRulesSheet { rulesOpen = false }
                        .padding(Self.margin)
                        .transition(.opacity)
                } else {
                    sheet(geo.size)
                }
            }
        }
        .animation(.easeInOut(duration: 0.18), value: rulesOpen)
    }

    // MARK: one layout, two ends of it

    /// HOW FAR OPEN THE DRAWER IS, 0 to 1, from the height and nothing else.
    ///
    /// There were two layouts here and a threshold between them, so a collapse
    /// was a SWAP: the board jumped from one size to another while the drawer
    /// was still moving, and the whole sheet re-laid out underneath it. The
    /// host app's note on this is the one worth reading - a board laid out
    /// from the `style` prop cannot follow a resize, and re-presenting at the
    /// start of the transition is the "display rearranges right before the
    /// collapse" jump. Its answer is a continuous fraction of the HEIGHT, and
    /// this is the same answer with a tenth of the machinery, because this
    /// game has exactly one thing on the sheet that moves.
    ///
    /// Everything below is `lerp(collapsed, expanded, t)`. Nothing switches.
    private func openness(_ h: CGFloat) -> CGFloat {
        /* THE COLLAPSED END IS 360, above the tallest compact drawer Messages
         * hands out (340, 323 with the keyboard up). The window used to start
         * at 270, so the real compact height sat 18% of the way open and drew
         * the headline as a ghost over the board. */
        let lo: CGFloat = 360, hi = Self.collapsedCeiling + 130
        let x = min(1, max(0, (h - lo) / (hi - lo)))
        return x * x * (3 - 2 * x)          // smoothstep, so the ends settle
    }

    private func lerp(_ a: CGFloat, _ b: CGFloat, _ t: CGFloat) -> CGFloat {
        a + (b - a) * t
    }

    private func sheet(_ size: CGSize) -> some View {
        let t = openness(size.height)

        /* THE COLLAPSED STRIP PUTS THINGS BESIDE THE BOARD AND THE EXPANDED
         * SHEET PUTS THEM ABOVE AND BELOW IT, so the two ends are the same
         * four things in a different arrangement rather than two layouts.
         * The board is centred in what the four leave, and every number is
         * one lerp. Nothing switches, so a collapse is a resize. */
        let vpad = lerp(Self.vmargin, Self.margin, t)
        let top  = lerp(0, Self.barHeight, t)       // the bar, when there is one
        let bot  = lerp(0, Self.doorSide + 6, t)    // the row the door sits in
        let col  = lerp(Self.column, 0, t)          // the "you are" column
        let gut  = lerp(0, 3, t)
        let door = lerp(Self.doorCollapsed, Self.doorSide, t)
        let icon = lerp(34, 46, t)

        /* THE LINES STOP ON THE SHEET: the main lines run 5% past the board
         * (UI.html), so the width the board may take leaves room for them -
         * on a Pro Max the tips land about 16 points in, as in the spec. */
        let avail = size.height - 2 * vpad - top - bot
        let side = max(0, min((size.width - 2 * (Self.margin + gut) - 2 * col)
                                  / (1 + 2 * Uttt.boardReach),
                              avail))

        /* THE EXPANDED BOARD SITS HIGH, NOT CENTRED (UI.html "Expanded, on
         * four real phones": the board just under the header and the spare
         * height all at the bottom with the doors). Collapsed it is centred
         * in what is left, as it always was; the offset is one lerp between
         * the two, so a collapse is still a resize and nothing switches. */
        let free = max(0, avail - side)
        let lift = lerp(free / 2, min(free, Self.boardGap), t)

        return board
            .frame(width: side, height: side)
            .padding(.top, top + lift)
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
            .padding(.bottom, bot)
            .overlay(alignment: .topLeading) {
                indicator(icon: icon, lead: lerp(3, 4, t))
                    .frame(width: max(col, icon + 2), alignment: .leading)
                    .padding(.top, lerp(4, 0, t))
            }
            .overlay(alignment: .topTrailing) {
                VStack(alignment: .trailing, spacing: 3) {
                    headlineView
                    /* The line under it: where you sent them, or at the end
                     * the winning line spoken (docs/UI.html 04, 06, 07). */
                    if !model.subline.isEmpty {
                        Text(model.subline)
                            .font(.system(size: 14))
                            .foregroundStyle(UtttInk.muted)
                            .lineLimit(1)
                    }
                }
                .opacity(Double(t)).allowsHitTesting(t > 0.5)
            }
            .overlay(alignment: .bottomTrailing) {
                HStack(alignment: .center, spacing: 10) {
                    if let title = UtttDoorButton.title(self.door), t > 0.5 {
                        UtttDoorButton(title: title, height: door, act: onDoor)
                            .opacity(Double((t - 0.5) * 2))
                    }
                    UtttRulebookButton(side: door) { rulesOpen = true }
                }
            }
            .padding(.horizontal, Self.margin)
            .padding(.vertical, vpad)
    }

    /// THE BAR'S LINE, with the other side drawn rather than spelled.
    ///
    /// The mark is sized to the CAP HEIGHT of the type beside it, not to the
    /// line box, or it sits low and reads as a separate object; and it is
    /// nudged down by a point because a drawn circle's optical centre is not
    /// its bounding box's. `.firstTextBaseline` does the rest.
    @ViewBuilder private var headlineView: some View {
        let ink = Uttt.over == .none ? Self.ink : Self.blue
        HStack(alignment: .firstTextBaseline, spacing: 0) {
            switch model.headline {
            case .text(let t):
                headlineText(t, ink)
            case .mark(let before, let m, let after):
                if !before.isEmpty { headlineText(before, ink) }
                UtttMarkIcon(mark: m, seed: model.seed &* 31 &+ 7)
                    .frame(width: 21, height: 21)
                    .alignmentGuide(.firstTextBaseline) { $0.height - 2 }
                if !after.isEmpty { headlineText(after, ink) }
            }
        }
        .lineLimit(1)
    }

    private func headlineText(_ t: String, _ ink: Color) -> some View {
        Text(t)
            .font(.system(size: 21, weight: .bold))
            .tracking(-0.315)              // -.015em
            .foregroundStyle(ink)
    }

    /// "you are" over a 46-point mark, which is 19 points of label, a 4-point
    /// lead and the mark.
    private static let barHeight: CGFloat = 72
    private static let doorSide = UtttRulebookButton.expandedSide

    /// The air between the bar and the expanded board: 12 points plus the
    /// 5% the main lines run above the board, so the tips clear the "you
    /// are" mark. UI.html centres that mark in a row above the board; the
    /// owner keeps it under its label, as on the strip, so the row goes and
    /// the board comes up under the bar.
    private static let boardGap: CGFloat = 30

    // MARK: the pieces

    private var board: some View {
        UtttLiveBoard(clock: model.clock, positionKey: model.positionKey,
                      onTap: { model.tap(at: $0) })
    }

    /// THE SIDE INDICATOR IS A DRAWN MARK, not a glyph - the same X that is
    /// about to land on the board, out of the same pen. Setting it in a font
    /// made it the only thing in the frame that did not come off the nib.
    private func indicator(icon: CGFloat, lead: CGFloat) -> some View {
        VStack(spacing: 0) {
            // Two lines, set on a 9.5-point body - line-height 1, so they read
            // as one two-line label rather than two labels.
            VStack(spacing: -2.8) {
                line(Uttt.say(.youAre1))
                line(Uttt.say(.youAre2))
            }
            UtttMarkIcon(mark: model.you, seed: model.seed &+ 4)
                .frame(width: icon, height: icon)
                .padding(.top, lead)
        }
    }

    private func line(_ s: String) -> some View {
        Text(s)
            .font(.system(size: 9.5, weight: .semibold))
            .tracking(1.9)                     // .2em
            .textCase(.uppercase)
            .foregroundStyle(Self.label)
    }
}
