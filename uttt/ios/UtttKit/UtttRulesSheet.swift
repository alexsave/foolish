import UIKit

/// What the door opens on.
///
/// THE TEXT IS THE KERNEL'S. Six lines and a title come out of `uttt_draw.c`
/// for the same reason the nine block names do - it is the one thing that
/// knows what the rules are, and a second copy of them in a renderer is a
/// second rulebook that drifts. This file lays them out and nothing else.
///
/// A SHEET OF ITS OWN, on the same paper, presented over the game. It used to
/// replace the board inside the drawer, so a swipe down on the rules was a
/// swipe on Messages' drawer and collapsed or closed the game (owner,
/// 2026-09-23). Presented as its own page sheet it carries its own drag to
/// dismiss, and the swipe closes the RULES - foolish presents its rulebook
/// the same way.
public final class UtttRulesSheet: UIViewController {
    /// The margin round the rules, the same on every edge.
    static let margin: CGFloat = 13

    private let paper = CALayer()
    private let titleLabel = UILabel()
    private let back = UIButton(type: .custom)
    private let scroll = UIScrollView()
    private var rows: [(UILabel, UILabel)] = []

    public init() {
        super.init(nibName: nil, bundle: nil)
        modalPresentationStyle = .pageSheet
        if let s = sheetPresentationController {
            s.detents = [.large()]
            s.prefersGrabberVisible = true
        }
    }
    required init?(coder: NSCoder) { fatalError() }

    public override func viewDidLoad() {
        super.viewDidLoad()
        view.backgroundColor = UtttPaper.flat
        paper.contentsGravity = .resize
        paper.show(UtttPaper.bitmap(side: 420))
        view.layer.addSublayer(paper)

        titleLabel.attributedText = UtttType.headline.text(Uttt.rulesTitle)
        titleLabel.accessibilityTraits = .header
        view.addSubview(titleLabel)

        var c = UIButton.Configuration.plain()
        c.attributedTitle = AttributedString(NSAttributedString(string: "Back", attributes: [
            .font: UIFont.systemFont(ofSize: 15, weight: .semibold), .foregroundColor: UtttInk.blue,
        ]))
        c.contentInsets = NSDirectionalEdgeInsets(top: 6, leading: 10, bottom: 6, trailing: 10)
        back.configuration = c
        back.addTarget(self, action: #selector(close), for: .touchUpInside)
        view.addSubview(back)

        scroll.showsVerticalScrollIndicator = false
        view.addSubview(scroll)
        let tally = UtttType(size: 11, weight: .bold, kern: 1.2, color: UtttInk.label)
        let body = UtttType(size: 15, color: UtttInk.ink)
        for (i, line) in Uttt.rules.enumerated() {
            /* The count is a drawn thing too - a tally, not a numeral,
             * because a digit set in a font would be the only printed
             * character on the sheet. */
            let t = UILabel(), l = UILabel()
            t.attributedText = tally.text(String(repeating: "|", count: i + 1))
            t.isAccessibilityElement = false
            l.attributedText = body.text(line)
            l.numberOfLines = 0
            scroll.addSubview(t)
            scroll.addSubview(l)
            rows.append((t, l))
        }
    }

    @objc private func close() { dismiss(animated: true) }

    public override func viewDidLayoutSubviews() {
        super.viewDidLayoutSubviews()
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        paper.frame = view.bounds
        CATransaction.commit()
        let m = Self.margin
        let inner = view.bounds.inset(by: view.safeAreaInsets).insetBy(dx: m, dy: m)
        let bs = back.sizeThatFits(.zero)
        let ts = titleLabel.sizeThatFits(.zero)
        /* the title and Back on one first baseline */
        let headH = max(ts.height, bs.height)
        titleLabel.frame = CGRect(x: inner.minX, y: inner.minY, width: min(ts.width, inner.width - bs.width - 12), height: ts.height)
        let titleBase = inner.minY + UtttType.headline.font().ascender
        let backFont = UIFont.systemFont(ofSize: 15, weight: .semibold)
        back.frame = CGRect(x: inner.maxX - bs.width, y: titleBase - 6 - backFont.ascender,
                            width: bs.width, height: bs.height)
        let top = inner.minY + headH + 14
        scroll.frame = CGRect(x: inner.minX, y: top, width: inner.width, height: inner.maxY - top)
        var y: CGFloat = 0
        let textX: CGFloat = 44 + 9
        for (t, l) in rows {
            let ls = l.sizeThatFits(CGSize(width: inner.width - textX, height: .greatestFiniteMagnitude))
            l.frame = CGRect(x: textX, y: y, width: inner.width - textX, height: ls.height)
            let ts = t.sizeThatFits(.zero)
            /* on the line's first baseline */
            let dy = UIFont.systemFont(ofSize: 15).ascender - UIFont.systemFont(ofSize: 11, weight: .bold).ascender
            t.frame = CGRect(x: 0, y: y + dy, width: 44, height: ts.height)
            y += ls.height + 13
        }
        scroll.contentSize = CGSize(width: inner.width, height: y - 13 + 8)
    }
}
