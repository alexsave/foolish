import CUttt
import QuartzCore
import SwiftUI

/// THE ONE LOOP. A display link that asks the kernel what the board looks
/// like at this frame's presentation time and publishes the answer.
///
/// It decides nothing: which strokes move, in what order and for how long
/// is `uti_motion` / `uti_motion_at` (uttt/c/src/uttt_anim.c). This file owns
/// a clock and when to stop asking, which is when the kernel says nothing
/// will change again.
@MainActor
public final class UtttMotionClock: ObservableObject {
    @Published public private(set) var frame: UtiFrame {
        didSet { for f in observers.values { f() } }
    }

    /// THE LAYERS THAT DRAW A FRAME ARE CALLED DIRECTLY, not through SwiftUI:
    /// a SwiftUI view that observed `frame` re-rendered through RenderBox on
    /// the main thread every display frame (UtttLiveBoard). Returns a token
    /// for `unobserve`.
    public func observe(_ f: @escaping () -> Void) -> Int {
        nextObserver += 1
        observers[nextObserver] = f
        return nextObserver
    }
    public func unobserve(_ token: Int) { observers[token] = nil }
    private var observers: [Int: () -> Void] = [:]
    private var nextObserver = 0
    private var plan: UtiMotion
    private var link: CADisplayLink?
    private var origin: CFTimeInterval?
    private var landed = false
    private var onLanded: (() -> Void)?
    private var ticks = 0
    private var settledWaiters: [() -> Void] = []

    /// Runs `f` once the kernel says the board has settled (the ink is down
    /// and the wash has arrived), or now if it already has. The host inserts
    /// its bubble here: an insert stalls the whole screen for a few frames,
    /// and it must not land on the highlighter mid-travel.
    public func whenSettled(_ f: @escaping () -> Void) {
        if frame.settled != 0 || link == nil { f(); return }
        settledWaiters.append(f)
    }

    private func settle() {
        let w = settledWaiters
        settledWaiters = []
        w.forEach { $0() }
    }

    public init() {
        plan = Uttt.motion(.still)
        frame = Uttt.frame(plan, at: 0)
    }

    /// Play the resident game's last move through `ch`. `onLanded` runs once,
    /// on the first frame the ink is down (UI.html: the drawer auto-collapses
    /// once the ink lands, never during), or at once if nothing moves.
    public func run(_ ch: Uttt.Channel, onLanded: (() -> Void)? = nil) {
        plan = Uttt.motion(ch)
        origin = nil
        landed = false
        ticks = 0
        self.onLanded = onLanded
        frame = Uttt.frame(plan, at: 0)
        guard frame.running != 0 else {
            stop()
            land()
            return
        }
        UtttLog.note("motion", "ch \(plan.ch) mv \(plan.mv) ink \(plan.ink_ms) wash \(plan.wash_at)+\(plan.wash_ms) end \(plan.end_ms)")
        if link == nil {
            let l = CADisplayLink(target: Tick(self), selector: #selector(Tick.fire(_:)))
            l.preferredFrameRateRange = CAFrameRateRange(minimum: 60, maximum: 120, preferred: 120)
            l.add(to: .main, forMode: .common)
            link = l
        }
    }

    public func stop() {
        link?.invalidate()
        link = nil
        settle()
        let w = doneWaiters
        doneWaiters = []
        w.forEach { $0() }
    }

    /// Runs `f` once the whole plan has run - ink, highlighter,
    /// settlement - or now if nothing is moving.
    public func whenDone(_ f: @escaping () -> Void) {
        if link == nil { f(); return }
        doneWaiters.append(f)
    }
    private var doneWaiters: [() -> Void] = []

    private func land() {
        guard !landed else { return }
        landed = true
        let f = onLanded
        onLanded = nil
        f?()
    }

    fileprivate func tick(_ l: CADisplayLink) {
        /* THE CLOCK STARTS ON THE FIRST FRAME THE BOARD CAN BE SEEN. A board
         * that is still rasterising (the first of a process lands a few
         * frames late, off the main thread) would otherwise miss the start
         * of its own ink. */
        guard UtttBoard.underReady else { return }
        let now = l.targetTimestamp
        if origin == nil { origin = now }
        let ms = Int32(((now - (origin ?? now)) * 1000).rounded())
        frame = Uttt.frame(plan, at: ms)
        ticks += 1
        if frame.landed != 0 { land() }
        if frame.settled != 0 { settle() }
        if frame.running == 0 {
            UtttLog.note("motion done", "\(ms) ms, \(ticks) frames")
            stop()
        }
    }

    /// CADisplayLink retains its target; this breaks the cycle.
    private final class Tick: NSObject {
        weak var clock: UtttMotionClock?
        init(_ c: UtttMotionClock) { clock = c }
        @objc func fire(_ l: CADisplayLink) {
            guard let clock else { l.invalidate(); return }
            MainActor.assumeIsolated { clock.tick(l) }
        }
    }
}
