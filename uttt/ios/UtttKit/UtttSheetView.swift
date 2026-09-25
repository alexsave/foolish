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
    /// How far the clip reaches past the sheet through a run: up by the
    /// auto-collapse's push (the drawer's top is above the pushed sheet),
    /// down by a followed shrink's travel (the drawer's bottom, where the
    /// doors ride, is below the sheet laid out short).
    private var reachUp: CGFloat = 0
    private var reachDown: CGFloat = 0
    /// How far the host's spring carries a followed growth past the handed
    /// height (`uti_spring_past`): the drawer's bottom, where the doors and
    /// the ruler's bar ride, is below the sheet laid out at that height
    /// while it overshoots - cut off there, filmed (TESTFLIGHT_PLAN 18).
    private var past: CGFloat = 0
    /// The drawer is growing toward the laid-out height (an expand), not
    /// shrinking toward it.
    private(set) var grows = false
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
            self.grows = run.map { !$0.shrinks } ?? false
            self.reachUp = run.map { $0.pushes ? $0.travel : 0 } ?? 0
            self.reachDown = run.map { $0.pushes ? 0 : $0.shrinks ? $0.travel : self.past } ?? 0
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
            if UtttRuler.on {
                UtttLog.note("ruler-height", String(format: "%.1f clock %d inset %.1f %.1f bounds %.1f", h,
                                                    MotionRuler.clockMs, safeAreaInsets.top, safeAreaInsets.bottom,
                                                    bounds.height))
            }
#endif
            _ = slide?.heard(h, after: before)
            if before > 0, abs(h - before) > 1, slide?.run == nil { rideHost(to: h, jump: abs(h - before) > 60) }
        }
        UIView.performWithoutAnimation {
            CATransaction.begin()
            CATransaction.setDisableActions(true)
            let ph = max(Self.paperHeight, bounds.height)
            paper.frame = CGRect(x: 0, y: bounds.height - ph, width: bounds.width, height: ph)
            content.frame = inner
            clip.frame = CGRect(x: 0, y: -reachUp, width: inner.width, height: inner.height + reachUp + reachDown)
#if DEBUG
            rulerBottom?.frame = content.bounds
            rulerTop?.frame = content.bounds
            if let t = rulerTop { ride(t) { _ in CollapseRidePose(dy: 0) } }
            if let b = rulerBottom { ride(b) { s in CollapseRidePose(dy: s) } }
#endif
            lay(inner.size, from: from)
            CATransaction.commit()
        }
    }

    /// THE HOST IS MOVING THE DRAWER ON ITS OWN SPRING, either way: a tap to
    /// expand, a drag released upward, or a drag released DOWNWARD - a
    /// flick lets go mid-drawer, Messages hands the final height once and
    /// animates the rest itself (TESTFLIGHT_PLAN 17: laid out short at once,
    /// the board shrank in one frame and the doors jumped to mid-drawer).
    /// The height it handed is the final one, and its animation on this
    /// view's bounds says how it gets there - read off the layer (mass,
    /// stiffness, damping, velocity, duration, the height it started from),
    /// and handed to the slide with the kernel's curve for it
    /// (`uti_spring_left`), so every rider follows the drawer's edges on the
    /// same composited frames. A height handed with no animation (a finger
    /// on the handle) is followed by the layout alone.
    private func rideHost(to h: CGFloat, jump: Bool) {
        guard let slide else { return }
        for key in layer.animationKeys() ?? [] {
            guard let a = layer.animation(forKey: key) as? CASpringAnimation,
                  a.keyPath == "bounds.size", a.isAdditive,
                  let from = (a.fromValue as? NSValue)?.cgSizeValue, abs(from.height) > 1
            else { continue }
            /* additive: the presentation is the model plus `from` easing
             * to zero, so the drawer started `from.height` off the new one */
            let travel = abs(from.height)
            let m = Float(a.mass), k = Float(a.stiffness), c = Float(a.damping)
            let v0 = Float(a.initialVelocity)
            past = CGFloat(uti_spring_past(Float(travel), m, k, c, v0))
            UtttLog.note("host-move", String(format: "%.1f -> %.1f over %.3fs (m %.2f k %.1f c %.2f v0 %.2f) past %.1f",
                                             h + from.height, h, a.duration, m, k, c, v0, past))
            slide.follow(from: h + from.height, to: h, duration: a.duration,
                         begin: { [weak self] in self?.layer.animation(forKey: key)?.beginTime ?? 0 },
                         left: { t in
                             CGFloat(uti_spring_left(Float(travel), m, k, c, v0, Int32((t * 1000).rounded())))
                         })
            return
        }
#if DEBUG
        let keys = (layer.animationKeys() ?? []).map { k -> String in
            let a = layer.animation(forKey: k)
            return "\(k):\(type(of: a as Any)):\((a as? CAPropertyAnimation)?.keyPath ?? "-")"
        }
        if jump { UtttLog.note("host-still", String(format: "%.1f, animations %@", h, keys.joined(separator: " "))) }
#endif
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
    /// A kernel box (x, y, w, h in sheet points) as a frame.
    func rect(_ r: (Float, Float, Float, Float)) -> CGRect {
        CGRect(x: CGFloat(r.0), y: CGFloat(r.1), width: CGFloat(r.2), height: CGFloat(r.3))
    }

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
    ///
    /// THE COPY SHOWING AS A SLIDE STARTS IS SET AT THE START'S LAYOUT (`B`):
    /// the band through a collapse, the column through an expand - the other
    /// at the end's (`L`). `wordsBox` says which box each copy takes.
    func placeWords(column: UIView, band: UIView, _ L: UtiSheet, _ B: UtiSheet,
                    at: @escaping (CGFloat) -> UtiSheet) {
        shown(column, grows ? B.words_alpha : L.words_alpha)
        shown(band, from != nil && !grows ? B.band_alpha : L.band_alpha)
        ride(column) { s in CollapseRidePose(dy: 0, alpha: CGFloat(at(s).words_alpha)) }
        ride(band) { s in CollapseRidePose(dy: 0, alpha: CGFloat(at(s).band_alpha)) }
    }

    /// The layouts the two copies of the words are set at (see `placeWords`).
    func wordsLayouts(_ L: UtiSheet, _ B: UtiSheet) -> (column: UtiSheet, band: UtiSheet) {
        grows ? (B, L) : (L, B)
    }

    /// Rides the drawer's bottom edge (the doors): none of a collapse's push,
    /// all of an expand's growth.
    func rideBottom(_ v: UIView) { ride(v) { s in CollapseRidePose(dy: s) } }

    /// A DOOR THAT BELONGS TO THE EXPANDED VIEW (Again, Copy code): at its
    /// box, faded to the kernel's `door_alpha`, riding the bottom - and
    /// through a run its opacity rides too, from the start height's to the
    /// end's, so a collapse fades it out with the drawer and an expand fades
    /// it in (it was hidden in the collapse's first frame: filmed on a
    /// device, TESTFLIGHT_PLAN 17). Hidden only when no run needs it.
    func placeDoor(_ v: UIView, _ box: (Float, Float, Float, Float), _ L: UtiSheet,
                   at: @escaping (CGFloat) -> UtiSheet) {
        v.frame = rect(box)
        v.alpha = CGFloat(L.door_alpha)
        v.isHidden = L.door_alpha <= 0 && from == nil
        v.isUserInteractionEnabled = L.door_alpha > 0.5
        ride(v) { s in CollapseRidePose(dy: s, alpha: CGFloat(at(s).door_alpha)) }
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
