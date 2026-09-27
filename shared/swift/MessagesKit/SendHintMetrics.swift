// SendHintMetrics - the send hint's numbers, once, for both of its renderers:
// SendHint.swift (SwiftUI, the sister product) and SendHintView.swift (UIKit
// and Core Animation, uttt). Why each is what it is is written at SendHint.

import CoreGraphics

public enum SendHintMetrics {
    /// Messages' Send circle's centre from the screen's trailing edge.
    public static let axisFromScreenTrailing: CGFloat = 42
    /// How long a staged bubble sits unsent before the hint appears.
    public static let defaultFuse: Double = 3
    /// The arrow, stretched: "taller AND a bit wider" are two numbers.
    public static let arrowSize = CGSize(width: 21, height: 29)
    /// Peak-to-trough travel of the bob.
    public static let bobTravel: CGFloat = 14
    /// Seconds per bob.
    public static let bobPeriod: Double = 0.85
    /// Where the hint rests, down from the top of its container: negative,
    /// lifted into the drawer's top margin toward the Send button.
    public static let crestRoom: CGFloat = -9
    /// The stamped ring's radius, shared by the arrow and the caption.
    public static let ringRadius: CGFloat = 1.6
    /// The caption's size; its face is the system's, made heavy.
    public static let captionSize: CGFloat = 15
}
