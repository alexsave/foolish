#if UTTT_BIG_BOARD
import UIKit

/// THE 243 BOARD'S SCREENS (docs/BIG_BOARD.md): the play surface, the
/// spectator's view of it, and the wait for my own invitation. UIKit only,
/// on the 9 x 9 screens' paper (UtttSheetView, with no auto-collapse ride).
///
/// The layout is NOT the kernel's `uti_sheet`: a header band (who I am, the
/// headline, room at the right for the send hint), the board in the
/// rest, and the doors at the bottom (the host's "243" badge in the bottom
/// left corner). Every number is in `UtttBigLayout`.

/// Every number on the big screens, in one place.
enum UtttBigLayout {
    /// The sheet's margin.
    static let pad: CGFloat = 12
    /// The header band's height.
    static let header: CGFloat = 64
    /// The "you are" mark's side, and the gap between the label and it.
    static let mark: CGFloat = 26
    static let markLead: CGFloat = 3
    /// Between the indicator and the headline.
    static let gap: CGFloat = 12
    /// Room left at the header's right for the send hint, which points at
    /// Messages' Send button from the top right corner while a bubble is staged.
    static let hintRoom: CGFloat = 72
    /// The doors' side: the 9 x 9 rulebook's.
    static var door: CGFloat { UtttRulebookButton.expandedSide }
    /// The Again bar's widest.
    static let againMax: CGFloat = 220
    /// The region a board opens focused on is at least this deep (level 2 is
    /// a 27 x 27 block); a shallower one opens on the whole board.
    static let focusLevel = 2
}

/// What the big screen is allowed to know: the UtttModel analogue, over the
/// big resident (UtttBig). No clock: a big move draws in place.
@MainActor
public final class UtttBigModel {
    public let you: Uttt.Mark
    /// The position changed and this device moved in it: the host stages on
    /// this, as on UtttModel's.
    public private(set) var positionKey = 0 {
        didSet { if positionKey != oldValue { onPosition?() } }
    }
    public var onPosition: (() -> Void)?
    /// The screen's ear: something it draws changed.
    public var onChange: (() -> Void)?

    /// True while this device's last move is staged and unsent: its cell is
    /// drawn as the draft, and another tap replaces it.
    public private(set) var pending = false {
        didSet { if pending != oldValue { onChange?() } }
    }

    public init(you: Uttt.Mark) { self.you = you }

    /// The host says whether a draft is outstanding.
    public func setPending(_ on: Bool) { pending = on }

    /// A tap on cell `cell`. A tap that is not a move does nothing (the
    /// kernel refuses an illegal cell); a change of mind replaces the draft.
    public func tap(_ cell: Int) {
        if pending {
            guard UtttBig.canReplace(cell), UtttBig.undoMine() else { return }
            guard UtttBig.playAsMe(cell) else {
                UtttLog.fault("big", "canReplace said yes to \(cell) and play refused it")
                return
            }
        } else {
            guard UtttBig.over == .none, UtttBig.canMove, UtttBig.playAsMe(cell) else { return }
        }
        UtttLog.note("big", "played \(cell)")
        positionKey &+= 1
        onChange?()
    }

    /// The position was loaded behind the model's back: look again.
    public func refresh() { onChange?() }
}

/// The big board's play surface (and, not interactive, the spectator's).
public final class UtttBigGameScreen: UtttSheetView {
    private let model: UtttBigModel
    private let interactive: Bool
    private let board = UtttBigBoardView(frame: .zero)
    private let indicator = UIView()
    private let you1 = UILabel()
    private let you2 = UILabel()
    private let youMark: UtttInkView
    private let headline = UtttHeadlineView()
    private let rulebook: UtttRulebookButton
    private var again: UtttDoorButton?
    private var opened = false

    /// DEBUG: a zoom the board opens at instead of its own focus (the rig's
    /// `dev.bigzoom`), centred on the region.
    public var openingZoom: CGFloat?

    public init(model: UtttBigModel, interactive: Bool, onDoor: @escaping () -> Void = {},
                onRules: @escaping () -> Void = {}, onDiagnostics: (() -> Void)? = nil,
                onLongHold: (() -> Void)? = nil) {
        self.model = model
        self.interactive = interactive
        youMark = UtttInkView.mark(model.you, look: UtttBig.look)
        rulebook = UtttRulebookButton(act: onRules, onHold: onDiagnostics)
        super.init(slide: nil)
        rulebook.setLongHold(onLongHold)
        if interactive {
            board.onTap = { [weak model] cell in model?.tap(cell) }
        }
        content.addSubview(board)
        you1.attributedText = UtttType.small.text(UtttBig.say(.youAre1))
        you2.attributedText = UtttType.small.text(UtttBig.say(.youAre2))
        indicator.addSubview(you1)
        indicator.addSubview(you2)
        indicator.addSubview(youMark)
        indicator.isAccessibilityElement = true
        indicator.accessibilityLabel = UtttBig.say(.youAreSpoken)
        indicator.isHidden = model.you == .none
        content.addSubview(indicator)
        headline.isAccessibilityElement = true
        headline.accessibilityTraits = .header
        content.addSubview(headline)
        if UtttBig.door == .again {
            let a = UtttDoorButton(title: UtttBig.say(.doorAgain), act: onDoor)
            content.addSubview(a)
            again = a
        }
        content.addSubview(rulebook)
        model.onChange = { [weak self] in self?.changed() }
        pushBoard()
    }
    required init?(coder: NSCoder) { fatalError() }

