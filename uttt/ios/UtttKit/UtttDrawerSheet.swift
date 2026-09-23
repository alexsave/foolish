import CUttt
import SwiftUI

/// THE SHEET AT THE DRAWER'S HEIGHT, for every screen.
///
/// Every screen lays out at the kernel's drawer height (`UtttDrawerClock`, a
/// spring on the host's response toward the height Messages last handed),
/// laid out at once and never tweened by the host's animation, with the
/// ruler's edge bars over it. One container, so no screen can lay out at the
/// raw height or branch on it: the content is handed a size and asks
/// `Uttt.sheet` where everything goes.
struct UtttDrawerSheet<Content: View>: View {
    @ViewBuilder let content: (CGSize) -> Content

    @StateObject private var drawer = UtttDrawerClock()

    var body: some View {
        GeometryReader { geo in
            /* A NEW HEIGHT IS LAID OUT AT ONCE. Messages resizes the drawer
             * inside a UIKit animation block and the hosting controller
             * bridges that into SwiftUI: every element then crept on a slow
             * curve and landed in steps after the drawer had stopped
             * (measured with the ruler). Every number on the sheet is a
             * function of the height, so the height is the only animation. */
            let _ = drawer.frame
            let h = drawer.layout(for: geo.size.height)
            content(CGSize(width: geo.size.width, height: h))
                .frame(width: geo.size.width, height: h)
                .transaction(value: h) { $0.animation = nil }
        }
        .onGeometryChange(for: CGFloat.self) { $0.size.height } action: { drawer.report($0) }
        .overlay { MotionRulerEdges(on: UtttRuler.on) }
#if DEBUG
        /* With the ruler on, every height the sheet is handed, so a filmed
         * take can be read against what the layout was given. */
        .onGeometryChange(for: CGFloat.self) { $0.size.height } action: { h in
            if UtttRuler.on { UtttLog.note("ruler-height", String(format: "%.1f clock %d", h, MotionRuler.clockMs)) }
        }
#endif
    }
}

extension View {
    /// Put a view's top left at a point the kernel gave, in a sheet-sized box.
    func placed(x: Float, y: Float) -> some View {
        offset(x: CGFloat(x), y: CGFloat(y))
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
    }

    /// WRAPPED AT SPACES ONLY: as many lines as the words, so a line breaks
    /// between words and a word wider than the column scales down to it
    /// rather than breaking mid-word ("Diagona / l"). One line in the band.
    func wordsWrap(_ s: String, column: Bool) -> some View {
        lineLimit(column ? max(1, s.split(separator: " ").count) : 1)
            .minimumScaleFactor(0.5)
    }

    /// Set in the layout's box of words: `x, y, w, h` from `UtiSheet.words`,
    /// the content wrapped to its width and pinned to `alignment`.
    func inWords(_ L: UtiSheet, alignment: Alignment) -> some View {
        frame(width: CGFloat(L.words.2), height: CGFloat(L.words.3), alignment: alignment)
            .placed(x: L.words.0, y: L.words.1)
    }
}
