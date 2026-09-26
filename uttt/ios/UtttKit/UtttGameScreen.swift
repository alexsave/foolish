import CUttt
import UIKit

/// The play surface, at every height Messages gives it.
///
/// MESSAGES CHOOSES THE SIZE AND THIS FILE ONLY RECOGNISES IT. Collapsed is
/// 340 points tall - 323 while the compose field holds the first responder -
/// and the width is the phone's. Expanded is the screen less about 126 points
/// of Messages chrome: 541 on an SE, 726 on a 16, 748 on a 16 Pro, 830 on a
/// Pro Max. Nothing here asks to be any size; it is handed one and every
/// number on it is the kernel's (`Uttt.sheet`, uttt_anim.c).
///
/// docs/UI.html, "What holds which edge": the header holds the top, the doors
/// the bottom, and the board THE CENTRE - "348 to 214 is a scale, not a
/// slide". So the board's centre is the sheet's at every height, and
/// everything else is placed at an edge around it.
public final class UtttGameScreen: UtttSheetView {
    private let model: UtttModel
    private let door: Uttt.Door
    private let onDoor: () -> Void
    /// THE RULES ARE A SHEET OF THEIR OWN over the game, so a swipe down on
    /// them closes the rules, not the Messages drawer. The host presents it.
    private let onRules: () -> Void

    private let board: UtttBoardView
    private let indicator = UIView()
    private let youAre = UIView()
    private let you1 = UILabel()
    private let you2 = UILabel()
    private let youMark: UtttInkView
    private let column = UtttWordsView(align: .right)
    private let band = UtttWordsView(align: .right)
    private let rulebook: UtttRulebookButton
    private var again: UtttDoorButton?
    /// THE REPLAY LINK at the end, beside Again: the kernel's URL for the
    /// finished game onto the pasteboard (an iMessage extension can open
    /// only its own container's scheme - foolish's replay row), and the door
    /// then reads as its own receipt.
    private var copy: UtttDoorButton?

    /// `door` is the kernel's answer for this board (utm_door) - at the end of
    /// a game, Again. It stands in the expanded view only (docs/UI.html 08:
    /// "starting a game from the strip you land on by accident is how you
    /// start a game by accident").
    public init(model: UtttModel, door: Uttt.Door = .none, slide: CollapseSlide?,
                onDoor: @escaping () -> Void = {}, onRules: @escaping () -> Void = {},
                onDiagnostics: (() -> Void)? = nil) {
        self.model = model
        self.door = door
        self.onDoor = onDoor
        self.onRules = onRules
        board = UtttBoardView(clock: model.clock)
        youMark = UtttInkView.mark(model.you, seed: model.seed &+ 4)
        rulebook = UtttRulebookButton(act: onRules, onHold: onDiagnostics)
        super.init(slide: slide)
        board.onTap = { [weak model] p in model?.tap(at: p) }
        content.addSubview(board)
        /* THE SIDE INDICATOR IS A DRAWN MARK, not a glyph, under two lines
         * of label set on a 9.5-point body - one element to VoiceOver. */
        youAre.addSubview(you1)
        youAre.addSubview(you2)
        you1.attributedText = UtttType.small.text(Uttt.say(.youAre1))
        you2.attributedText = UtttType.small.text(Uttt.say(.youAre2))
        indicator.addSubview(youAre)
        indicator.addSubview(youMark)
        indicator.isAccessibilityElement = true
        indicator.accessibilityLabel = Uttt.say(.youAreSpoken)
        content.addSubview(indicator)
        content.addSubview(column)
        content.addSubview(band)
        if let title = UtttDoorButton.title(door) {
            let a = UtttDoorButton(title: title, act: onDoor)
            content.addSubview(a)
            again = a
            /* READ NOW, while the resident game is this screen's: the
             * kernel's one slot can hold another game by the time of a tap. */
            let url = Uttt.replayURL
            var c: UtttDoorButton?
            c = url.map { url in
                UtttDoorButton(title: Uttt.say(.doorCopy)) {
                    UIPasteboard.general.string = url
                    c?.title = Uttt.say(.doorCopied)
                }
            }
            if let c { content.addSubview(c) }
            copy = c
        }
        content.addSubview(rulebook)
#if DEBUG
        if UtttRuler.on {
            band.ruler = true
            youMark.layer.addSublayer(orange)
            rulebook.layer.addSublayer(blue)
            again?.layer.addSublayer(violet)
        }
#endif
        model.onChange = { [weak self] in self?.changed() }
    }
    required init?(coder: NSCoder) { fatalError() }

#if DEBUG
    private let orange = MotionRuler.square(.orange)
    private let blue = MotionRuler.square(.blue)
    private let violet = MotionRuler.square(.violet)
#endif

