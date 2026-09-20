import SwiftUI

/// The one door on the expanded sheet.
///
/// A SQUARE WHOSE EDGE AND FILL ARE BOTH ROUGH. It sits on the same piece of
/// paper as the board and is drawn by the same hand, so a flat rectangle would
/// be the only printed thing in the frame - and the glyph on it is filled with
/// its own hachure at about 55% because a bright solid book would fight the
/// fill underneath it.
///
/// All of that is `uttt_rule.c`, like every other drawn thing in this app.
/// This file fills polygons and computes no coordinate: it hands the kernel
/// the size the button has - which the kernel needs, because a hachure gap is
/// a whole number of points and the shape is therefore not scale-free - and
/// fills what comes back.
public struct UtttRulebookButton: View {
    public let side: CGFloat
    private let action: () -> Void

    public init(side: CGFloat = 54, action: @escaping () -> Void = {}) {
        self.side = side
        self.action = action
    }

    public var body: some View {
        Button(action: action) {
            Canvas { ctx, size in
                UtttBoard.fill(Uttt.rulebook(w: size.width, h: size.height),
                               into: ctx, side: min(size.width, size.height))
            }
            .frame(width: side, height: side)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityLabel("Rulebook")
    }
}
