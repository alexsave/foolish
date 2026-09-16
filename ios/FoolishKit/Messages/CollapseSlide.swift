// CollapseSlide - how far the collapse has pushed the board down, published so
// the board can take it back off the half of itself that does not want it.
//
// The auto-collapse moves two groups of things and they want opposite
// treatment, which is the owner's call on round 47:
//
//   THE GREEN LINE - own hand, action pills, undo, settings. These sit on the
//   box's BOTTOM edge, which is the drawer's position plus our height, so they
//   carry the whole of the collapse's judder: our height is a render old on a
//   third of the composited frames and the edge lands a frame of drawer travel
//   away. They are carried by a Core Animation keyframe on the hosting layer
//   instead, which the render server evaluates on every frame it composites -
//   ours or not - so they do not move at all. See `CollapseTween.slideOffsets`.
//
//   THE RED LINE - table cards, deck, discard, the opponent ring. These sit on
//   the box's TOP edge, which is the drawer's own descending edge, and the
//   drawer is smooth: filmed with our height held constant the top bar runs
//   420.0, 450.3, 479.7, 509.3 with no reversal at all. They were never the
//   problem, and they should keep riding the drawer down exactly as they did.
//
// The layer animation moves the whole hosting view, so the red group gets it
// too and has to have it removed. This is that number: the board offsets the
// red group UP by it, cancelling the slide for those views only.
//
// Deliberately an ordinary SwiftUI value at the app's own frame rate, not
// another layer animation. A cancellation that is a frame stale leaves the red
// group exactly where the old design left it, and the old design is what the
// owner asked to keep for them - so the staleness here is not a defect to be
// engineered away, it IS the specified behaviour.
import CoreGraphics
import SwiftUI

public struct CollapseSlideKey: EnvironmentKey {
    public static let defaultValue: CGFloat = 0
}

public extension EnvironmentValues {
    /// Points the collapse's slide currently has the board pushed down by, or
    /// zero when no slide is running.
    var collapseSlide: CGFloat {
        get { self[CollapseSlideKey.self] }
        set { self[CollapseSlideKey.self] = newValue }
    }
}
