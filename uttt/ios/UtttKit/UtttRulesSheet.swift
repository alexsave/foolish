import CUttt
import UIKit

/// What the door opens on: docs/RULES.html, the illustrated rules sheet.
///
/// EVERYTHING ON IT IS THE KERNEL'S. The title and the eight lines come out
/// of `uttt_say.c`, each line's drawing out of `uttt_draw_rule` - the board's
/// own pen, hashes, marks, win line, wash and promise - and every size out of
/// `uttt_rules_look`. This file measures the text, which only the text engine
/// can, and puts things where those numbers say.
///
/// THE TWO YELLOWS IN THE TEXT are drawn as the board draws them: "yellow
/// outline" gets the promise's pen box round it, "yellow tinted area" the
/// wash behind it. The kernel names the phrases; the text engine says where
/// they landed.
///
/// A SHEET OF ITS OWN, on the same paper, presented over the game. It used to
/// replace the board inside the drawer, so a swipe down on the rules was a
/// swipe on Messages' drawer and collapsed or closed the game (owner,
/// 2026-09-23). Presented as its own page sheet it carries its own drag to
/// dismiss, and the swipe closes the RULES.
///
/// NO BACK BUTTON (owner, 2026-09-23): the sheet is dismissed by swiping it
/// down, and the platform's grabber says so.
public final class UtttRulesSheet: UIViewController {
    private let paper = CALayer()
    private let titleLabel = UILabel()
    private let scroll = UIScrollView()
    private var rows: [Row] = []

    /// One rule: its drawing, its text, and the yellow drawn with the text.
    private final class Row {
        let art: UtttInkView
        let text: UITextView
        let yellow: Uttt.RulesYellow
        /* the tint, one piece a line the phrase runs over, as CSS clones a
         * background across a break */
        var tints: [UIView] = []
        let box = UtttInkView(key: "", square: false) { _ in [] }

        init(_ i: Int, line: String, look L: UtiRulesLook) {
            art = UtttInkView(key: "rule \(i)", square: true) { _ in Uttt.rule(i) }
            art.isAccessibilityElement = false
            yellow = Uttt.rulesYellow(i)
            text = UITextView(usingTextLayoutManager: false)
            text.isEditable = false
            text.isScrollEnabled = false
            text.isSelectable = false
            text.backgroundColor = .clear
            text.textContainerInset = .zero
            text.textContainer.lineFragmentPadding = 0
            text.attributedText = Row.words(line, yellow, L)
            box.isAccessibilityElement = false
        }

        /// The line in the body type, with room opened either side of a
        /// marked phrase (RULES.html's padding round the two yellows), and
        /// the outlined phrase kept on one line as the mockup keeps it.
        static func words(_ line: String, _ y: Uttt.RulesYellow, _ L: UtiRulesLook) -> NSAttributedString {
            let font = UIFont.systemFont(ofSize: CGFloat(L.body_pt))
            let p = NSMutableParagraphStyle()
            let lead = (CGFloat(L.body_pt) * CGFloat(L.body_lead)).rounded()
            p.minimumLineHeight = lead
            p.maximumLineHeight = lead
            p.lineBreakMode = .byWordWrapping
            let s = NSMutableAttributedString(string: line, attributes: [
                .font: font, .foregroundColor: UtttInk.rgba(L.ink), .paragraphStyle: p,
                /* the line's glyphs centred in its height, as CSS sets them */
                .baselineOffset: (lead - font.lineHeight) / 2,
            ])
            let (r, room): (NSRange?, CGFloat) = {
                switch y {
                case .none: return (nil, 0)
                case .outline(let r): return (r, CGFloat(L.word_room))
                case .tint(let r): return (r, CGFloat(L.tint_pad_x))
                }
            }()
            guard let r, r.length > 0, r.location + r.length <= s.length else { return s }
            if case .outline = y {
                let ns = s.string as NSString
                ns.substring(with: r).enumerated().forEach { k, c in
                    if c == " " { s.replaceCharacters(in: NSRange(location: r.location + k, length: 1),
                                                     with: "\u{00A0}") }
                }
            }
            /* the room: after the character before the phrase and after its
             * last character, which is where kerning opens space */
            if r.location > 0 { s.addAttribute(.kern, value: room, range: NSRange(location: r.location - 1, length: 1)) }
            s.addAttribute(.kern, value: room, range: NSRange(location: r.location + r.length - 1, length: 1))
            return s
        }

        /// The marked phrase's rects in the text view, one a line it runs
        /// over, around its glyphs' type height and without the room.
        func phraseRects(_ L: UtiRulesLook) -> [CGRect] {
            let r: NSRange, room: CGFloat
            switch yellow {
            case .none: return []
            case .outline(let x): r = x; room = CGFloat(L.word_room)
            case .tint(let x): r = x; room = CGFloat(L.tint_pad_x)
            }
            let lm = text.layoutManager
            let glyphs = lm.glyphRange(forCharacterRange: r, actualCharacterRange: nil)
            let font = UIFont.systemFont(ofSize: CGFloat(L.body_pt))
            var out: [CGRect] = []
            lm.enumerateLineFragments(forGlyphRange: glyphs) { frag, _, _, lineGlyphs, _ in
                let g = NSIntersectionRange(lineGlyphs, glyphs)
                guard g.length > 0 else { return }
                var b = lm.boundingRect(forGlyphRange: g, in: self.text.textContainer)
                /* the room after the last character is not the phrase's */
                if NSMaxRange(g) == NSMaxRange(glyphs) { b.size.width -= room }
                let base = frag.minY + lm.location(forGlyphAt: g.location).y
                out.append(CGRect(x: b.minX, y: base - font.ascender, width: b.width,
                                  height: font.ascender - font.descender))
            }
            return out
        }
    }