    private func changed() {
        pushBoard()
        setNeedsLayout()
    }

    /// The resident big game onto the board: one snapshot, the zoom kept.
    private func pushBoard() {
        var nodes = [UInt8](repeating: 0, count: UtttBigBoardView.nodeCount)
        for id in 0..<nodes.count { nodes[id] = UtttBig.node(id).rawValue }
        let region = UtttBig.region
        board.set(cells: UtttBig.cells, nodes: nodes,
                  regionRect: region >= 0 ? UtttBig.nodeRect(region) : nil,
                  last: UtttBig.last, draft: model.pending ? UtttBig.last : -1)
    }

    override func lay(_ size: CGSize, from: CGFloat?) {
        typealias M = UtttBigLayout
        /* THE HEADER: "you are" over my mark at the left, the headline beside
         * it, the send hint's corner left free. */
        let a = you1.sizeThatFits(.zero), b = you2.sizeThatFits(.zero)
        let labelW = max(a.width, b.width)
        let stackW = max(labelW, M.mark)
        let stackH = a.height + b.height + M.markLead + M.mark
        indicator.frame = CGRect(x: M.pad, y: max(0, (M.header - stackH) / 2), width: stackW, height: stackH)
        you1.frame = CGRect(x: (stackW - a.width) / 2, y: 0, width: a.width, height: a.height)
        you2.frame = CGRect(x: (stackW - b.width) / 2, y: a.height, width: b.width, height: b.height)
        youMark.frame = CGRect(x: (stackW - M.mark) / 2, y: a.height + b.height + M.markLead,
                               width: M.mark, height: M.mark)

        let left = indicator.isHidden ? M.pad : indicator.frame.maxX + M.gap
        let width = max(0, size.width - left - M.hintRoom)
        let pre = UtttBig.say(.headlinePre), post = UtttBig.say(.headlinePost)
        let m = UtttBig.sayMark
        let said: UtttModel.Headline = m == .none ? .text(pre + post) : .mark(pre, m, post)
        let hs = headline.set(said, ink: UtttInk.ink, look: UtttBig.look, width: width,
                              column: false, align: .left)
        headline.accessibilityLabel = UtttBig.say(.headlineSpoken)
        headline.frame = CGRect(x: left, y: (M.header - hs.height) / 2, width: hs.width, height: hs.height)

        /* THE BOARD IN THE REST, the doors at the bottom: in the board's side
         * margins when the area is wide enough to leave them, else in a
         * footer of their own. */
        let area = CGRect(x: 0, y: M.header, width: size.width, height: max(0, size.height - M.header))
        let footer = M.door + 2 * M.pad
        let wide = area.width - area.height >= 2 * (M.door + 2 * M.pad) && again == nil
        let boardRect = wide ? area : CGRect(x: area.minX, y: area.minY,
                                             width: area.width, height: max(0, area.height - footer))
        board.frame = boardRect
        rulebook.frame = CGRect(x: size.width - M.pad - M.door, y: size.height - M.pad - M.door,
                                width: M.door, height: M.door)
        if let again {
            let w = min(M.againMax, max(0, size.width - 3 * M.pad - M.door))
            again.frame = CGRect(x: rulebook.frame.minX - M.pad - w, y: rulebook.frame.minY,
                                 width: w, height: M.door)
        }

        if !opened, board.bounds.width > 0, board.bounds.height > 0 {
            opened = true
            board.layoutIfNeeded()
            open()
        }
    }

    /// The zoom a board opens at: on the region where the next mark must go
    /// when it is a block of 27 or smaller, else the whole board.
    private func open() {
        let region = UtttBig.region
        let rect = region >= 0 ? UtttBig.nodeRect(region) : CGRect(x: 0, y: 0, width: 1, height: 1)
        if let z = openingZoom, z > 1 {
            /* focus fits a rect to a third of the view: the rect that zoom
             * means, centred where the region is */
            let w = z.isFinite ? 1 / (3 * z) : 1 / CGFloat(3 * UtttBig.side)
            board.focus(on: CGRect(x: rect.midX - w / 2, y: rect.midY - w / 2, width: w, height: w),
                        animated: false)
            UtttLog.note("big", "opened at a set zoom \(z), zoom \(board.zoomScale)")
            return
        }
        if region >= 0, UtttBig.nodeLevel(region) >= UtttBigLayout.focusLevel {
            board.focus(on: rect, animated: false)
        }
    }
}

/// My own big invitation, nobody has joined: the kernel's words, no board.
public final class UtttBigLobby: UtttSheetView {
    private let words = UtttLobbyWords()

    public init() {
        super.init(slide: nil)
        content.addSubview(words)
    }
    required init?(coder: NSCoder) { fatalError() }

    override func lay(_ size: CGSize, from: CGFloat?) {
        typealias M = UtttBigLayout
        words.frame = CGRect(x: M.pad, y: M.pad, width: max(0, size.width - M.pad - M.hintRoom),
                             height: max(0, size.height - 2 * M.pad))
        words.set(UtttBig.say(.waitingHeadline), UtttBig.say(.waitingSubline), column: false)
    }
}
#endif
