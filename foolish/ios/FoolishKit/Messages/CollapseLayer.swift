// CollapseLayer - a piece of the board that rides the collapse on a Core
// Animation layer of its own.
//
// THE PROBLEM, measured before any of this was written. The auto-collapse moves
// two groups of things and they want opposite treatment. The hand and the
// action pills sit on the box's BOTTOM edge and must not move at all; the table
// cards, deck, discard and opponent ring sit at some fraction of the box's
// height and ride the drawer's descending top edge. The host moves our hosting
// view perfectly smoothly at the composite rate (filmed with our height held
// constant the edges run 420.0, 450.3, 479.7, 509.3 - not one reversal), but
// SwiftUI renders at ~60Hz under a render server compositing at 86-94Hz, so a
// third of the frames on screen carry our geometry from one render ago. At
// peak the drawer travels 3.6pt/ms; a render we did not draw is ~30pt of error.
//
// A keyframe animation on the hosting view's layer fixed the hand: the box is
// laid out COMPACT for the whole collapse and pushed down by the drawer's
// remaining travel, evaluated by the render server on every frame it
// composites, ours or not. Filmed jerk for the hand went 7684 -> ~20. But one
// layer carries one motion, and the table group then had to take that push
// back off itself in ordinary SwiftUI - a smooth value minus a stale one - so
// the judder was moved from the hand to the table, not removed. The numbers
// that say so, six takes each:
//
//     bar                    jerk      stray
//     green  hand            26        1587      <- fixed
//     magenta table cards    4420      729211    <- now carries the staleness
//     yellow  opponent       14096     2342132
//
// With ONE layer carrying translation T(t) and layout positions P_i sampled at
// render time, a view sits at hostTop(t) + T(t) + P_i(t_render). Holding the
// hand still needs T = -hostTop + c; a view a fraction f down the box wants to
// move by f of the box's shrink instead, which is T's share (1 - f) taken back
// - and taken back at the composite rate, which layout cannot do. Exactly one
// fraction can be composite-accurate per layer. The table needs several.
//
// THE DESIGN. Each such view is hosted in a `UIHostingController` of its own -
// a UIKit view with a real `CALayer` - and that layer gets the same keyframe
// animation as the hosting view's, scaled by -(1 - f). The two animations are
// evaluated by the render server on the same composited frame, so their sum is
// exact whatever SwiftUI is doing:
//
//     f = 0    deck, discard, the ruler's red bar    ride the drawer's top edge
//     f = 0.5  the battle                            half the box's shrink
//     y(h)     an opponent on the ring               the ring's own rule, whose
//                                                    radius opens as the box
//                                                    shrinks (CollapseRide.path)
//
// which is exactly where the old, fully SwiftUI-driven layout put them, minus
// the frame of staleness. The hand stays in the main tree and keeps the whole
// of the hosting layer's motion, as before. Six takes each, same rig:
//
//     bar                    jerk   floor   rough      jerk = floor is a bar
//     magenta table cards    3230    3062     221      that moves exactly as
//     yellow  opponent       9515    8623     611      the drawer does; rough
//     red     drawer top    13591   12580     874      is the judder itself
//
// against 4420 / 3744 and 14096 / 11701 before (jerk / rough).
//
// WHAT A NESTED HOST COSTS, and how each cost is paid here:
//
//   - PREFERENCES stop at a hosting boundary. The battle frames, the deck and
//     discard frames and the seat frames are all published by the views inside,
//     in `boardSpace`, and every flight on the board aims at them. So the
//     inner tree declares its own `boardSpace`, its values are relayed out
//     through `PreferenceRelay`, and the wrapper re-publishes them REBASED by
//     its own origin in the outer `boardSpace` - the existing
//     `onPreferenceChange` handlers in MessageTableView see the same keys with
//     the same meaning and are untouched.
//   - ENVIRONMENT stops there too, so the inner root is handed the outer
//     environment whole (`context.environment`).
//   - TRANSACTIONS stop there. A `withAnimation` in the outer tree reaches
//     the inner one only if the root is re-assigned inside that transaction,
//     so `updateUIViewController` does exactly that.
//   - THE LAYER IS NEVER LAID OUT BY SWIFTUI'S TRANSFORM. SwiftUI positions a
//     platform view through its centre and bounds; the keyframes animate the
//     presentation layer's `transform.translation.y` and leave the model
//     value alone, so the two compose instead of fighting.
//
// BEHIND THE SAME KNOB AS THE SLIDE. The wrapper is inert - it renders its
// content in place, in the main tree - unless `MessagesRootView` puts a
// `CollapseLayers` bus in the environment, and it does that only when the
// slide is on - `CollapseTween.slideByDefault`, which is true from build 71,
// or `slide=` in the DEBUG `dev.collapse` knob. With the slide off (`slide=0`)
// there are no nested hosts at all.