    public init() {
        super.init(nibName: nil, bundle: nil)
        modalPresentationStyle = .pageSheet
        if let s = sheetPresentationController {
            s.detents = [.large()]
            s.prefersGrabberVisible = true
            /* THE PAPER IS ONE COLOUR IN BOTH APPEARANCES, so the grabber
             * is the light appearance's: the dark one's pale pill vanished
             * into the napkin. The sheet's container draws it, so the
             * override goes on the presentation, not on this controller. */
            s.traitOverrides.userInterfaceStyle = .light
        }
    }
    required init?(coder: NSCoder) { fatalError() }

    public override func viewDidLoad() {
        super.viewDidLoad()
        view.backgroundColor = UtttPaper.flat
        paper.contentsGravity = .resize
        paper.show(UtttPaper.bitmap(side: 420))
        view.layer.addSublayer(paper)

        let L = Uttt.rulesLook
        titleLabel.attributedText = UtttType(size: CGFloat(L.title_pt), weight: .bold, kern: -0.015 * CGFloat(L.title_pt),
                                             color: UtttInk.rgba(L.ink)).text(Uttt.rulesTitle)
        titleLabel.accessibilityTraits = .header
        view.addSubview(titleLabel)

        scroll.showsVerticalScrollIndicator = false
        view.addSubview(scroll)
        for (i, line) in Uttt.rules.enumerated() {
            let row = Row(i, line: line, look: L)
            scroll.addSubview(row.art)
            scroll.addSubview(row.text)
            scroll.addSubview(row.box)
            rows.append(row)
        }
    }

    /// VoiceOver's escape (the two-finger scrub) closes the rules, which is
    /// the swipe down's job for a reader who cannot see the grabber.
    public override func accessibilityPerformEscape() -> Bool {
        dismiss(animated: true)
        return true
    }

    public override func viewDidLayoutSubviews() {
        super.viewDidLayoutSubviews()
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        paper.frame = view.bounds
        CATransaction.commit()
        let L = Uttt.rulesLook
        let safe = view.safeAreaInsets
        let x0 = safe.left + CGFloat(L.margin_x)
        let width = view.bounds.width - x0 - safe.right - CGFloat(L.margin_x)
        let ts = titleLabel.sizeThatFits(CGSize(width: width, height: .greatestFiniteMagnitude))
        titleLabel.frame = CGRect(x: x0, y: safe.top + CGFloat(L.top), width: min(ts.width, width), height: ts.height)
        let top = titleLabel.frame.maxY + CGFloat(L.title_gap)
        scroll.frame = CGRect(x: x0, y: top, width: width, height: view.bounds.height - top)

        /* EVERY ROW AS TALL AS THE TALLEST, so the eight drawings sit on an
         * even beat (RULES.html), each drawing and its text centred in it */
        let art = CGFloat(L.art), textX = art + CGFloat(L.art_gap), textW = width - textX
        let sizes = rows.map { $0.text.sizeThatFits(CGSize(width: textW, height: .greatestFiniteMagnitude)) }
        let rowH = max(art, sizes.map { ceil($0.height) }.max() ?? 0)
        var y: CGFloat = 0
        for (row, s) in zip(rows, sizes) {
            row.art.frame = CGRect(x: 0, y: y + (rowH - art) / 2, width: art, height: art)
            let h = ceil(s.height)
            row.text.frame = CGRect(x: textX, y: y + (rowH - h) / 2, width: textW, height: h)
            row.text.layoutIfNeeded()
            let rects = row.phraseRects(L).map { $0.offsetBy(dx: row.text.frame.minX, dy: row.text.frame.minY) }
            row.tints.forEach { $0.removeFromSuperview() }
            row.tints = []
            row.box.isHidden = true
            switch row.yellow {
            case .tint:
                for r in rects {
                    let t = UIView(frame: r.insetBy(dx: -CGFloat(L.tint_pad_x), dy: -CGFloat(L.tint_pad_y)))
                    t.backgroundColor = UtttInk.rgba(L.tint)
                    t.isUserInteractionEnabled = false
                    scroll.insertSubview(t, belowSubview: row.text)
                    row.tints.append(t)
                }
            case .outline:
                if let r = rects.first {
                    /* the phrase plus the room, and the pen's pad round that */
                    let f = r.insetBy(dx: -(CGFloat(L.word_room) - 1 + CGFloat(L.box_pad)),
                                      dy: -CGFloat(L.box_pad))
                    row.box.isHidden = false
                    row.box.frame = f
                    row.box.key = "rule box \(Int(f.width * 4))x\(Int(f.height * 4))"
                    row.box.polys = { size in Uttt.ruleBox(w: size.width, h: size.height) }
                }
            case .none: break
            }
            y += rowH + CGFloat(L.row_gap)
        }
        scroll.contentSize = CGSize(width: width, height: y - CGFloat(L.row_gap) + CGFloat(L.bottom) + safe.bottom)
    }
}
