import CUttt
import UIKit

/// The inks the screens set type in, and the drawn things that do not move.
///
/// UIKIT AND CORE ANIMATION, NO SWIFTUI (TESTFLIGHT_PLAN 14): the extension's
/// process no longer loads SwiftUI at all. Every word is a UILabel, every pen
/// drawing a layer whose contents is a bitmap of the kernel's polygons.
enum UtttInk {
    static let ink   = UIColor(red: 0.114, green: 0.106, blue: 0.087, alpha: 1)   // #1d1b16
    static let muted = UIColor(red: 0.42, green: 0.40, blue: 0.35, alpha: 1)
    static let label = UIColor(red: 0.541, green: 0.522, blue: 0.467, alpha: 1)   // #8a8577
    static let blue  = UIColor(red: 0.145, green: 0.216, blue: 0.420, alpha: 1)   // #25376b
    /// The doors' outline ink (uttt_rule.c EDGE, #1b2a52), which the label
    /// is set in so the word and the bar are one pen.
    static let doorInk = UIColor(red: 0.106, green: 0.165, blue: 0.322, alpha: 1)
}

/// One face of type: size, weight, tracking, colour, and whether it is set
/// in capitals.
struct UtttType {
    var size: CGFloat
    var weight: UIFont.Weight = .regular
    var kern: CGFloat = 0
    var color: UIColor
    var upper = false

    static let headline = UtttType(size: 21, weight: .bold, kern: -0.315, color: UtttInk.ink)
    static let subline  = UtttType(size: 14, color: UtttInk.muted)
    /// The 9.5-point label ("YOU ARE", "WATCHING"): .2em, capitals.
    static let small    = UtttType(size: 9.5, weight: .semibold, kern: 1.9, color: UtttInk.label, upper: true)

    func font(_ scale: CGFloat = 1) -> UIFont { .systemFont(ofSize: size * scale, weight: weight) }

    func text(_ s: String, scale: CGFloat = 1, align: NSTextAlignment = .natural,
              color: UIColor? = nil) -> NSAttributedString {
        let p = NSMutableParagraphStyle()
        p.alignment = align
        p.lineBreakMode = .byWordWrapping
        return NSAttributedString(string: upper ? s.uppercased() : s, attributes: [
            .font: font(scale), .kern: kern * scale, .foregroundColor: color ?? self.color,
            .paragraphStyle: p,
        ])
    }

    func width(_ s: String, scale: CGFloat = 1) -> CGFloat {
        ceil(text(s, scale: scale).size().width)
    }
}

extension UILabel {
    /// SET IN A BOX OF `width`: in a COLUMN wrapped at spaces only, as many
    /// lines as the words, a word wider than the column scaling the type down
    /// to it (never "Diagona / l"); on one line otherwise, scaled to fit.
    /// Down to half size, as the sheet always allowed. Returns its size.
    @discardableResult
    func set(_ s: String, _ type: UtttType, width: CGFloat, column: Bool,
             align: NSTextAlignment, color: UIColor? = nil, maxHeight: CGFloat = .infinity) -> CGSize {
        let words = s.split(separator: " ").map(String.init)
        let natural = column ? (words.map { type.width($0) }.max() ?? 0) : type.width(s)
        var scale = natural > width && natural > 0 ? max(0.5, width / natural) : 1
        numberOfLines = column ? max(1, words.count) : 1
        lineBreakMode = column ? .byWordWrapping : .byTruncatingTail
        var size = CGSize.zero
        for _ in 0..<6 {
            attributedText = type.text(s, scale: scale, align: align, color: color)
            size = sizeThatFits(CGSize(width: width, height: .greatestFiniteMagnitude))
            if size.height <= maxHeight || scale <= 0.5 { break }
            scale = max(0.5, scale * 0.9)
        }
        return CGSize(width: min(width, ceil(size.width)), height: ceil(size.height))
    }
}

/// A DRAWN THING THAT DOES NOT MOVE - the "you are" mark, the rulebook door,
/// the Again door - painted ONCE per size by Core Graphics into a bitmap and
/// shown as a layer's contents, a texture the compositor keeps.
enum UtttInkImage {
    private static var cache: [String: CGImage] = [:]

    /// `polys` for `size` into a bitmap at the screen's scale: the unit
    /// square scaled to the smaller side (`square`) or stretched to the whole
    /// size (the Again door's bar).
    static func image(_ key: String, _ size: CGSize, _ square: Bool,
                      _ polys: (CGSize) -> [Uttt.Poly]) -> CGImage? {
        guard size.width >= 1, size.height >= 1 else { return nil }
        let k = "\(key)|\(Int(size.width * 4))x\(Int(size.height * 4))"
        if let img = cache[k] { return img }
        let scale = UIScreen.main.scale
        let pw = Int((size.width * scale).rounded(.up)), ph = Int((size.height * scale).rounded(.up))
        guard let cg = CGContext(data: nil, width: pw, height: ph, bitsPerComponent: 8, bytesPerRow: 0,
                                 space: CGColorSpaceCreateDeviceRGB(),
                                 bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue
                                     | CGBitmapInfo.byteOrder32Little.rawValue)
        else { return nil }
        cg.translateBy(x: 0, y: CGFloat(ph))
        cg.scaleBy(x: scale, y: -scale)
        let sx = square ? min(size.width, size.height) : size.width
        let sy = square ? min(size.width, size.height) : size.height
        for p in polys(size) {
            guard let head = p.points.first else { continue }
            cg.setFillColor(p.color)
            cg.beginPath()
            cg.move(to: CGPoint(x: head.x * sx, y: head.y * sy))
            for q in p.points.dropFirst() { cg.addLine(to: CGPoint(x: q.x * sx, y: q.y * sy)) }
            cg.closePath()
            cg.fillPath()
        }
        guard let img = cg.makeImage() else { return nil }
        /* a drawer drag hands a new size every frame: keep the last few */
        if cache.count > 24 { cache.removeAll() }
        cache[k] = img
        return img
    }
}