import SwiftUI

#if canImport(UIKit)
import UIKit

// MARK: - How a view rides

/// Where a view sits in its box as the box shrinks - the rule the layer walks.
public enum CollapseRide {
    /// A fixed fraction `f` of the box's height: the view moves by `f` of the
    /// box's shrink, so the layer takes back `1 - f` of the slide.
    case fraction(CGFloat)
    /// The view's y in its box as a FUNCTION of the box's height, with the
    /// height the box rests at now. For the opponent ring, whose radius opens
    /// with the collapse (`ringPoint`'s 0.35 -> 0.38): a fixed fraction would
    /// put it at its compact radius on the flip frame, 13pt above where the
    /// old layout had it that frame. With the layout's own rule the layer walks
    /// exactly the path the layout would have walked through every height in
    /// between - the difference is precisely a frame of staleness, gone.
    case path(rest: CGFloat, y: (CGFloat) -> CGFloat)

    /// The layer's offset when the hosting layer is pushed down by `s`: where
    /// the old layout would have put the view, minus where the compact layout
    /// has put it, minus the push it is already getting.
    func offset(push s: CGFloat) -> CGFloat {
        switch self {
        case .fraction(let f): return -(1 - f) * s
        case .path(let rest, let y): return y(rest + s) - y(rest) - s
        }
    }

    /// Enough of the curve to tell one ride from another, so an update that
    /// changes nothing does not reinstall keyframes mid-run.
    var signature: [CGFloat] {
        switch self {
        case .fraction(let f): return [f]
        case .path(let rest, let y): return [rest, y(rest), y(rest + 100), y(rest + 400)]
        }
    }
}

// MARK: - The bus

/// Every collapse layer on the board, and the one run they are all riding.
///
/// Owned by `MessagesRootView` beside the tween state, handed down through
/// the environment, and driven from the same place as the hosting view's own
/// keyframes - `follow`'s start case - in the same runloop turn, so all the
/// animations land in one `CATransaction` and the first composited frame is
/// already right.
@MainActor
public final class CollapseLayers {
    private struct Entry {
        weak var view: UIView?
        var ride: CollapseRide
    }
    /// A collapse in progress: what the hosting layer is being pushed down by
    /// and when it started, so a layer that registers or changes its fraction
    /// mid-run can join it in phase.
    private struct Run {
        let travel: CGFloat
        let duration: Double
        let response: Double
        let began: CFTimeInterval
    }

    private var entries: [ObjectIdentifier: Entry] = [:]
    private var run: Run?

    static let key = "cards.foolish.collapse.layer"

    public init() {}

    func register(_ view: UIView, ride: CollapseRide) {
        entries[ObjectIdentifier(view)] = Entry(view: view, ride: ride)
        if let run { install(run, on: view.layer, ride: ride) }
    }

