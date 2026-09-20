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
    public init(model: UtttModel) { _model = StateObject(wrappedValue: model) }

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

    /// Anything taller than the taller of the two collapsed heights is the
    /// expanded sheet. There is no third size.
    private static let collapsedCeiling: CGFloat = 400

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
                } else if geo.size.height <= Self.collapsedCeiling {
                    collapsed(geo.size)
                } else {
                    expanded(geo.size)
                }
            }
        }
        .animation(.easeInOut(duration: 0.18), value: rulesOpen)
    }

    // MARK: collapsed

    /// THE COLUMNS NEVER MOVE AND THE BOARD GIVES WAY. The board is
    /// min(height - 26, width - 26 - 92): 257 on a 375-wide SE, 275 on 393,
    /// 284 on 402, and 314 on a 440 Pro Max where the 340 starts cutting it
    /// again. That is the right way round - a label that shifts by device is a
    /// label you have to find, and a board that shrinks is just a smaller
    /// board.
    ///
    /// No status line. The strip is the board, the side you are on, and the
    /// wash that says where you have been sent; a headline here would be the
    /// fourth thing in a frame that only has room for three.
    private func collapsed(_ size: CGSize) -> some View {
        let side = max(0, min(size.height - 2 * Self.vmargin,
                              size.width - 2 * Self.margin - 2 * Self.column))
        return ZStack(alignment: .topLeading) {
            board
                .frame(width: side, height: side)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            indicator(icon: 34, lead: 3)
                .frame(width: Self.column)
                .padding(.top, 4)
        }
        .padding(.horizontal, Self.margin)
        .padding(.vertical, Self.vmargin)
    }

    // MARK: expanded

    /// AND THE BOARD CANNOT USE THE EXTRA HEIGHT. It is square and the width
    /// binds on every phone, so it tops out at 343 on an SE and 408 on a Pro
    /// Max while the sheet runs to 830 - the 289 points a Pro Max buys you are
    /// all vertical, and a square board has no use for vertical. It is centred
    /// in whatever the bar and the door leave.
    private func expanded(_ size: CGSize) -> some View {
        // The margin plus a three-point gutter either side: 343, 361, 370, 408
        // on the four phones, which are the design document's numbers.
        let byWidth = size.width - 2 * (Self.margin + 3)
        let byHeight = size.height - 2 * Self.margin - Self.barHeight - Self.doorSide
        let side = max(0, min(byWidth, byHeight))
        return VStack(spacing: 0) {
            HStack(alignment: .top, spacing: 8) {
                indicator(icon: 46, lead: 4).frame(width: 48)
                Spacer(minLength: 0)
                headlineView
            }
            .frame(height: Self.barHeight, alignment: .top)
            Spacer(minLength: 0)
            board.frame(width: side, height: side)
            Spacer(minLength: 0)
            HStack(spacing: 0) {
                Spacer(minLength: 0)
                UtttRulebookButton(side: Self.doorSide) { rulesOpen = true }
            }
        }
        .padding(Self.margin)
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
    private static let doorSide: CGFloat = 54

    // MARK: the pieces

    private var board: some View {
        UtttBoard(active: model.active, last: model.last,
                  positionKey: model.positionKey,
                  animating: model.animating,
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
                line("you")
                line("are")
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
