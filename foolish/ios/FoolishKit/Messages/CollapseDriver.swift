// CollapseDriver - the clock that moves the box through an auto-collapse.
//
// The DECISION of what height the box should be at any instant is
// `CollapseTween.height(from:to:at:)`, a pure function of time. This is the
// part that cannot be pure: a timer that asks it on every tick and hands the
// answer to the view. See the note at the top of CollapseTween for why this is
// a timer and not `withAnimation`.
//
// One driver per collapse. `start` begins the clock, `retarget` re-points the
// end of the curve (eased, never stepped), `stop` ends it early - a manual drag
// mid-collapse hands the box straight back to the model - and `onDone` fires
// once when the run is over, on the main actor, so the view can release the
// box through its re-measure nudge exactly as before.

import CoreGraphics
import Foundation
import QuartzCore

@MainActor
final class CollapseDriver {
    private var timer: DispatchSourceTimer?
    private var from: CGFloat = 0
    private var startedAt: CFTimeInterval = 0
    private var lead: Double = 0
    private var response: Double = CollapseTween.hostResponse
    /// The target the curve ends on, and - while a retarget eases - where it
    /// is easing from and since when.
    private var target: CGFloat = 0
    private var targetWas: CGFloat = 0
    private var retargetedAt: CFTimeInterval = -1

    /// The height the driver last committed, for a retarget that arrives before
    /// the first tick and for the release.
    private(set) var height: CGFloat = 0

    /// Begin a collapse from `from` to `to`. `tick` is called on the main actor
    /// with each new height, `onDone` once when the run ends by itself.
    func start(from: CGFloat, to: CGFloat,
               lead: Double = CollapseTween.hostLead,
               hz: Double = CollapseTween.driveHz,
               response: Double = CollapseTween.hostResponse,
               duration: Double = CollapseTween.driveDuration,
               tick: @escaping (CGFloat) -> Void,
               onDone: @escaping () -> Void) {
        stop()
        self.from = from
        self.target = to
        self.targetWas = to
        self.retargetedAt = -1
        self.lead = lead
        self.response = response
        self.startedAt = CACurrentMediaTime()
        // The first height NOW, in the same runloop turn as the report that
        // started this, so the first commit after the flip is already the
        // right one rather than the expanded box again.
        let h0 = evaluate(at: startedAt)
        height = h0
        tick(h0)
        let t = DispatchSource.makeTimerSource(flags: .strict, queue: .main)
        let period = max(1.0 / 240, 1.0 / max(1, hz))
        t.schedule(deadline: .now() + period, repeating: period, leeway: .nanoseconds(0))
        stamps = [startedAt]
        t.setEventHandler { [weak self] in
            guard let self, let running = self.timer else { return }
            let now = CACurrentMediaTime()
            self.stamps.append(now)
            if now - self.startedAt >= duration {
                running.cancel()
                self.timer = nil
                self.report()
                onDone()
                return
            }
            let h = self.evaluate(at: now)
            // A commit is not free: skip the frame if nothing moved by a
            // hundredth of a point, which is the settled tail of the spring.
            if abs(h - self.height) < 0.01 { return }
            self.height = h
            tick(h)
        }
        timer = t
        t.resume()
    }

    /// A later, taller report: ease the end of the curve up to it rather than
    /// stepping. See `CollapseTween.step` for why only upward.
    func retarget(to: CGFloat) {
        guard timer != nil else { return }
        // The effective target right now becomes the start of the ease, so a
        // second retarget mid-ease does not snap back to the first one's start.
        targetWas = effectiveTarget(at: CACurrentMediaTime())
        target = to
        retargetedAt = CACurrentMediaTime()
    }

    var isRunning: Bool { timer != nil }

    /// End the run early. `onDone` is NOT called: the caller is already
    /// releasing the box.
    func stop() {
        timer?.cancel()
        timer = nil
    }

    /// When each tick actually ran, for the trace below.
    private var stamps: [CFTimeInterval] = []

    /// DEBUG trace, one line per collapse: how the ticks were really spaced.
    /// A tick asked for every 8.3ms that arrives every 16.7ms says the main
    /// thread's layout is the ceiling, not the timer; a tick that arrives on
    /// time while the film still shows 60Hz says the render server is.
    private func report() {
        guard AnimLog.on, stamps.count > 2 else { return }
        var hist: [Int: Int] = [:]
        for (a, b) in zip(stamps, stamps.dropFirst()) {
            hist[Int(((b - a) * 1000).rounded()), default: 0] += 1
        }
        let first = stamps.prefix(40).enumerated().dropFirst()
            .map { String(format: "%.1f", (stamps[$0.offset] - stamps[$0.offset - 1]) * 1000) }
        AnimLog.say("collapse driver ticks=\(stamps.count - 1) "
                    + "intervals(ms:count)=\(hist.sorted { $0.key < $1.key }.map { "\($0.key):\($0.value)" }.joined(separator: " ")) "
                    + "first40=\(first.joined(separator: ","))")
    }

    private func effectiveTarget(at now: CFTimeInterval) -> CGFloat {
        guard retargetedAt >= 0 else { return target }
        return CollapseTween.retargetBlend(from: targetWas, to: target, at: now - retargetedAt)
    }

    private func evaluate(at now: CFTimeInterval) -> CGFloat {
        CollapseTween.height(from: from, to: effectiveTarget(at: now),
                             at: now - startedAt + lead, response: response)
    }
}
