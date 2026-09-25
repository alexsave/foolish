// SendHintView - SendHint (SendHint.swift) on UIKit and Core Animation, for a
// product with no SwiftUI in its process (uttt, TESTFLIGHT_PLAN 14).
//
// THE SAME HINT: the arrow in the glyph Messages' Send button carries (SF
// Symbols `arrow.up`, stretched to `SendHintMetrics.arrowSize`), its caption in
// heavy type under it, both ringed by eight stamped copies at `ringRadius`,
// the pair riding one wave up toward the Send button (`bobTravel`, `bobPeriod`,
// `crestRoom`), centred on the send axis and clamped to the screen's edge.
// Every number is SendHintMetrics'; only the machinery differs.
//
// WHAT MOVES IS THE RENDER SERVER'S: the ink is one bitmap, painted once per
// caption and appearance, on a layer whose bob is a repeating keyframe
// animation - nothing is drawn per frame and the main thread is not asked.
// The fuse is a timer; the fade is a layer opacity animation.

#if canImport(UIKit)
import UIKit

@MainActor
public final class SendHintView: UIView {
    /// The words under the arrow, in the product's own language.
    public var caption: String = "" { didSet { if caption != oldValue { repaint() } } }
    /// White glyph and words ringed in the send blue (true, uttt's light
    /// paper) or send-blue ringed in `outline` (the sister product's felt).
    public var whiteInk = true { didSet { if whiteInk != oldValue { repaint() } } }
    public var outline: UIColor = .white { didSet { repaint() } }
    /// Where the send axis sits from the SCREEN's trailing edge and from
    /// this view's: SendHint's `screenAxis` and `centerFromTrailing`.
    public var screenAxis: CGFloat = SendHintMetrics.axisFromScreenTrailing { didSet { setNeedsLayout() } }
    public var centerFromTrailing: CGFloat = SendHintMetrics.axisFromScreenTrailing { didSet { setNeedsLayout() } }
    /// How long a staged bubble waits before the hint appears.
    public var fuse: Double = SendHintMetrics.defaultFuse
    /// Hide with no fade (uttt); the sister product fades.
    public var hidesAtOnce = true

    /// SendHint's caption face: the system's, made heavy.
    public static let captionFont = UIFont.systemFont(ofSize: SendHintMetrics.captionSize, weight: .heavy)

    private let bob = CALayer()
    private var shown = false
    private var staged = false
    private var visible = true
    private var fuseTimer: Timer?
    private var inkSize: CGSize = .zero
    private var arrowMidX: CGFloat = 0

