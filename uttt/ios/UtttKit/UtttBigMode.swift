#if UTTT_BIG_BOARD
import UIKit

/// THE 243 MODE (docs/BIG_BOARD.md): while it is on, a game this device
/// CREATES is a 243 x 243 game. It changes nothing else - games already in a
/// thread are whatever their bubbles say.
///
/// Kept in the extension's own defaults (like UtttSeats), so it lasts until
/// it is held off again, and never on in a build where the big game is not
/// available (UtttBig.available: a debug build or a TestFlight install).
public enum UtttBigMode {
    private static let key = "uttt.big.mode"

    public static var on: Bool {
        get { UtttBig.available && UserDefaults.standard.bool(forKey: key) }
        set { UserDefaults.standard.set(newValue && UtttBig.available, forKey: key) }
    }

    /// Flip the mode, with a haptic: success turning on, warning turning off.
    /// Returns the new state (always false where the big game is unavailable).
    @MainActor
    @discardableResult
    public static func toggle() -> Bool {
        let now = !on
        on = now
        UINotificationFeedbackGenerator().notificationOccurred(on ? .success : .warning)
        return on
    }
}

/// THE "243" BADGE: the mode's visible confirmation, and on a big game's
/// screen what board this is. The small label's type in the ink, on a
/// paper-coloured pill.
public final class UtttBigBadge: UIView {
    private let label = UILabel()
    /// The pill's padding around the word, and its size.
    private static let padX: CGFloat = 7
    private static let padY: CGFloat = 3

    public init() {
        super.init(frame: .zero)
        isUserInteractionEnabled = false
        backgroundColor = UtttPaper.flat
        layer.borderColor = UtttInk.ink.withAlphaComponent(0.35).cgColor
        layer.borderWidth = 1
        var type = UtttType.small
        type.color = UtttInk.ink
        label.attributedText = type.text("243")
        addSubview(label)
        isAccessibilityElement = true
        accessibilityLabel = "243"
        bounds.size = intrinsicContentSize
    }
    required init?(coder: NSCoder) { fatalError() }

    public override var intrinsicContentSize: CGSize {
        let s = label.sizeThatFits(.zero)
        return CGSize(width: ceil(s.width) + 2 * Self.padX, height: ceil(s.height) + 2 * Self.padY)
    }

    public override func layoutSubviews() {
        super.layoutSubviews()
        layer.cornerRadius = bounds.height / 2
        let s = label.sizeThatFits(.zero)
        label.frame = CGRect(x: (bounds.width - s.width) / 2, y: (bounds.height - s.height) / 2,
                             width: s.width, height: s.height)
    }

    /// The badge's place in a view of `bounds` (its safe area): the top right
    /// corner, `inset` in.
    public static let inset: CGFloat = 8
    public func place(in bounds: CGRect) {
        let s = intrinsicContentSize
        frame = CGRect(x: bounds.maxX - Self.inset - s.width, y: bounds.minY + Self.inset,
                       width: s.width, height: s.height)
    }
}
#endif
