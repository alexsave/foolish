import Combine
import CUttt
import SwiftUI

/// THE SHEET AT THE DRAWER'S HEIGHT, for every screen.
///
/// Every screen lays out at the height Messages hands it, at once, and never
/// tweened by the host's animation block (src/uttt_anim.h: a finger's
/// heights are where the drawer is, and a release or a tap to expand is the
/// host animating our view to the height it handed), with the ruler's edge
/// bars over it. One container, so no screen can lay out at the
/// raw height or branch on it: the content is handed a size and asks
/// `Uttt.sheet` where everything goes.
struct UtttDrawerSheet<Content: View>: View {
    /// The sheet's size, and the drawer height an auto-collapse slide started
    /// from while one runs (nil otherwise): a screen lays its riders out for
    /// it (see UtttGameScreen's words).
    @ViewBuilder let content: (CGSize, CGFloat?) -> Content

    /// The last height Messages handed, so a drop can be judged against it.
    @State private var handed: CGFloat = 0
    @Environment(\.collapseSlide) private var slide
    @State private var slideFrom: CGFloat?

    var body: some View {
        GeometryReader { geo in
            /* A NEW HEIGHT IS LAID OUT AT ONCE. Messages resizes the drawer
             * inside a UIKit animation block and the hosting controller
             * bridges that into SwiftUI: every element then crept on a slow
             * curve and landed in steps after the drawer had stopped
             * (measured with the ruler). Every number on the sheet is a
             * function of the height, so the height is the only animation.
             *
             * AN AUTO-COLLAPSE IS LAID OUT AT THE COMPACT HEIGHT FROM ITS
             * FIRST FRAME, and pushed by the slide (CollapseSlide): the frame
             * that first sees the drop never draws the spring's height. */
            let h = geo.size.height
            content(CGSize(width: geo.size.width, height: h), slideFrom)
                .frame(width: geo.size.width, height: h)
        }
        /* A DROP WHILE AN AUTO-COLLAPSE IS ARMED IS THE FLIP (CollapseSlide),
         * judged against the height before it. */
        .onGeometryChange(for: CGFloat.self) { $0.size.height } action: { h in
            _ = slide?.heard(h, after: handed)
            handed = h
        }
        .onReceive(slide?.$run.eraseToAnyPublisher() ?? Empty().eraseToAnyPublisher()) { r in
            slideFrom = r?.from
        }
        /* THE RULER'S TOP HALF RIDES THE DRAWER'S TOP, the bottom half the
         * sheet's bottom: through a slide they are two layers. */
        .overlay { MotionRulerEdges(on: UtttRuler.on, top: false).allowsHitTesting(false) }
        .overlay(alignment: .top) {
            if UtttRuler.on {
                MotionRulerEdges(on: true, bottom: false)
                    .collapseRide { _ in CollapseRidePose(dy: 0) }
                    .allowsHitTesting(false)
            }
        }
        /* NOTHING ON THE SHEET INHERITS AN ANIMATION, whatever the
         * transaction carries: Messages' UIKit block bridged into SwiftUI,
         * or the slide's own `run` published inside it. Every number here is
         * a function of the drawer height, and every motion is the drawer
         * clock's, the slide's (on the render server) or a view's own frame
         * clock. Stripping it only on a change of `h` left the flip's second
         * pass (`slideFrom` set, `h` unchanged) and the ruler's overlay
         * animated. TWO RULES, TWO VECTORS: this one is the sheet's own
         * graph; a rider's content sits in a nested host with a graph of its
         * own, which Messages' block reaches directly, and CollapseSlide's
         * rider root carries the same rule for it (that one is what moved
         * the board: 77 -> 29 -> 11 -> 4pt off centre, filmed). */
        .transaction { $0.animation = nil }
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
    /// THE BOARD HOLDS THE CENTRE AND SCALES ABOUT IT, on every screen: the
    /// board placed at the kernel's `L`, on a sheet-sized layer of its own
    /// that rides an auto-collapse (CollapseSlide) along the path the layout
    /// would walk - `at(s)` is the kernel's sheet for the drawer `s` points
    /// taller than `L`'s. One owner: the waiting and spectator screens had
    /// none, so the board after Again sat at its compact place through the
    /// whole slide, 207pt below the drawer's centre in its first frame
    /// (filmed with the ruler).
    func boardRide(_ L: UtiSheet, touches: Bool = false,
                   at: @escaping (CGFloat) -> UtiSheet) -> some View {
        let side = CGFloat(L.board.2)
        return frame(width: side, height: side)
            .boardRuler()
            .placed(x: L.board.0, y: L.board.1)
            .collapseRide(touches: touches) { s in
                let A = at(s)
                return CollapseRidePose(
                    dy: CGFloat(A.board.1 + A.board.2 / 2 - L.board.1 - L.board.2 / 2),
                    scale: side > 0 ? CGFloat(A.board.2) / side : 1,
                    pivot: CGPoint(x: CGFloat(L.board.0) + side / 2,
                                   y: CGFloat(L.board.1) + side / 2))
            }
    }

    /// The header's words, twice (the column beside the ink and the band
    /// across the top, UtttGameScreen), riding an auto-collapse with the
    /// top and crossfading on their layers: the column copy toward its
    /// alpha at each height, the band copy set as the slide's first frame
    /// had it (`B`) and fading out.
    func wordsRide(column: Bool, _ L: UtiSheet, from B: UtiSheet,
                   at: @escaping (CGFloat) -> UtiSheet) -> some View {
        collapseRide { s in
            let A = at(s)
            if column {
                return CollapseRidePose(dy: 0, alpha: L.words_alpha > 0
                                        ? CGFloat(A.words_alpha / L.words_alpha) : 1)
            }
            return CollapseRidePose(dy: 0, alpha: B.band_alpha > 0
                                    ? CGFloat(A.band_alpha / B.band_alpha) : 1)
        }
    }

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

    /// Set in the layout's COLUMN of words: `x, y, w, h` from
    /// `UtiSheet.words`, the content wrapped to its width and pinned to
    /// `alignment`, at the column copy's alpha.
    func inWords(_ L: UtiSheet, alignment: Alignment) -> some View {
        frame(width: CGFloat(L.words.2), height: CGFloat(L.words.3), alignment: alignment)
            .placed(x: L.words.0, y: L.words.1)
            .shown(L.words_alpha)
    }

    /// Set in the layout's BAND across the top, at the band copy's alpha.
    /// Every screen draws its words twice, once in each; the kernel shows a
    /// copy only where it fits, so neither is ever squeezed (uttt_sheet).
    func inBand(_ L: UtiSheet, alignment: Alignment) -> some View {
        frame(width: CGFloat(L.band.2), height: CGFloat(L.band.3), alignment: alignment)
            .placed(x: L.band.0, y: L.band.1)
            .shown(L.band_alpha)
    }

    /// Faded to `alpha`; out of VoiceOver and the touch path once it is
    /// mostly gone, so the one copy that shows is the one that is read.
    func shown(_ alpha: Float) -> some View {
        opacity(Double(alpha))
            .allowsHitTesting(alpha > 0.5)
            .accessibilityHidden(alpha < 0.5)
    }
}

public extension CollapseSlide {
    /// The auto-collapse's slide, on the kernel's curve and numbers
    /// (uttt_anim.h UTTT_COLLAPSE_*): the host's spring, pushed from the
    /// whole travel to nothing.
    static func uttt() -> CollapseSlide {
#if DEBUG
        if UtttRuler.on { CollapseSlide.probe = { UtttLog.note("slide", $0) } }
#endif
        return CollapseSlide(duration: Double(uti_collapse_ms()) / 1000,
                      steps: Int(uti_collapse_steps()),
                      flip: CGFloat(uti_collapse_flip())) { travel, t in
            CGFloat(uti_collapse_push(Float(travel), Int32((t * 1000).rounded())))
        }
    }
}
