// CollapseSlide - a Messages app's auto-collapse, run by the render server.
//
// THE PROBLEM (measured first by the older sibling product's collapse layer;
// see docs/COLLAPSE_MSE.md). When the app asks Messages for
// the compact drawer, Messages hands the extension its new height once, about
// 20ms before the drawer slides, and then moves the extension's view at the
// composite rate (~90Hz on the simulator). The extension renders at ~60Hz.
// A layout that follows the drawer frame by frame is therefore a render behind
// on a third of the frames the screen shows, and at the slide's peak speed a
// render is tens of points: every element steps.
//
// THE DESIGN, ported. From the flip on, the sheet is laid out at its COMPACT
// size for the whole slide, and a keyframe animation on the hosting view's
// layer pushes it down by the drawer's remaining travel, so its bottom edge
// never moves. Each element that must not ride the bottom is hosted on a
// layer of its own (`collapseRide`), and that layer gets its share of the
// push back, evaluated by the render server on the same composited frame:
//
//     the header      all of it back           rides the drawer's top
//     the board       half of it back, scaled  holds the centre, and scales
//                                              as the layout would have
//     the doors       none of it               ride the bottom
//
// Each ride is a function of the push `s` (the drawer is the compact height
// plus `s` tall): the layer's offset, its scale about its centre and its
// opacity, so an element walks exactly the path the per-frame layout would
// have walked through every height between, minus the frame of staleness.
//
// WHAT IS THE PRODUCT'S: the curve (`push(t)`), how long it lasts, how many
// keyframes, the flip threshold, and every ride. This file has no numbers of
// its own and knows no game - uttt's all come from its kernel (uttt_anim.h,
// UTTT_COLLAPSE_*), because when and how the sheet moves is a decision.

#if canImport(UIKit)
import UIKit

/// Where an element sits as the drawer is `s` points taller than the height
/// the sheet is laid out at (a collapse: laid out compact, `s` > 0; an
/// expand: laid out tall, `s` < 0): its offset from its laid-out place,
/// measured from the drawer's top (points, down positive), its scale about
/// its centre (or `pivot`), and its opacity (nil: untouched). An element
/// that rides the drawer's BOTTOM has `dy == s`.
public struct CollapseRidePose {
    public var dy: CGFloat
    /// Sideways, for an element whose layout moves it across as the drawer
    /// changes (a label centred over a mark that grows).
    public var dx: CGFloat
    public var scale: CGFloat
    public var alpha: CGFloat?
    /// The point the scale is about, in the view's own bounds; nil for its
    /// centre.
    public var pivot: CGPoint?
    public init(dy: CGFloat, dx: CGFloat = 0, scale: CGFloat = 1, alpha: CGFloat? = nil,
                pivot: CGPoint? = nil) {
        self.dy = dy
        self.dx = dx
        self.scale = scale
        self.alpha = alpha
        self.pivot = pivot
    }
}

/// The run and every layer riding it, for one extension.
///
/// UIKIT AND CORE ANIMATION ONLY: a product registers each rider view
/// (`register`), refreshes its ride at every layout (`update`), says when it
/// was laid out (`laidOut`), and hears the run start and end (`observe`).
@MainActor
public final class CollapseSlide {
    /// A slide in progress: pushed down by `travel` at its start, over
    /// `duration`, from the drawer height `from` to `to`.
    public struct Run: Equatable {
        public let from: CGFloat
        public let to: CGFloat
        public let began: CFTimeInterval
        /// The auto-collapse: this slide pushes the hosting view itself.
        /// A run that FOLLOWS the host's own animation (a tap to expand, a
        /// drag released either way) pushes nothing; the riders only follow.
        public let pushes: Bool
        /// The drawer is getting shorter (the riders take the collapse's
        /// poses, `s` > 0), whoever moves it.
        public var shrinks: Bool { to < from }
        /// How far the drawer goes, either way.
        public var travel: CGFloat { abs(from - to) }
    }

    /// The push `s` (the drawer is the laid-out height plus `s`) `t` seconds
    /// into the run, over how long, and - for an expand - when the host's
    /// own animation began (0 until its transaction commits).
    private var curve: (Double) -> CGFloat = { _ in 0 }
    private var runDuration: Double = 0
    private var hostBegin: (() -> CFTimeInterval)?

    /// The product's curve: the push `t` seconds in, for a travel.
    public let push: (CGFloat, Double) -> CGFloat
    public let duration: Double
    public let steps: Int
    public let flip: CGFloat