/// A view whose picture is a pen drawing at its own size (UtttInkImage).
final class UtttInkView: UIView {
    var key: String { didSet { if key != oldValue { setNeedsLayout() } } }
    let square: Bool
    var polys: (CGSize) -> [Uttt.Poly]
    private var drawn = ""

    init(key: String, square: Bool, polys: @escaping (CGSize) -> [Uttt.Poly]) {
        self.key = key
        self.square = square
        self.polys = polys
        super.init(frame: .zero)
        isUserInteractionEnabled = false
        layer.contentsGravity = .resize
        layer.actions = UtttLayers.still
    }
    required init?(coder: NSCoder) { fatalError() }

    override func layoutSubviews() {
        super.layoutSubviews()
        let k = "\(key)|\(bounds.size)"
        guard k != drawn else { return }
        drawn = k
        layer.contents = UtttInkImage.image(key, bounds.size, square, polys)
    }

    /// The side indicator: the same X that is about to land on the board,
    /// out of the same pen, because a glyph from a font would be the only
    /// thing in the frame that did not come off the nib.
    static func mark(_ m: Uttt.Mark, seed: Int32) -> UtttInkView {
        UtttInkView(key: "mark \(m.rawValue) \(seed)", square: true) { _ in Uttt.mark(m, seed: seed) }
    }
}

enum UtttLayers {
    /// No implicit animation: every motion on these screens is the clock's,
    /// the slide's or the host's.
    static let still: [String: CAAction] = [
        "position": NSNull(), "bounds": NSNull(), "frame": NSNull(), "contents": NSNull(),
        "backgroundColor": NSNull(), "opacity": NSNull(), "hidden": NSNull(), "transform": NSNull(),
    ]
}

/// THE ONE DOOR: docs/UI.html's `.udoor go` (full width, bottom of the sheet,
/// 14-point bold type at .04em), drawn by the pen rather than printed. The bar
/// - a rough outline over a two-fifths hachure, the rulebook square's pen - is
/// `uttt_draw_door`; this fills its polygons at the size it has and sets the
/// label on top in the outline's dark ink (feedback: every button is the pen's).
public final class UtttDoorButton: UIControl {
    private let bar = UtttInkView(key: "door", square: false) { s in Uttt.door(w: s.width, h: s.height) }
    private let label = UILabel()
    private let act: () -> Void

    public init(title: String, act: @escaping () -> Void) {
        self.act = act
        super.init(frame: .zero)
        addSubview(bar)
        label.attributedText = NSAttributedString(string: title, attributes: [
            .font: UIFont.systemFont(ofSize: 14, weight: .bold), .kern: 0.56,
            .foregroundColor: UtttInk.doorInk,
        ])
        label.textAlignment = .center
        addSubview(label)
        isAccessibilityElement = true
        accessibilityLabel = title
        accessibilityTraits = .button
        addTarget(self, action: #selector(fire), for: .touchUpInside)
    }
    required init?(coder: NSCoder) { fatalError() }

    @objc private func fire() { act() }

    public override func layoutSubviews() {
        super.layoutSubviews()
        bar.frame = bounds
        label.frame = bounds
    }

    /// The words on a door, or nil for no door. The kernel's.
    public static func title(_ door: Uttt.Door) -> String? {
        switch door {
        case .none:  return nil
        case .again: return Uttt.say(.doorAgain)
        }
    }
}

/// The rulebook door: a square whose edge and fill are both rough, the
/// book's glyph in its own hachure (`uttt_rule.c`). The kernel is handed the
/// size the button has - a hachure gap is a whole number of points, so the
/// shape is not scale-free - and this fills what comes back.
public final class UtttRulebookButton: UIControl {
    private let ink = UtttInkView(key: "rulebook", square: true) { s in Uttt.rulebook(w: s.width, h: s.height) }
    private let act: () -> Void

    /// The door's size - one size at every drawer height, the kernel's
    /// (`uttt_sheet`'s door) - and the Again bar's height, which stands
    /// beside it and must match it (owner).
    public static let expandedSide = CGFloat(Uttt.sheet(.play, size: CGSize(width: 440, height: 800)).door)

    public init(act: @escaping () -> Void) {
        self.act = act
        super.init(frame: .zero)
        addSubview(ink)
        isAccessibilityElement = true
        accessibilityLabel = Uttt.say(.doorRules)
        accessibilityTraits = .button
        addTarget(self, action: #selector(fire), for: .touchUpInside)
    }
    required init?(coder: NSCoder) { fatalError() }

    @objc private func fire() { act() }

    public override func layoutSubviews() {
        super.layoutSubviews()
        ink.frame = bounds
    }
}
