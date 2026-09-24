import CUttt
import UIKit

/// Everything that is not the game: the wait, and a bubble this build cannot
/// read.
///
/// THERE IS NO "TAKE A SEAT" SCREEN. The joiner is X and moves first, so
/// opening somebody's invitation opens the board with the first move on it -
/// the join IS the move. And there is no "start" screen either: opening the
/// app through the + menu is the invitation, staged there and then.
///
/// docs/UI.html, "Lobby and end", 02: a headline, a line under it and the
/// empty board. WITHOUT the spec's "Take it back" door: the owner decided
/// (2026-09-22) there is no undo and no take-back - the draft's own X is the
/// only way back. Every word is the kernel's (uttt_say.h).
///
/// ONE LAYOUT AT EVERY HEIGHT, the play surface's (`Uttt.sheet`): the board's
/// centre is the sheet's centre and it scales with the drawer, with the words
/// at the top left and the board clear of them - beside them when it is small
/// enough, under them otherwise.
public final class UtttLobbyScreen: UtttSheetView {

    public enum Stance: Equatable {
        /// You sent the board. Nobody has answered it.
        case waiting
        /// A bubble this build cannot read.
        case unreadable
    }

    public let stance: Stance
    /* THE EMPTY BOARD, with no wash: nobody is on move, and a board tinted
     * corner to corner reads as a different piece of paper. */
    private let board = UtttBoardView(clock: nil)
    private let column = UtttLobbyWords()
    private let band = UtttLobbyWords()

    public init(stance: Stance, slide: CollapseSlide?) {
        self.stance = stance
        super.init(slide: slide)
        board.isUserInteractionEnabled = false
        board.accessibilityElementsHidden = true
        board.isHidden = stance == .unreadable
        content.addSubview(board)
        content.addSubview(column)
        content.addSubview(band)
    }
    required init?(coder: NSCoder) { fatalError() }

    /* NO MARK ON THE WAITING SCREEN: the joiner will be X, and until somebody
     * joins there is nobody to be anything. */
    private var headline: String {
        switch stance {
        case .waiting:    return Uttt.say(.waitingHeadline)
        case .unreadable: return Uttt.say(.unreadableHeadline)
        }
    }

    private var subline: String {
        switch stance {
        case .waiting:    return Uttt.say(.waitingSubline)
        case .unreadable: return Uttt.say(.unreadableSubline)
        }
    }

    override func lay(_ size: CGSize, from: CGFloat?) {
        let L = Uttt.sheet(.wait, size: size)
        let at = { (s: CGFloat) -> UtiSheet in
            Uttt.sheet(.wait, size: CGSize(width: size.width, height: size.height + s))
        }
        let B = from.map { Uttt.sheet(.wait, size: CGSize(width: size.width, height: $0)) } ?? L
        placeBoard(board, L, at: at)
        /* THE WORDS GO WHERE THE BOARD LEAVES ROOM: in the column beside it
         * on the strip, wrapped onto as many lines as that takes, and across
         * the top once the sheet opens, where UI.html 02 sets them. */
        column.frame = CGRect(x: CGFloat(L.words.0), y: CGFloat(L.words.1),
                              width: CGFloat(L.words.2), height: CGFloat(L.words.3))
        column.set(headline, subline, column: true)
        band.frame = CGRect(x: CGFloat(B.band.0), y: CGFloat(B.band.1),
                            width: CGFloat(B.band.2), height: CGFloat(B.band.3))
        band.set(headline, subline, column: false)
        placeWords(column: column, band: band, L, B, at: at)
    }
}

/// The lobby's headline and the line under it, from the top left.
final class UtttLobbyWords: UIView {
    private let headline = UILabel()
    private let subline = UILabel()

    override init(frame: CGRect) {
        super.init(frame: frame)
        layer.actions = UtttLayers.still
        addSubview(headline)
        addSubview(subline)
        headline.accessibilityTraits = .header
    }
    required init?(coder: NSCoder) { fatalError() }

    func set(_ h: String, _ s: String, column: Bool) {
        let w = bounds.width
        let hs = headline.set(h, .headline, width: w, column: column, align: .left)
        headline.frame = CGRect(origin: .zero, size: hs)
        let ss = subline.set(s, .subline, width: w, column: column, align: .left)
        subline.frame = CGRect(x: 0, y: hs.height + 3, width: ss.width, height: ss.height)
    }
}