    /// The slide, while it runs. Observed (`observe`) so a sheet can lay its
    /// riders out at the start height's pose where it needs to (an element
    /// hidden at the compact height fades out on its layer instead).
    public private(set) var run: Run? {
        didSet { if run != oldValue { for f in observers.values { f(run) } } }
    }

    /// Called with the run as it starts and with nil as it ends. Returns a
    /// token for `unobserve`.
    @discardableResult
    public func observe(_ f: @escaping (Run?) -> Void) -> Int {
        nextObserver += 1
        observers[nextObserver] = f
        return nextObserver
    }
    public func unobserve(_ token: Int) { observers[token] = nil }
    private var observers: [Int: (Run?) -> Void] = [:]
    private var nextObserver = 0

    /// The layer the whole sheet is pushed on: the extension's hosting view.
    public weak var host: UIView?

    /// Armed by the app right before it asks for compact itself: only then
    /// is a large drop the auto-collapse. A drag is never armed.
    private var armed = false
    private var release: DispatchWorkItem?

    private struct Entry {
        weak var view: UIView?
        var ride: (CGFloat) -> CollapseRidePose
        /// The bounds the running animation was built for.
        var built: CGRect?
    }
    private var entries: [ObjectIdentifier: Entry] = [:]

    static let key = "messageskit.collapse.slide"