    /// Something drawn changed: laid out again before this turn's commit.
    /// NOT the board at once - the model bumps `boardKey` before it starts
    /// the clock on the new plan, and a board bound then paints the new
    /// move's whole mark from the old plan's last frame (filmed: one frame
    /// of the finished mark before the ink began).
    private func changed() {
        setNeedsLayout()
    }

    // MARK: one layout, from compact to expanded

    override func lay(_ size: CGSize, from: CGFloat?) {
        /* AT THE END THE STRIP CARRIES THE VERDICT (UI.html 08: "the verdict
         * and the board"), in the right column beside the board, over the
         * rulebook. While the game runs the strip carries no words - the
         * wash says it - and the headline fades in with the band. */
        let end = Uttt.over != .none
        let hint = model.pending
        let hasCopy = copy != nil
        let L = Uttt.sheet(.play, size: size, words: end, hint: hint, copy: hasCopy)
        /* THROUGH AN AUTO-COLLAPSE (CollapseSlide) the sheet is laid out at
         * the compact height and pushed; each rider walks the path the layout
         * would have walked, as a function of the push `s` - the drawer is
         * `size.height + s` tall - from the kernel's own layout. */
        let at = { (s: CGFloat) -> UtiSheet in
            Uttt.sheet(.play, size: CGSize(width: size.width, height: size.height + s),
                       words: end, hint: hint, copy: hasCopy)
        }
        /* The band's words are hidden on the strip, so through a slide they
         * are set as the slide's first frame had them and fade on their layer. */
        let B = from.map { Uttt.sheet(.play, size: CGSize(width: size.width, height: $0),
                                      words: end, hint: hint, copy: hasCopy) } ?? L
#if DEBUG
        if UtttRuler.on { UtttLog.note("sheet-play", String(format: "h %.1f from %.1f board y %.1f side %.1f", size.height, from ?? -1, L.board.1, L.board.2)) }
#endif
        board.positionKey = model.boardKey
        placeBoard(board, L, at: at)
        layIndicator(L, at: at)

        /* THE WORDS TWICE, in the column beside the ink and in the band, each
         * shown only where it fits (uttt_sheet) - so a drag crossfades them
         * and never squeezes them to "Wai...". */
        /* the kernel's: the winner's own ink at the end, O red for O */
        let ink = UtttInk.rgba(Uttt.headlineInk)
        let (LC, LB) = wordsLayouts(L, B)
        column.frame = rect(LC.words)
        column.set(model.headline, ink: ink, seed: model.seed &* 31 &+ 7,
                   subline: model.subline, column: true, sub: CGFloat(LC.sub_alpha),
                   board: CGFloat(L.board.2))
        band.frame = rect(LB.band)
        band.set(model.headline, ink: ink, seed: model.seed &* 31 &+ 7,
                 subline: model.subline, column: false, board: CGFloat(L.board.2))
        placeWords(column: column, band: band, L, B, at: at)

        /* THE DOORS HOLD THE BOTTOM: the rulebook in the corner, Again
         * beside it at its height (owner), both inside the margins
         * (uttt_sheet). */
        rulebook.frame = rect(L.rulebook)
        if let again {
            placeDoor(again, L.again, L, at: at)
        }
        if let copy {
            placeDoor(copy, L.copy, L, at: at)
        }
        rideRulebook(rulebook, L, at: at)
#if DEBUG
        MotionRuler.place(blue, in: rulebook.bounds)
        if let again { MotionRuler.place(violet, in: again.bounds) }
#endif
    }