    func update(_ view: UIView, ride: CollapseRide) {
        let id = ObjectIdentifier(view)
        guard var e = entries[id] else { return register(view, ride: ride) }
        let was = e.ride.signature, now = ride.signature
        guard was.count != now.count
            || zip(was, now).contains(where: { abs($0 - $1) > 0.0005 }) else { return }
        e.ride = ride
        entries[id] = e
        if let run { install(run, on: view.layer, ride: ride) }
    }

    func unregister(_ view: UIView) {
        entries.removeValue(forKey: ObjectIdentifier(view))
    }

    /// The collapse has flipped: the hosting layer is about to be pushed down
    /// by `travel` and eased to zero over `duration` on the host's curve. Give
    /// every registered layer its own share of the opposite motion.
    public func begin(travel: CGFloat, duration: Double,
                      response: Double = CollapseTween.hostResponse) {
        let r = Run(travel: travel, duration: duration, response: response,
                    began: CACurrentMediaTime())
        run = r
        sweep()
        for e in entries.values {
            guard let v = e.view else { continue }
            install(r, on: v.layer, ride: e.ride)
        }
    }

    /// The release, or a drag that interrupts. Actions disabled, for the same
    /// reason the hosting view's own release disables them: an implicit
    /// animation on the way out is a one-frame drop.
    public func end() {
        run = nil
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        for e in entries.values { e.view?.layer.removeAnimation(forKey: Self.key) }
        CATransaction.commit()
    }

    public var isRunning: Bool { run != nil }

    /// How many layers are riding the bus right now. For the trace.
    public var count: Int { entries.values.filter { $0.view != nil }.count }

    private func install(_ r: Run, on layer: CALayer, ride: CollapseRide) {
        layer.removeAnimation(forKey: Self.key)
        // The hosting layer's own keyframes, one for one, each turned into
        // this view's share. A view at fraction 1 takes the whole of the push
        // and needs nothing.
        let values = CollapseTween.slideOffsets(travel: r.travel, duration: r.duration,
                                                response: r.response)
            .map { ride.offset(push: $0) }
        guard values.contains(where: { abs($0) > 0.0005 }) else { return }
        let a = CAKeyframeAnimation(keyPath: "transform.translation.y")
        a.values = values
        a.duration = r.duration
        a.calculationMode = .linear
        // IN PHASE WITH THE RUN, not with this call: a layer that joins late
        // (an opponent that appeared, a fraction that changed with the box)
        // starts wherever the hosting layer already is.
        a.beginTime = layer.convertTime(r.began, from: nil)
        // Held at the end and removed by `end()`, exactly like the hosting
        // layer's: the model value is zero throughout, so letting the
        // animation fall off early would snap the view by its share of what is
        // left of the travel.
        a.fillMode = .both
        a.isRemovedOnCompletion = false
        layer.add(a, forKey: Self.key)
    }

    private func sweep() {
        entries = entries.filter { $0.value.view != nil }
    }
}

public struct CollapseLayersKey: EnvironmentKey {
    public static let defaultValue: CollapseLayers? = nil
}

public extension EnvironmentValues {
    /// The bus a collapse layer registers with, or nil when the slide is off -
    /// in which case `collapseLayer` renders its content in place.
    var collapseLayers: CollapseLayers? {
        get { self[CollapseLayersKey.self] }
        set { self[CollapseLayersKey.self] = newValue }
    }
}

// MARK: - Preferences across the boundary

/// A preference a nested host has to carry out to the board: what it publishes
/// inside, rebased into the outer `boardSpace` by the host's own origin there.
public protocol RelayedPreference: PreferenceKey where Value: Equatable & Sendable {
    static func rebased(_ value: Value, by origin: CGPoint) -> Value
}