/// The board, from outside the roster.
///
/// A spectator gets the game and no way to touch it, which is the whole
/// difference - so this is the play surface with the door taken off rather
/// than a screen of its own. At the end it does get the one door everybody
/// gets: Again. The rules open as they do on the play surface.
public final class UtttWatchScreen: UtttSheetView {
    private let model: UtttModel
    private let board = UtttBoardView(clock: nil)
    private let column = UIView()
    private let colLabel = UILabel()
    private let colSaid = UILabel()
    private let band = UIView()
    private let bandLabel = UILabel()
    private let bandSaid = UILabel()
    private let rulebook: UtttRulebookButton
    private var again: UtttDoorButton?

    public init(model: UtttModel, door: Uttt.Door = .none, slide: CollapseSlide?,
                onDoor: @escaping () -> Void = {}, onRules: @escaping () -> Void = {}) {
        self.model = model
        rulebook = UtttRulebookButton(act: onRules)
        super.init(slide: slide)
        content.addSubview(board)
        for (v, a, b) in [(column, colLabel, colSaid), (band, bandLabel, bandSaid)] {
            v.addSubview(a)
            v.addSubview(b)
            b.accessibilityTraits = .header
            content.addSubview(v)
        }
        if let title = UtttDoorButton.title(door) {
            let a = UtttDoorButton(title: title, act: onDoor)
            content.addSubview(a)
            again = a
        }
        content.addSubview(rulebook)
        model.onChange = { [weak self] in self?.setNeedsLayout() }
    }
    required init?(coder: NSCoder) { fatalError() }

    /// The play surface's one layout (`Uttt.sheet`), with the header line in
    /// place of the bar: the board centred and scaled with the drawer, the
    /// rulebook in the right column on the strip and beside Again at the
    /// bottom when expanded.
    override func lay(_ size: CGSize, from: CGFloat?) {
        let L = Uttt.sheet(.watch, size: size)
        board.active = model.active
        board.last = model.last
        board.positionKey = model.positionKey
        placeBoard(board, L) { s in
            Uttt.sheet(.watch, size: CGSize(width: size.width, height: size.height + s))
        }
        let label = Uttt.say(.watchLabel), said = Uttt.say(.watchLine)
        let saidType = UtttType(size: 17, weight: .bold, color: UtttInk.ink)
        /* On the strip the label over the line in the left column, the line
         * wrapped; opening, the two across the top band. */
        column.frame = CGRect(x: CGFloat(L.words.0), y: CGFloat(L.words.1),
                              width: CGFloat(L.words.2), height: CGFloat(L.words.3))
        let w = column.bounds.width
        let ls = colLabel.set(label, .small, width: w, column: false, align: .left)
        colLabel.frame = CGRect(origin: .zero, size: ls)
        let ss = colSaid.set(said, saidType, width: w, column: true, align: .left)
        colSaid.frame = CGRect(x: 0, y: ls.height + 3, width: ss.width, height: ss.height)
        shown(column, L.words_alpha)

        band.frame = CGRect(x: CGFloat(L.band.0), y: CGFloat(L.band.1),
                            width: CGFloat(L.band.2), height: CGFloat(L.band.3))
        let bw = band.bounds.width
        let bs = bandSaid.set(said, saidType, width: bw, column: false, align: .right)
        let bl = bandLabel.set(label, .small, width: max(0, bw - bs.width - 8), column: false, align: .left)
        /* on one first baseline, the label at the left, the line at the right */
        let saidFont = saidType.font(), labelFont = UtttType.small.font()
        bandSaid.frame = CGRect(x: bw - bs.width, y: 0, width: bs.width, height: bs.height)
        bandLabel.frame = CGRect(x: 0, y: saidFont.ascender - labelFont.ascender,
                                 width: bl.width, height: bl.height)
        shown(band, L.band_alpha)

        /* Again belongs to the expanded view (UI.html 08); the rulebook
         * stands beside it at its height, and alone in the right column on
         * the strip. */
        let d = CGFloat(L.door), hpad = CGFloat(L.hpad), vpad = CGFloat(L.vpad)
        let y = size.height - vpad - d
        rulebook.frame = CGRect(x: size.width - hpad - d, y: y, width: d, height: d)
        if let again {
            again.frame = CGRect(x: 0, y: y, width: max(0, size.width - hpad - d - 10), height: d)
            again.alpha = CGFloat(L.door_alpha)
            again.isHidden = L.door_alpha <= 0
        }
    }
}
