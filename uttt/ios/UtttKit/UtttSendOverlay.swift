import UIKit

/// WHAT STANDS OVER EVERY SCREEN while a bubble is on its way out: the send
/// hint and, when Messages never answered an insert, the send door.
///
/// It lives over the screens rather than in them because the extension swaps
/// whole screens (a move that ends the game swaps the board for the finished
/// one, Again swaps it for the lobby) and the bubble in the field does not
/// care which screen is up - the hint's fuse must keep burning across a swap,
/// which it can only do if nothing under it is torn down.
///
/// Every touch goes through to the screen under it except one on the send
/// door, the only thing here that is a control.
public final class UtttSendOverlay: UIView {
    /// A bubble is in the input field and nobody has sent it.
    public var staged = false { didSet { if staged != oldValue { update(restart: false) } } }
    /// The drawer is compact: the only place Messages' Send button is above us.
    public var compact = true { didSet { if compact != oldValue { update(restart: false) } } }
    /// Every insert went unanswered; the door inserts on a tap.
    public var door = false { didSet { if door != oldValue { showDoor() } } }
    /// A replacement bubble restarts the fuse.
    public func restart() { update(restart: true) }

    /// Whether the overlay's layer is up (see the host's `hideHintNow`).
    public var hintLayerShown = true
    /// Called the frame the drawer is laid out taller than it rested: a drag
    /// on the handle hands a new height every frame, and willTransition only
    /// comes at the release.
    public var onGrow: (() -> Void)?
    public var rest: CGFloat = 0

    /// The hint's container starts this far below the drawer's top, the
    /// sister product's board inset: the arrow rests in the top margin and
    /// its crest reaches up toward Messages' Send button.
    static let hintTop: CGFloat = 14

    /// The band at the drawer's bottom the send door stands in: the door and
    /// the paper around it. The only part of the overlay that takes a touch.
    public static let doorStrip: CGFloat = UtttRulebookButton.expandedSide + 2 * doorPad
    static let doorPad: CGFloat = 10

    /* THE HINT (shared/swift/MessagesKit/SendHintView): the sister product's
     * arrow, bob, ring and axis; ours are the caption and the fuse, both the
     * kernel's. WHITE WORDS RINGED IN THE SEND BLUE (owner, 2026-09-23): a
     * white ring vanished into the paper and a black one read as a stamp.
     * DOWN IN THE FRAME IT IS SENT, or the drawer starts to grow - a fade
     * let it linger up to 2s on the film. */
    private let hint = SendHintView()
    private let strip = UIView()
    private let doorButton: UtttDoorButton

    public init(onDoor: @escaping () -> Void) {
        doorButton = UtttDoorButton(title: Uttt.say(.doorSend), act: onDoor)
        super.init(frame: .zero)
        backgroundColor = .clear
        hint.caption = Uttt.say(.sendHint)
        hint.fuse = Uttt.sendHintSeconds
        hint.whiteInk = true
        hint.hidesAtOnce = true
        addSubview(hint)
        /* THE SEND DOOR: every insert went unanswered, so the bubble never
         * reached the field. A tap inserts it again - by then the drawer is
         * presenting and Messages' gate lets it through
         * (docs/INSERT_GATING.md). The Again door's pen and type, on a strip
         * of paper so it reads over whatever board is under it. */
        strip.backgroundColor = UtttPaper.flat
        strip.addSubview(doorButton)
        strip.alpha = 0
        strip.isHidden = true
        addSubview(strip)
    }
    required init?(coder: NSCoder) { fatalError() }

    private func update(restart: Bool) {
        hint.update(staged: staged && !door, visible: compact, restart: restart)
    }

    private func showDoor() {
        update(restart: false)
        if door { strip.isHidden = false }
        UIView.animate(withDuration: 0.25, delay: 0, options: [.curveEaseInOut, .beginFromCurrentState]) {
            self.strip.alpha = self.door ? 1 : 0
        } completion: { _ in
            if !self.door { self.strip.isHidden = true }
        }
    }

    public override func layoutSubviews() {
        super.layoutSubviews()
        let h = bounds.height
        if rest == 0 || compact && h < rest { rest = h }
        if h > rest + 4 { onGrow?() } else if compact { rest = h }
        UIView.performWithoutAnimation {
            hint.frame = CGRect(x: 0, y: Self.hintTop, width: bounds.width, height: max(0, h - Self.hintTop))
            let s = Self.doorStrip
            strip.frame = CGRect(x: 0, y: h - s, width: bounds.width, height: s)
            doorButton.frame = CGRect(x: 16, y: Self.doorPad, width: max(0, bounds.width - 32),
                                      height: UtttRulebookButton.expandedSide)
        }
    }

    public override func hitTest(_ point: CGPoint, with event: UIEvent?) -> UIView? {
        guard door, point.y >= bounds.height - Self.doorStrip else { return nil }
        return super.hitTest(point, with: event)
    }
}