    #if DEBUG
    /// DEBUG ONLY: where the slide's own record goes (every begin, install,
    /// rider layout and end, and each rider's layers every display frame of
    /// a run), with the time since the run began. A product sets it while
    /// its ruler is on, so a filmed frame can be read against what the
    /// layers were given.
    public static var probe: ((String) -> Void)?
    private var probeLink: CADisplayLink?
    private func note(_ s: @autoclosure () -> String) {
        guard let p = Self.probe else { return }
        let t = run.map { (CACurrentMediaTime() - $0.began) * 1000 } ?? -1
        p(String(format: "%+.1fms ", t) + s())
    }
    private func startProbe() {
        guard Self.probe != nil, probeLink == nil else { return }
        let l = CADisplayLink(target: ProbeTick(self), selector: #selector(ProbeTick.fire(_:)))
        l.add(to: .main, forMode: .common)
        probeLink = l
    }
    fileprivate func probeFrame() {
        guard run != nil else { probeLink?.invalidate(); probeLink = nil; return }
        for e in entries.values { if let v = e.view { note("frame " + Self.describe(v.layer)) } }
    }
    static func describe(_ l: CALayer) -> String {
        let p = l.presentation()
        let pt = p?.transform ?? l.transform
        var out = String(format: "%p b=%@ pos=%@ pres.pos=%@ pres.ty=%.1f pres.sy=%.3f keys=%@",
                         unsafeBitCast(l, to: Int.self),
                         NSCoder.string(for: l.bounds), NSCoder.string(for: l.position),
                         NSCoder.string(for: p?.position ?? .zero), pt.m42, pt.m22,
                         (l.animationKeys() ?? []).joined(separator: ","))
        /* The content drawn inside the rider: every layer of it with
         * a picture at least 100pt wide, in the rider's coordinates. */
        func walk(_ c: CALayer, _ depth: Int) {
            for s in c.sublayers ?? [] {
                if s.bounds.width >= 100 {
                    let f = s.convert(s.bounds, to: l)
                    out += String(format: " | d%d %@ %@ fr=%@ tr=%.1f/%.3f c=%d keys=%@", depth,
                                  String(describing: type(of: s)), NSCoder.string(for: f),
                                  NSCoder.string(for: s.frame), s.transform.m42, s.transform.m22,
                                  s.contents != nil ? 1 : 0,
                                  (s.animationKeys() ?? []).joined(separator: ","))
                }
                if depth < 8 { walk(s, depth + 1) }
            }
        }
        walk(l, 0)
        return out
    }
    private final class ProbeTick: NSObject {
        weak var slide: CollapseSlide?
        init(_ s: CollapseSlide) { slide = s }
        @objc func fire(_ l: CADisplayLink) {
            guard let slide else { l.invalidate(); return }
            MainActor.assumeIsolated { slide.probeFrame() }
        }
    }
    #endif

    public init(duration: Double, steps: Int, flip: CGFloat,
                push: @escaping (CGFloat, Double) -> CGFloat) {
        self.duration = duration
        self.steps = max(2, steps)
        self.flip = flip
        self.push = push
    }

    /// The app is about to ask for compact.
    public func arm() { armed = true }

    /// Stand down an arm that no collapse followed.
    public func disarm() {
        armed = false
        if run == nil { drain() }
    }

    /// Run `f` once no slide is running or armed: a whole-screen swap in the
    /// middle of one would land on a hosting view with no push, a jump of
    /// the whole remaining travel.
    public func whenStill(_ f: @escaping () -> Void) {
        if run == nil && !armed { f() } else { waiting.append(f) }
    }
    private var waiting: [() -> Void] = []
    private func drain() {
        let w = waiting
        waiting = []
        w.forEach { $0() }
    }

    /// A height was handed, after `previous`. Returns true when this is the
    /// flip - the sheet must lay out at `height` now, with no spring - and
    /// starts the slide in the same runloop turn, so the layout at the
    /// compact height and the push land in one transaction. A height that
    /// grows while a slide runs (a finger caught the drawer) ends it.
    public func heard(_ height: CGFloat, after previous: CGFloat) -> Bool {
        if let r = run {
            /* A new height under a run the host drives is a new gesture or
             * a new animation of the host's: this one is over. */
            if !r.pushes || height > previous + 1 { end() }
            return false
        }
        guard armed, previous - height > flip else { return false }
        armed = false
        #if DEBUG
        NSLog("collapse-slide begin %.1f -> %.1f", previous, height)
        #endif
        begin(from: previous, to: height)
        #if DEBUG
        note("begin \(previous) -> \(height)")
        startProbe()
        #endif
        return true
    }

    private func begin(from: CGFloat, to: CGFloat) {
        let travel = from - to
        curve = { [push] t in push(travel, t) }
        runDuration = duration
        hostBegin = nil
        let r = Run(from: from, to: to, began: CACurrentMediaTime(), pushes: true)
        run = r
        let pushes = samples()
        if let layer = host?.layer {
            let a = CAKeyframeAnimation(keyPath: "transform.translation.y")
            a.values = pushes
            a.duration = duration
            a.calculationMode = .linear
            /* HELD AT THE END and removed by `end`, in one transaction with
             * every rider's: the model value is zero throughout. */
            a.fillMode = .both
            a.isRemovedOnCompletion = false
            layer.add(a, forKey: Self.key)
        }
        entries = entries.filter { $0.value.view != nil }
        for (id, e) in entries {
            if let v = e.view { install(r, on: v.layer, ride: e.ride); entries[id]?.built = v.bounds }
        }
        let w = DispatchWorkItem { [weak self] in self?.end() }
        release = w
        DispatchQueue.main.asyncAfter(deadline: .now() + duration, execute: w)
    }

    /// THE HOST'S OWN MOVE, either way: the host handed a new height and is
    /// animating the extension's view there itself, on its own spring - a
    /// tap to expand, or a drag released up or down (a flick: the finger
    /// lets go mid-drawer and the host carries the rest; TESTFLIGHT_PLAN 14,
    /// 17). The sheet is laid out at the new height from the first frame;
    /// every rider is carried on the render server by where the layout for
    /// the drawer's height at that moment would put it - `left(t)`, the
    /// points the host still has to go `t` seconds in (positive, toward
    /// `to`), read off its own animation and evaluated by the product - over
    /// the host's `duration`, from the host's own begin time (`begin`, 0
    /// while its transaction is open), so the two run on the same composited
    /// frames. Nothing is pushed.
    public func follow(from: CGFloat, to: CGFloat, duration: Double,
                       begin: @escaping () -> CFTimeInterval,
                       left: @escaping (Double) -> CGFloat) {
        if run != nil { end() }
        armed = false
        /* s: the drawer less the laid-out height - positive while a shrink
         * has yet to land, negative while a growth has. */
        let sign: CGFloat = to < from ? 1 : -1
        curve = { t in sign * left(t) }
        runDuration = duration
        hostBegin = begin
        let r = Run(from: from, to: to, began: CACurrentMediaTime(), pushes: false)
        run = r
        #if DEBUG
        NSLog("collapse-slide follow %.1f -> %.1f", from, to)
        note("follow \(from) -> \(to)")
        startProbe()
        #endif
        entries = entries.filter { $0.value.view != nil }
        for (id, e) in entries {
            if let v = e.view { install(r, on: v.layer, ride: e.ride); entries[id]?.built = v.bounds }
        }
        let w = DispatchWorkItem { [weak self] in self?.end() }
        release = w
        DispatchQueue.main.asyncAfter(deadline: .now() + duration, execute: w)
    }

    /// The release, or a drag that interrupts: every animation off in one
    /// transaction with actions disabled, since an implicit animation on the
    /// way out is a one-frame drop.
    public func end() {
        release?.cancel()
        release = nil
        guard run != nil else { return }
        #if DEBUG
        note("end")
        #endif
        run = nil
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        host?.layer.removeAnimation(forKey: Self.key)
        for e in entries.values { e.view?.layer.removeAnimation(forKey: Self.key) }
        CATransaction.commit()
        drain()
    }

    public func register(_ view: UIView, ride: @escaping (CGFloat) -> CollapseRidePose) {
        entries[ObjectIdentifier(view)] = Entry(view: view, ride: ride)
        if let run { install(run, on: view.layer, ride: ride) }
    }

    public func update(_ view: UIView, ride: @escaping (CGFloat) -> CollapseRidePose) {
        let id = ObjectIdentifier(view)
        #if DEBUG
        if run != nil { note("update " + Self.describe(view.layer)) }
        #endif
        /* A RIDE REFRESHED UNDER A RUNNING SLIDE IS REBUILT at the next
         * `laidOut`, in phase: the flip is heard before the sheet is laid
         * out at the compact height, so the rides the run was installed with
         * were the tall layout's. */
        entries[id] = Entry(view: view, ride: ride, built: run != nil ? nil : entries[id]?.built)
    }

    /// A rider was laid out. A scale about a pivot is built from the layer's
    /// bounds, and a rider can be laid out at the compact size after the flip
    /// was heard - so a rider whose bounds changed under a running slide gets
    /// its animation rebuilt, in phase, in the same layout pass.
    public func laidOut(_ view: UIView) {
        let id = ObjectIdentifier(view)
        #if DEBUG
        if run != nil { note("laidOut " + Self.describe(view.layer)) }
        #endif
        guard let run, let e = entries[id], e.built != view.bounds else { return }
        install(run, on: view.layer, ride: e.ride)
        entries[id]?.built = view.bounds
    }

    public func unregister(_ view: UIView) {
        entries.removeValue(forKey: ObjectIdentifier(view))
    }

    private func samples() -> [CGFloat] {
        (0...steps).map { curve(runDuration * Double($0) / Double(steps)) }
    }

    private func install(_ r: Run, on layer: CALayer,
                         ride: (CGFloat) -> CollapseRidePose) {
        layer.removeAnimation(forKey: Self.key)
        let poses = samples().map { s -> CollapseRidePose in
            var p = ride(s)
            if r.pushes { p.dy -= s }    // the push it is already getting
            return p
        }
        let g = CAAnimationGroup()
        var parts: [CAAnimation] = []
        let t = CAKeyframeAnimation(keyPath: "transform")
        /* ABOUT THE PIVOT, whatever the anchor: a scale about the layer's
         * anchor is followed by the move that puts the pivot back. A rider
         * is placed by its frame, never by a transform of its own - this
         * animation replaces the transform (filmed once: the board flew to
         * the sheet's top left). */
        let b = layer.bounds, ap = layer.anchorPoint
        #if DEBUG
        note(String(format: "install %p b=%@ pos=%@ pose0 dy=%.1f sc=%.3f poseEnd dy=%.1f sc=%.3f",
                    unsafeBitCast(layer, to: Int.self),
                    NSCoder.string(for: b), NSCoder.string(for: layer.position),
                    poses.first?.dy ?? 0, poses.first?.scale ?? 1,
                    poses.last?.dy ?? 0, poses.last?.scale ?? 1))
        #endif
        t.values = poses.map { p -> NSValue in
            let pv = p.pivot ?? CGPoint(x: b.midX, y: b.midY)
            let cx = pv.x - (b.minX + ap.x * b.width), cy = pv.y - (b.minY + ap.y * b.height)
            return NSValue(caTransform3D: CATransform3DScale(
                CATransform3DMakeTranslation(p.dx - (p.scale - 1) * cx,
                                             p.dy - (p.scale - 1) * cy, 0),
                p.scale, p.scale, 1))
        }
        t.calculationMode = .linear
        parts.append(t)
        if poses.contains(where: { $0.alpha != nil }) {
            let o = CAKeyframeAnimation(keyPath: "opacity")
            o.values = poses.map { Float($0.alpha ?? 1) }
            o.calculationMode = .linear
            parts.append(o)
        }
        g.animations = parts
        g.duration = runDuration
        /* IN PHASE WITH THE RUN, not with this call: a layer that joins
         * late starts wherever the hosting layer already is. An expand is
         * in phase with the HOST's animation: its begin time once committed,
         * and until then none, so both are stamped by the same commit. */
        if let hostBegin {
            let b = hostBegin()
            if b > 0 { g.beginTime = b }
        } else {
            g.beginTime = layer.convertTime(r.began, from: nil)
        }
        g.fillMode = .both
        g.isRemovedOnCompletion = false
        layer.add(g, forKey: Self.key)
    }
}

#endif