    /// THE HEADER HOLDS THE TOP: two lines of label centred over the drawn
    /// mark, at the kernel's anchor (`you`: centred in the strip's left
    /// column, at the left pad open). THROUGH AN AUTO-COLLAPSE THE LABEL AND THE MARK
    /// RIDE APART: the mark is 34 points on the strip and 46 open (`icon`),
    /// so riding the pair as one layer made the mark jump to its compact
    /// size in the slide's first frame (7pt, filmed). The mark scales about
    /// its top left and moves with the header; the label, centred over the
    /// mark, moves across by half its change of size.
    private func layIndicator(_ L: UtiSheet, at: @escaping (CGFloat) -> UtiSheet) {
        let icon = CGFloat(L.icon)
        /* two lines on a 9.5-point body, 2.8 points closer than their line
         * height: they read as one two-line label rather than two labels */
        let a = you1.sizeThatFits(.zero), b = you2.sizeThatFits(.zero)
        let ls = CGSize(width: max(a.width, b.width), height: a.height + b.height - 2.8)
        you1.frame = CGRect(x: (ls.width - a.width) / 2, y: 0, width: a.width, height: a.height)
        you2.frame = CGRect(x: (ls.width - b.width) / 2, y: a.height - 2.8, width: b.width, height: b.height)
        /* the indicator's left edge at a layout: its anchor less the share
         * of its width the kernel puts left of it */
        func left(_ S: UtiSheet) -> CGFloat {
            CGFloat(S.you.0) - CGFloat(S.you.1) * max(ls.width, CGFloat(S.icon))
        }
        func markLeft(_ S: UtiSheet) -> CGFloat {
            left(S) + (max(ls.width, CGFloat(S.icon)) - CGFloat(S.icon)) / 2
        }
        let stack = max(ls.width, icon)
        let x0 = left(L), y0 = CGFloat(L.vpad + L.icon_top)
        indicator.frame = CGRect(x: x0, y: y0, width: stack,
                                 height: ls.height + CGFloat(L.icon_lead) + icon)
        youAre.frame = CGRect(x: (stack - ls.width) / 2, y: 0, width: ls.width, height: ls.height)
        youMark.frame = CGRect(x: (stack - icon) / 2, y: ls.height + CGFloat(L.icon_lead),
                               width: icon, height: icon)
#if DEBUG
        MotionRuler.place(orange, in: youMark.bounds)
#endif
        ride(youAre) { s in
            let A = at(s)
            /* the label stays centred over the mark wherever it is */
            return CollapseRidePose(dy: CGFloat(A.icon_top - L.icon_top),
                                    dx: markLeft(A) + CGFloat(A.icon) / 2 - markLeft(L) - CGFloat(L.icon) / 2)
        }
        ride(youMark) { s in
            let A = at(s)
            return CollapseRidePose(
                dy: CGFloat(A.icon_top + A.icon_lead - L.icon_top - L.icon_lead),
                dx: markLeft(A) - markLeft(L),
                scale: L.icon > 0 ? CGFloat(A.icon / L.icon) : 1,
                pivot: .zero)
        }
    }
}

/// THE HEADLINE AND THE LINE UNDER IT, in one of the two places (the column
/// beside the ink, wrapped; the band across the top, one line), aligned to
/// one edge of the box it is framed to.
final class UtttWordsView: UIView {
    let align: NSTextAlignment
    private let headline = UtttHeadlineView()
    private let subline = UILabel()
    /// DEBUG: the ruler's yellow and lime squares on this copy.
    var ruler = false
#if DEBUG
    private let yellow = MotionRuler.square(.yellow)
    private let lime = MotionRuler.square(.lime)
#endif

    init(align: NSTextAlignment) {
        self.align = align
        super.init(frame: .zero)
        layer.actions = UtttLayers.still
        addSubview(headline)
        addSubview(subline)
        headline.isAccessibilityElement = true
        headline.accessibilityTraits = .header
    }
    required init?(coder: NSCoder) { fatalError() }

    /// The words, set to this view's width: the headline, then - 3 points
    /// under it - the line under it (where you sent them, or at the end the
    /// winning line spoken; docs/UI.html 04, 06, 07).
    /// `sub` is the second line's alpha (uttt_sheet's `sub_alpha`): a
    /// column too narrow for it carries the headline alone.
    func set(_ h: UtttModel.Headline, ink: UIColor, seed: Int32, subline text: String, column: Bool,
             sub: CGFloat = 1, board: CGFloat = 0) {
        let w = bounds.width
        let hs = headline.set(h, ink: ink, seed: seed, width: w, column: column, align: align,
                              board: board)
        headline.accessibilityLabel = Uttt.say(.headlineSpoken)
        headline.frame = CGRect(x: align == .right ? w - hs.width : 0, y: 0, width: hs.width, height: hs.height)
        subline.isHidden = text.isEmpty || sub <= 0
        subline.alpha = sub
        var ss = CGSize.zero
        if !text.isEmpty {
            ss = subline.set(text, .subline, width: w, column: column, align: align)
            subline.frame = CGRect(x: align == .right ? w - ss.width : 0, y: hs.height + 3,
                                   width: ss.width, height: ss.height)
        }
#if DEBUG
        if ruler {
            if yellow.superlayer == nil { layer.addSublayer(yellow); layer.addSublayer(lime) }
            MotionRuler.place(yellow, in: headline.frame)
            lime.isHidden = text.isEmpty
            MotionRuler.place(lime, in: subline.frame)
        }
#endif
    }
}