extension RelayedPreference {
    /// Inside the host: catch the value, then TAKE IT BACK.
    ///
    /// A preference does not stop at a hosting boundary after all - SwiftUI
    /// bridges what a nested `UIHostingController` publishes up into the tree
    /// its representable sits in. Found the expensive way: the first throw-in
    /// filmed on a hosted board flew to `to=(284,42)`, the new slot's frame in
    /// the host's OWN space, because the raw value had reduced into the
    /// board's `battleFrames` a pass before the relay's rebased one could.
    /// So the value is caught here and the outgoing one reset to the default,
    /// and the relay is the only road out. If the bridge ever went away this
    /// would reset nothing and change nothing.
    static func tap<V: View>(_ v: V, into relay: PreferenceRelay) -> AnyView {
        AnyView(v.onPreferenceChange(Self.self) { relay.set(Self.self, $0) }
                 .transformPreference(Self.self) { $0 = Self.defaultValue })
    }

    /// Outside the host: publish it again, moved into the outer space.
    static func emit(from relay: PreferenceRelay, origin: CGPoint) -> AnyView {
        AnyView(Color.clear.preference(key: Self.self,
                                       value: rebased(relay.get(Self.self) ?? defaultValue,
                                                      by: origin)))
    }
}

extension BattleFramesKey: RelayedPreference {
    public static func rebased(_ v: [Int: CGRect], by o: CGPoint) -> [Int: CGRect] {
        v.mapValues { $0.offsetBy(dx: o.x, dy: o.y) }
    }
}
extension BattleCardFramesKey: RelayedPreference {
    public static func rebased(_ v: [String: CGRect], by o: CGPoint) -> [String: CGRect] {
        v.mapValues { $0.offsetBy(dx: o.x, dy: o.y) }
    }
}
extension SeatFramesKey: RelayedPreference {
    public static func rebased(_ v: [Int: CGRect], by o: CGPoint) -> [Int: CGRect] {
        v.mapValues { $0.offsetBy(dx: o.x, dy: o.y) }
    }
}
extension RoleMarkFramesKey: RelayedPreference {
    public static func rebased(_ v: [Int: CGRect], by o: CGPoint) -> [Int: CGRect] {
        v.mapValues { $0.offsetBy(dx: o.x, dy: o.y) }
    }
}
extension DeckFrameKey: RelayedPreference {
    public static func rebased(_ v: CGRect, by o: CGPoint) -> CGRect {
        v == .zero ? v : v.offsetBy(dx: o.x, dy: o.y)
    }
}
extension DiscardFrameKey: RelayedPreference {
    public static func rebased(_ v: CGRect, by o: CGPoint) -> CGRect {
        v == .zero ? v : v.offsetBy(dx: o.x, dy: o.y)
    }
}

/// The values a nested host has published, by key. An object rather than
/// `@State` because it is written from inside the host's own tree and read
/// from the wrapper's - two SwiftUI graphs with one owner between them.
public final class PreferenceRelay: ObservableObject {
    @Published private var values: [ObjectIdentifier: any Sendable] = [:]

    func set<K: RelayedPreference>(_ key: K.Type, _ value: K.Value) {
        let id = ObjectIdentifier(key)
        if let old = values[id] as? K.Value, old == value { return }
        values[id] = value
        #if DEBUG || SOLO_TESTING
        // In the host's own space; the wrapper adds its origin on the way out.
        FlightRecorder.note("relay", "\(K.self) \(value)")
        #endif
    }

    func get<K: RelayedPreference>(_ key: K.Type) -> K.Value? {
        values[ObjectIdentifier(key)] as? K.Value
    }
}

// MARK: - The wrapper

public extension View {
    /// Host this view on a layer of its own that takes back `1 - fraction` of
    /// the collapse's slide at the composite rate - see the file note.
    /// `relaying` names the preferences it publishes that the board reads.
    /// Inert (the view in place, no host) unless the environment carries a
    /// `CollapseLayers` bus.
    func collapseLayer(fraction: CGFloat,
                       relaying: [any RelayedPreference.Type] = []) -> some View {
        CollapseLayer(ride: .fraction(fraction), keys: relaying, content: self)
    }