    public override init(frame: CGRect) {
        super.init(frame: frame)
        isUserInteractionEnabled = false
        isAccessibilityElement = false
        accessibilityElementsHidden = true      // decorative; the staged state reads elsewhere
        bob.actions = ["position": NSNull(), "bounds": NSNull(), "contents": NSNull()]
        bob.contentsGravity = .resize
        layer.addSublayer(bob)
        layer.opacity = 0
        layer.actions = ["opacity": NSNull()]
        /* the send blue is the appearance's: repainted when it changes */
        if #available(iOS 17.0, *) {
            registerForTraitChanges([UITraitUserInterfaceStyle.self]) { (v: SendHintView, _) in
                v.repaint()
            }
        }
    }
    required init?(coder: NSCoder) { fatalError() }

    /// The lifecycle, SendHint's: a new `restart` or `staged` going true
    /// restarts the fuse; `staged` false hides it (at once, `hidesAtOnce`);
    /// `visible` gates showing without disturbing the fuse.
    public func update(staged: Bool, visible: Bool, restart: Bool) {
        let fuseKey = staged != self.staged || restart
        self.staged = staged
        self.visible = visible
        if fuseKey {
            fuseTimer?.invalidate()
            fuseTimer = nil
            shown = false
            if staged {
                fuseTimer = Timer.scheduledTimer(withTimeInterval: fuse, repeats: false) { [weak self] _ in
                    MainActor.assumeIsolated {
                        guard let self, self.staged else { return }
                        self.shown = true
                        self.apply()
                    }
                }
            }
        }
        apply()
    }

    private var on: Bool { shown && visible && (staged || !hidesAtOnce) }

    private func apply() {
        let want: Float = on ? 1 : 0
        guard layer.opacity != want else { return }
        let from = layer.presentation()?.opacity ?? layer.opacity
        layer.opacity = want
        if on || !hidesAtOnce {
            let a = CABasicAnimation(keyPath: "opacity")
            a.fromValue = from
            a.toValue = want
            a.duration = 0.35
            a.timingFunction = CAMediaTimingFunction(name: .easeInEaseOut)
            layer.add(a, forKey: "fade")
        } else {
            layer.removeAnimation(forKey: "fade")
        }
        if on { startBob() } else { bob.removeAnimation(forKey: "bob") }
    }

    /// Rest at 0, crest at -bobTravel: (1 - cos) starts the wave at rest and
    /// only ever lifts away from it - SendHintArrow's sine, sampled.
    private func startBob() {
        guard bob.animation(forKey: "bob") == nil else { return }
        let n = 48
        let a = CAKeyframeAnimation(keyPath: "transform.translation.y")
        a.values = (0...n).map { i -> CGFloat in
            let t = Double(i) / Double(n)
            return -SendHintMetrics.bobTravel / 2 * (1 - cos(t * 2 * .pi))
        }
        a.duration = SendHintMetrics.bobPeriod
        a.repeatCount = .infinity
        a.calculationMode = .linear
        bob.add(a, forKey: "bob")
    }

    public override func didMoveToWindow() {
        super.didMoveToWindow()
        if bob.contents == nil { repaint() }
        if window != nil, on { bob.removeAnimation(forKey: "bob"); startBob() }
    }

    /// Messages fills its Send circle with the system blue: #007AFF light,
    /// #0A84FF dark, as SendHint.swift writes them.
    private var sendBlue: UIColor {
        traitCollection.userInterfaceStyle == .dark
            ? UIColor(red: 0x0A / 255, green: 0x84 / 255, blue: 1, alpha: 1)
            : UIColor(red: 0, green: 0x7A / 255, blue: 1, alpha: 1)
    }

    /// The arrow over the caption, both ringed, into one bitmap.
    private func repaint() {
        let fill = whiteInk ? UIColor.white : sendBlue
        let ring = whiteInk ? sendBlue : outline
        let arrow = SendHintMetrics.arrowSize
        let r = SendHintMetrics.ringRadius
        let attrs: [NSAttributedString.Key: Any] = [.font: Self.captionFont]
        let text = (caption as NSString).size(withAttributes: attrs)
        let gap: CGFloat = 3
        let w = ceil(max(arrow.width, text.width)) + 2 * r + 2
        let h = ceil(arrow.height + gap + text.height) + 2 * r + 2
        /* the caption's centre on the axis unless that runs it past the
         * screen's edge (SendHintArrow's clamp): its right edge at most
         * `screenAxis - 4` right of the axis */
        let capHalf = text.width / 2
        let shift = max(0, capHalf - (screenAxis - 4))
        let size = CGSize(width: w + shift, height: h)
        let ax = r + 1 + max(arrow.width, text.width) / 2 + shift
        let arrowRect = CGRect(x: ax - arrow.width / 2, y: r + 1, width: arrow.width, height: arrow.height)
        let textOrigin = CGPoint(x: ax - capHalf - shift, y: arrowRect.maxY + gap)
        let glyph = UIImage(systemName: "arrow.up", withConfiguration: UIImage.SymbolConfiguration(pointSize: arrow.height))?.withRenderingMode(.alwaysTemplate)
        let fmt = UIGraphicsImageRendererFormat.preferred()
        fmt.preferredRange = .standard
        let caption = self.caption
        let img = UIGraphicsImageRenderer(size: size, format: fmt).image { _ in
            func stamp(_ color: UIColor, _ dx: CGFloat, _ dy: CGFloat) {
                glyph?.withTintColor(color).draw(in: arrowRect.offsetBy(dx: dx, dy: dy))
                var a = attrs
                a[.foregroundColor] = color
                (caption as NSString).draw(at: CGPoint(x: textOrigin.x + dx, y: textOrigin.y + dy),
                                           withAttributes: a)
            }
            for i in 0..<8 {
                let t = CGFloat(i) * .pi / 4
                stamp(ring, r * cos(t), r * sin(t))
            }
            stamp(fill, 0, 0)
        }
        inkSize = size
        arrowMidX = ax
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        bob.contents = img.cgImage
        bob.contentsScale = img.scale
        CATransaction.commit()
        setNeedsLayout()
    }

    /// The arrow's centre on the axis `centerFromTrailing` in from this
    /// view's trailing edge, its rest `crestRoom` from the top.
    public override func layoutSubviews() {
        super.layoutSubviews()
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        let axis = bounds.width - centerFromTrailing
        bob.frame = CGRect(x: axis - arrowMidX, y: SendHintMetrics.crestRoom - SendHintMetrics.ringRadius - 1,
                           width: inkSize.width, height: inkSize.height)
        CATransaction.commit()
    }
}
#endif