/// THE BAR'S LINE, with the other side drawn rather than spelled: words, a
/// drawn mark, words ("Waiting on <O>", "<X> wins").
///
/// The mark's middle sits on the middle of the lower-case words beside it -
/// half their x-height above the baseline (owner, 2026-09-23: the mark and
/// "wins" were not centred on each other). IN THE STRIP'S COLUMN words-only
/// lines WRAP ("You win" over two lines rather than a board a size
/// smaller), and a line with a drawn mark in it scales down to the column
/// instead, since a mark cannot break a line.
final class UtttHeadlineView: UIView {
    private let text = UILabel()
    private let before = UILabel()
    private let after = UILabel()
    private let mark = UtttInkView(key: "", square: true) { _ in [] }

    override init(frame: CGRect) {
        super.init(frame: frame)
        for v in [text, before, after] as [UIView] { addSubview(v) }
        addSubview(mark)
    }
    required init?(coder: NSCoder) { fatalError() }

    /// The headline at `width`; returns the size it takes.
    /// `type` is the headline's own unless a screen sets a smaller line
    /// the same way (the spectator's "<O> to play"); the mark is as tall as
    /// the type is big.
    func set(_ h: UtttModel.Headline, ink: UIColor, seed: Int32, width: CGFloat,
             column: Bool, align: NSTextAlignment, type: UtttType = .headline,
             board: CGFloat = 0) -> CGSize {
        switch h {
        case .text(let t):
            before.isHidden = true; after.isHidden = true; mark.isHidden = true
            text.isHidden = false
            var s = text.set(t, type, width: width, column: column, align: align, color: ink)
            /* AN EMPTY HEADLINE (their move drawing in, UTI_WORDS_HUSH) KEEPS
             * ITS LINE, so the line under it does not jump up and back. */
            if t.isEmpty { s = CGSize(width: 0, height: ceil(type.font(1).lineHeight)) }
            text.frame = CGRect(origin: .zero, size: s)
            return s
        case .mark(let b, let m, let a):
            text.isHidden = true
            before.isHidden = b.isEmpty; after.isHidden = a.isEmpty; mark.isHidden = false
            let side: CGFloat = type.size
            let natural = type.width(b) + side + type.width(a)
            let k = natural > width ? max(0.5, width / natural) : 1
            let f = type.font(k)
            before.attributedText = type.text(b, scale: k, color: ink)
            after.attributedText = type.text(a, scale: k, color: ink)
            let bs = b.isEmpty ? .zero : before.sizeThatFits(.zero)
            let as_ = a.isEmpty ? .zero : after.sizeThatFits(.zero)
            let ms = side * k
            let lineH = ceil(f.lineHeight)
            /* the mark centred on the words' x-height, on the first baseline */
            let base = f.ascender
            let my = base - f.xHeight / 2 - ms / 2
            let top = min(0, my)
            before.frame = CGRect(x: 0, y: -top, width: bs.width, height: lineH)
            /* `board`: the board's side, so the kernel draws this mark's
             * strokes as wide as the last move's (0: its own lighter pen) */
            let ratio = board > 0 && ms > 0 ? (board / ms * 100).rounded() / 100 : 0
            mark.key = "mark \(m.rawValue) \(seed) \(ratio)"
            mark.polys = { _ in Uttt.mark(m, seed: seed, board: ratio) }
            mark.frame = CGRect(x: bs.width, y: my - top, width: ms, height: ms)
            after.frame = CGRect(x: bs.width + ms, y: -top, width: as_.width, height: lineH)
            let w = min(width, bs.width + ms + as_.width)
            return CGSize(width: w, height: max(lineH - top, my - top + ms))
        }
    }
}
