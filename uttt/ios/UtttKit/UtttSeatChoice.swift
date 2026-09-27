#if DEBUG
import UIKit

/// WHICH OF THE TWO PEOPLE YOU ARE, asked once when a bubble is opened.
///
/// A two-handed game in a transcript cannot be played on one phone: Messages
/// gives a conversation exactly one local participant, so the invitation goes
/// out and nothing ever happens. With `dev.picker` set, opening a bubble asks
/// this first and then gets out of the way - the game plays normally
/// afterwards, with no debug anything on any screen.
///
/// IT DOES NOT NAME THE MARKS. Which seat is X is not decided until both are
/// taken, and a chooser that leaked it would be a nicer version of the hole
/// this app was just fixed to close.
///
/// ON THE NAPKIN like every other screen: without the sheet this came up as
/// dark ink on the drawer's own dark background and read as an empty box.
public final class UtttSeatChoice: UtttSheetView {
    private let pick: (String) -> Void
    private let title = UILabel()
    private let line = UILabel()
    private var seats: [UIButton] = []

    public init(pick: @escaping (String) -> Void) {
        self.pick = pick
        super.init(slide: nil)
        title.attributedText = UtttType.headline.text("Who are you?", align: .center)
        line.attributedText = UtttType(size: 13, color: UtttInk.label)
            .text("One phone, two people. Pick a side and play.", align: .center)
        line.adjustsFontSizeToFitWidth = true
        content.addSubview(title)
        content.addSubview(line)
        for (word, t) in [("a", "The one who\nput it down"), ("b", "The one who\ntook it up")] {
            let b = UIButton(type: .custom)
            let p = NSMutableParagraphStyle()
            p.alignment = .center
            b.setAttributedTitle(NSAttributedString(string: t, attributes: [
                .font: UIFont.systemFont(ofSize: 14, weight: .semibold),
                .foregroundColor: UtttInk.ink, .paragraphStyle: p,
            ]), for: .normal)
            b.titleLabel?.numberOfLines = 2
            b.layer.cornerRadius = 33
            b.layer.borderWidth = 1.2
            b.layer.borderColor = UtttInk.label.withAlphaComponent(0.7).cgColor
            b.addAction(UIAction { [weak self] _ in self?.pick(word) }, for: .touchUpInside)
            content.addSubview(b)
            seats.append(b)
        }
    }
    required init?(coder: NSCoder) { fatalError() }

    override func lay(_ size: CGSize, from: CGFloat?) {
        let inner = CGRect(origin: .zero, size: size).insetBy(dx: 13, dy: 13)
        let ts = title.sizeThatFits(.zero)
        let ls = line.sizeThatFits(.zero)
        let block = ts.height + 5 + ls.height + 20 + 66
        var y = inner.midY - block / 2
        title.frame = CGRect(x: inner.midX - ts.width / 2, y: y, width: ts.width, height: ts.height)
        y += ts.height + 5
        let lw = min(ls.width, inner.width)
        line.frame = CGRect(x: inner.midX - lw / 2, y: y, width: lw, height: ls.height)
        y += ls.height + 20
        let x0 = inner.midX - (132 * 2 + 12) / 2
        for (i, b) in seats.enumerated() {
            b.frame = CGRect(x: x0 + CGFloat(i) * (132 + 12), y: y, width: 132, height: 66)
        }
    }
}
#endif
