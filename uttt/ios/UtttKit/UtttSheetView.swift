import CUttt
import UIKit

/// THE SHEET AT THE DRAWER'S HEIGHT, for every screen: the paper, the content
/// laid out by the kernel (`uttt_sheet`) at the height Messages hands, the
/// auto-collapse's riders, and the ruler's edge bars.
///
/// A NEW HEIGHT IS LAID OUT AT ONCE, never tweened: Messages resizes the
/// drawer inside a UIKit animation block, and a frame set inside it would
/// creep on the host's curve. Every number on the sheet is a function of the
/// height, so the height is the only animation (src/uttt_anim.h: a finger's
/// heights are where the drawer is, and a release or a tap to expand is the
/// host animating our view to the height it handed).
///
/// AN AUTO-COLLAPSE IS LAID OUT AT THE COMPACT HEIGHT FROM ITS FIRST FRAME
/// and pushed by the slide (CollapseSlide): the frame that first sees the
/// drop is the flip, and every rider walks, on the render server, the path
/// the layout would have walked (`ride`).
///
/// A screen subclasses this and fills `lay(_:from:)`: one kernel layout, the
/// frames, and a ride per rider. It never branches on the raw height.
public class UtttSheetView: UIView {
    let slide: CollapseSlide?
    /// Everything on the sheet but the paper, in the safe area, and clipped
    /// to the sheet's edge - which through a slide is the drawer's, `travel`
    /// above the compact sheet's top.
    let content = UIView()
    private let paper = CALayer()
    private let clip = CALayer()
    private var handed: CGFloat = 0
    private var from: CGFloat?
    private var travel: CGFloat = 0
    private var token: Int?
#if DEBUG
    private var rulerTop: MotionRulerEdges?
    private var rulerBottom: MotionRulerEdges?
#endif

    init(slide: CollapseSlide?) {
        self.slide = slide
        super.init(frame: .zero)
        backgroundColor = UtttPaper.flat
        layer.actions = UtttLayers.still
        paper.actions = UtttLayers.still
        paper.contentsGravity = .resize
        paper.show(UtttPaper.bitmap(side: 420))
        layer.addSublayer(paper)
        clip.backgroundColor = UIColor.black.cgColor
        clip.actions = UtttLayers.still
        content.layer.mask = clip
        content.layer.actions = UtttLayers.still
        addSubview(content)
#if DEBUG
        if UtttRuler.on {
            /* THE RULER'S TOP HALF RIDES THE DRAWER'S TOP, the bottom half
             * the sheet's bottom: through a slide they are two layers. */
            let b = MotionRulerEdges(top: false, bottom: true)
            let t = MotionRulerEdges(top: true, bottom: false)
            content.addSubview(b)
            content.addSubview(t)
            rulerBottom = b
            rulerTop = t
        }
#endif
        token = slide?.observe { [weak self] run in
            guard let self else { return }
            self.from = run?.from
            self.travel = run?.travel ?? 0
            self.setNeedsLayout()
        }
    }
    required init?(coder: NSCoder) { fatalError() }

    deinit {
        if let token, let slide { MainActor.assumeIsolated { slide.unobserve(token) } }
    }

    /// THE PAPER IS ONE SIZE, bottom-anchored: this tall at every drawer
    /// height, so its grain never stretches as the drawer moves, and taller
    /// than any drawer plus the longest auto-collapse push (the paper above
    /// the pushed sheet is this same sheet's top). Nothing clips it: a
    /// background that ends exactly at its own bounds shows black the moment
    /// the bounds are stale by a frame.
    private static let paperHeight: CGFloat = 1600

    public override func safeAreaInsetsDidChange() {
        super.safeAreaInsetsDidChange()
        setNeedsLayout()
    }

    public override func layoutSubviews() {
        super.layoutSubviews()
        let inner = bounds.inset(by: safeAreaInsets)
        let h = inner.height
        /* A DROP WHILE AN AUTO-COLLAPSE IS ARMED IS THE FLIP (CollapseSlide),
         * judged against the height before it. It starts the run - whose
         * observer sets `from` and `travel` at once - and this same pass lays
         * the sheet out at the compact height with every rider's ride. */
        if h != handed {
            let before = handed
            handed = h
#if DEBUG
            if UtttRuler.on { UtttLog.note("ruler-height", String(format: "%.1f clock %d", h, MotionRuler.clockMs)) }
#endif
            _ = slide?.heard(h, after: before)
        }
        UIView.performWithoutAnimation {
            CATransaction.begin()
            CATransaction.setDisableActions(true)
            let ph = max(Self.paperHeight, bounds.height)
            paper.frame = CGRect(x: 0, y: bounds.height - ph, width: bounds.width, height: ph)
            content.frame = inner
            clip.frame = CGRect(x: 0, y: -travel, width: inner.width, height: inner.height + travel)
#if DEBUG
            rulerBottom?.frame = content.bounds
            rulerTop?.frame = content.bounds
            if let t = rulerTop { ride(t) { _ in CollapseRidePose(dy: 0) } }
#endif
            lay(inner.size, from: from)
            CATransaction.commit()
        }
    }