    /// The same, for a view whose place in the box is not a fixed fraction of
    /// its height - see `CollapseRide.path`.
    func collapseLayer(ride: CollapseRide,
                       relaying: [any RelayedPreference.Type] = []) -> some View {
        CollapseLayer(ride: ride, keys: relaying, content: self)
    }
}

struct CollapseLayer<Content: View>: View {
    let ride: CollapseRide
    let keys: [any RelayedPreference.Type]
    let content: Content

    @Environment(\.collapseLayers) private var bus
    @StateObject private var relay = PreferenceRelay()

    var body: some View {
        if let bus {
            CollapseLayerHost(ride: ride, keys: keys, content: content,
                              bus: bus, relay: relay)
                // THE ORIGIN, from the same layout pass that placed the host,
                // and the re-publication in the same breath: a preference is
                // delivered a pass later than the layout it describes, and the
                // relayed frames are a host's own layout plus this - so the
                // lag is the one the board already had, not two of them.
                .background(GeometryReader { g in
                    let origin = g.frame(in: .named(boardSpace)).origin
                    ZStack {
                        ForEach(keys.indices, id: \.self) { i in
                            keys[i].emit(from: relay, origin: origin)
                        }
                    }
                })
        } else {
            content
        }
    }
}

private struct CollapseLayerHost<Content: View>: UIViewControllerRepresentable {
    let ride: CollapseRide
    let keys: [any RelayedPreference.Type]
    let content: Content
    let bus: CollapseLayers
    let relay: PreferenceRelay

    final class Coordinator {
        var bus: CollapseLayers?
    }

    func makeCoordinator() -> Coordinator { Coordinator() }

    func makeUIViewController(context: Context) -> UIHostingController<AnyView> {
        let h = UIHostingController(rootView: root(context))
        h.view.backgroundColor = .clear
        // A card's tilt, a bar drawn wider than its view: the content is
        // allowed to overhang its own box exactly as it does in the main tree.
        h.view.clipsToBounds = false
        // The host would otherwise inset its root by whatever of the window's
        // safe area it overlaps, and the keyboard; the main tree already
        // decided both for the whole box.
        if #available(iOS 16.4, *) { h.safeAreaRegions = [] }
        h.sizingOptions = []
        context.coordinator.bus = bus
        bus.register(h.view, ride: ride)
        return h
    }

    func updateUIViewController(_ h: UIHostingController<AnyView>, context: Context) {
        // INSIDE THE OUTER TRANSACTION, so a `withAnimation` around the state
        // change that reached here animates the inner tree the same way.
        withTransaction(context.transaction) { h.rootView = root(context) }
        bus.update(h.view, ride: ride)
    }

    static func dismantleUIViewController(_ h: UIHostingController<AnyView>,
                                          coordinator: Coordinator) {
        coordinator.bus?.unregister(h.view)
    }

    func sizeThatFits(_ proposal: ProposedViewSize, uiViewController h: UIHostingController<AnyView>,
                      context: Context) -> CGSize? {
        // The content's own answer to the same proposal the main tree would
        // have made it: a badge or a deck well reports its size, a grid with
        // `maxWidth: .infinity` fills the width it is offered.
        h.sizeThatFits(in: proposal.replacingUnspecifiedDimensions())
    }

    private func root(_ context: Context) -> AnyView {
        var v = AnyView(content
            // The board's named space, declared again in here: the frames the
            // content publishes resolve against this root, and the wrapper
            // rebases them into the outer one.
            .coordinateSpace(name: boardSpace))
        for k in keys { v = k.tap(v, into: relay) }
        return AnyView(v.environment(\.self, context.environment))
    }
}

#else

public enum CollapseRide {
    case fraction(CGFloat)
    case path(rest: CGFloat, y: (CGFloat) -> CGFloat)
}

public extension View {
    func collapseLayer(fraction: CGFloat, relaying: [Any] = []) -> some View { self }
    func collapseLayer(ride: CollapseRide, relaying: [Any] = []) -> some View { self }
}

#endif
