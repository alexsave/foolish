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

    /// TWO 46-POINT COLUMNS, ONE EITHER SIDE, in the collapsed strip. The left
    /// one carries "you are" and the right one carries nothing - it exists so
    /// the board sits in the middle of the SHEET rather than in the middle of
    /// what is left over, which are different places and the eye knows it.
    private static let column: CGFloat = 46

    /// Anything taller than the taller of the two collapsed heights is the
    /// expanded sheet. There is no third size.
    private static let collapsedCeiling: CGFloat = 400

    private static let label = Color(red: 0.541, green: 0.522, blue: 0.467) // #8a8577
    private static let ink   = Color(red: 0.114, green: 0.106, blue: 0.086) // #1d1b16
    private static let blue  = Color(red: 0.145, green: 0.216, blue: 0.420) // #25376b

    public var body: some View {
        UtttSheet {
            GeometryReader { geo in
                if geo.size.height <= Self.collapsedCeiling {
                    collapsed(geo.size)
                } else {
                    expanded(geo.size)
                }
            }
        }
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
        let side = max(0, min(size.height - 2 * Self.margin,
                              size.width - 2 * Self.margin - 2 * Self.column))
        return ZStack(alignment: .topLeading) {
            board
                .frame(width: side, height: side)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            indicator(icon: 34, lead: 3)
                .frame(width: Self.column)
                .padding(.top, 4)
        }
        .padding(Self.margin)
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
                Text(model.headline)
                    .font(.system(size: 21, weight: .bold))
                    .tracking(-0.315)              // -.015em
                    .lineLimit(1)
                    .foregroundStyle(Uttt.over == .none ? Self.ink : Self.blue)
            }
            .frame(height: Self.barHeight, alignment: .top)
            Spacer(minLength: 0)
            board.frame(width: side, height: side)
            Spacer(minLength: 0)
            HStack(spacing: 0) {
                Spacer(minLength: 0)
                // TODO: the rulebook itself is not built. The door is drawn
                // because the sheet has one; it opens on nothing yet.
                UtttRulebookButton(side: Self.doorSide) {}
            }
        }
        .padding(Self.margin)
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