    /// The screen: lay everything out for `size` (the drawer less its safe
    /// area), and - through a slide - for the drawer height `from` it began at.
    func lay(_ size: CGSize, from: CGFloat?) {}

    /// Rides an auto-collapse as `pose` says for a push `s` (the drawer is
    /// the laid-out height plus `s`). Called at every layout, after the
    /// view's frame is set: registered once, its ride refreshed, and its
    /// animation rebuilt if a running slide was built for other bounds.
    func ride(_ v: UIView, _ pose: @escaping (CGFloat) -> CollapseRidePose) {
        guard let slide else { return }
        if ridden.insert(ObjectIdentifier(v)).inserted {
            slide.register(v, ride: pose)
        } else {
            slide.update(v, ride: pose)
        }
        slide.laidOut(v)
    }
    private var ridden = Set<ObjectIdentifier>()

    /// THE BOARD HOLDS THE CENTRE AND SCALES ABOUT IT, on every screen: the
    /// board's square at the kernel's `L`, riding an auto-collapse along the
    /// path the layout would walk - `at(s)` is the kernel's sheet for the
    /// drawer `s` points taller.
    func placeBoard(_ board: UIView, _ L: UtiSheet, at: @escaping (CGFloat) -> UtiSheet) {
        let side = CGFloat(L.board.2)
        board.frame = CGRect(x: CGFloat(L.board.0), y: CGFloat(L.board.1), width: side, height: side)
        ride(board) { s in
            let A = at(s)
            return CollapseRidePose(
                dy: CGFloat(A.board.1 + A.board.2 / 2 - L.board.1 - L.board.2 / 2),
                scale: side > 0 ? CGFloat(A.board.2) / side : 1)
        }
    }

    /// The words, twice - the column beside the ink and the band across the
    /// top - each shown only where it fits (uttt_sheet), riding an
    /// auto-collapse with the top and crossfading on their layers: the column
    /// copy toward its alpha at each height, the band copy (laid out as the
    /// slide's first frame had it, `B`) fading out.
    func placeWords(column: UIView, band: UIView, _ L: UtiSheet, _ B: UtiSheet,
                    at: @escaping (CGFloat) -> UtiSheet) {
        shown(column, L.words_alpha)
        shown(band, from != nil ? B.band_alpha : L.band_alpha)
        ride(column) { s in CollapseRidePose(dy: 0, alpha: CGFloat(at(s).words_alpha)) }
        ride(band) { s in CollapseRidePose(dy: 0, alpha: CGFloat(at(s).band_alpha)) }
    }

    /// Faded to `alpha`; out of VoiceOver and the touch path once it is
    /// mostly gone, so the one copy that shows is the one that is read.
    func shown(_ v: UIView, _ alpha: Float) {
        v.alpha = CGFloat(alpha)
        v.isUserInteractionEnabled = alpha > 0.5
        v.accessibilityElementsHidden = alpha < 0.5
    }

    /// The slide's start height's layout, while one runs.
    var slideFrom: CGFloat? { from }
}

public extension CollapseSlide {
    /// The auto-collapse's slide, on the kernel's curve and numbers
    /// (uttt_anim.h UTTT_COLLAPSE_*): the host's spring, pushed from the
    /// whole travel to nothing.
    static func uttt() -> CollapseSlide {
#if DEBUG
        if UtttRuler.on { CollapseSlide.probe = { UtttLog.note("slide", $0) } }
#endif
        return CollapseSlide(duration: Double(uti_collapse_ms()) / 1000,
                      steps: Int(uti_collapse_steps()),
                      flip: CGFloat(uti_collapse_flip())) { travel, t in
            CGFloat(uti_collapse_push(Float(travel), Int32((t * 1000).rounded())))
        }
    }
}
