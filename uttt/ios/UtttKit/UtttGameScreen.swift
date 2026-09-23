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

    /// The verdict's box as set, so the kernel can fit the board around it.
    @State private var words: CGSize = .zero

    /// The air between the words and the board, below the words' own lines.
    private static let wordsAir: CGFloat = 6

    public var body: some View {
        UtttSheet {
            if rulesOpen {
                UtttRulesSheet { rulesOpen = false }
                    .padding(Self.margin)
                    .transition(.opacity)
            } else {
                UtttDrawerSheet { size in sheet(size) }
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
    private func sheet(_ size: CGSize) -> some View {
        /* AT THE END THE STRIP CARRIES THE VERDICT (UI.html 08: "the verdict
         * and the board"), top right where the expanded sheet puts it; the
         * board keeps its centre and gives up only what it must to clear it.
         * While the game runs the strip carries no words - the wash says it -
         * and the headline fades in with the bar. */
        let end = Uttt.over != .none
        let box = end ? CGSize(width: words.width, height: words.height + Self.wordsAir) : .zero
        let L = Uttt.sheet(.play, size: size, words: box)
        let r = UtttRuler.on
        let hpad = CGFloat(L.hpad), vpad = CGFloat(L.vpad)
        let icon = CGFloat(L.icon)

        return board
            .frame(width: CGFloat(L.board.2), height: CGFloat(L.board.2))
            .boardRuler()
            .placed(x: L.board.0, y: L.board.1)
            .overlay(alignment: .topLeading) {
                indicator(icon: icon, lead: CGFloat(L.icon_lead))
                    .frame(width: max(CGFloat(L.col), icon + 2), alignment: .leading)
                    .padding(.leading, hpad)
                    .padding(.top, vpad + CGFloat(L.icon_top))
            }
            .overlay(alignment: .topTrailing) {
                VStack(alignment: .trailing, spacing: 3) {
                    headlineView
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
                            .lineLimit(1)
                            .motionSquare(.lime, on: r)
                    }
                }
                .fixedSize()
                .measured($words)
                .opacity(Double(L.words_alpha)).allowsHitTesting(L.words_alpha > 0.5)
                .accessibilityHidden(L.words_alpha < 0.5)
                .padding(.trailing, hpad)
                .padding(.top, vpad)
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
                .motionSquare(.orange, on: UtttRuler.on)
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
