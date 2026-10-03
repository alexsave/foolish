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

/// THE MODE'S DOOR (docs/BIG_BOARD.md): a finger held STILL on a grid for
/// `UtttBig.holdSeconds`, drifting no more than `UtttBig.holdSlop` points.
/// It fires once, the moment the hold is reached, and the touch need not be
/// on a playable cell.
///
/// IT TAKES NOTHING FROM THE BOARD'S OWN GESTURES: it cancels no touch,
/// delays no touch, and nothing waits for it to fail - so a tap stages a move
/// exactly as before, with no added delay. A double tap and a tap lift long
/// before 4 s; a pan or a drag moves past the slop; a pinch is two fingers.
/// Each makes this recogniser fail on its own. The one owner of these
/// settings: both boards (UtttBoardView, UtttBigBoardView) add this class.
final class UtttModeHold: UILongPressGestureRecognizer {
    var action: () -> Void

    init(_ action: @escaping () -> Void) {
        self.action = action
        super.init(target: nil, action: nil)
        addTarget(self, action: #selector(held))
        minimumPressDuration = UtttBig.holdSeconds
        allowableMovement = UtttBig.holdSlop
        cancelsTouchesInView = false
        delaysTouchesBegan = false
        delaysTouchesEnded = false
    }

    @objc private func held() {
        guard state == .began else { return }
        action()
    }

    /// Add the door to `view`, once; a second call only changes the action.
    static func install(on view: UIView, _ action: @escaping () -> Void) -> UtttModeHold {
        if let h = view.gestureRecognizers?.first(where: { $0 is UtttModeHold }) as? UtttModeHold {
            h.action = action
            return h
        }
        let h = UtttModeHold(action)
        view.addGestureRecognizer(h)
        return h
    }
}

/// THE "243" BADGE: the mode's visible confirmation, and on a big game's
/// screen what board this is, in the bottom left corner. The small label's type in the ink, on a
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

    /// The badge's place in a view of `bounds` (its safe area): the BOTTOM
    /// LEFT corner, `inset` in. Not the top right: the send hint points at
    /// Messages' Send button from there while a bubble is staged, and the
    /// two were drawn on top of each other (rig, 2026-10-01). The bottom left
    /// is empty on every screen at rest; the doors stand at the bottom right.
    public static let inset: CGFloat = 12
    public func place(in bounds: CGRect) {
        let s = intrinsicContentSize
        frame = CGRect(x: bounds.minX + Self.inset, y: bounds.maxY - Self.inset - s.height,
                       width: s.width, height: s.height)
    }
}
#endif
