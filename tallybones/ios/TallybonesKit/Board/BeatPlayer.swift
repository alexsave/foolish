// BeatPlayer.swift - plays the kernel's timeline (tallybones/c/src/tb_beats.h)
// on the dice tray. Adapted from pickemup/ios/PickemupKit/Board/BeatPlayer.swift.
//
// NO DURATION, CURVE OR ORDER LIVES HERE (T9, T21). A plan is what the kernel
// laid out when a bubble was adopted (tb_api_adopt) or my own was sent
// (tb_api_mark_sent); this holds which plan is current and when it began, and
// every frame asks C two things: the board as of now (tb_api_beats_frame: the
// face each die shows, blank while it waits in the cup, a blur while it
// tumbles, its value once it lands) and the settle beat's transform for that
// die (tb_api_beat_sample: the drop, the spin, the landing swell). What is
// left for Swift is which anchor a sample moves: die i is the tray's die.i.
//
// ONE PLAN AT A TIME, AND A NEW ONE WINS: `play` replaces whatever was
// playing, mid-flight (the clear-not-revert rule), and the new plan starts
// from the board the kernel says it starts from.
//
// No `held` plan (pickemup's staged-draft hold): Tallybones stages a move
// without motion, since a staged KEEP's reroll does not exist yet (T11), so
// every plan here ends and the settled view takes over.
//
// The kernel's plan also has STAMP, TURN and FADE beats (a score landing in
// its row, the turn bar passing, the results). The tray draws the dice only;
// the card and the badges show the settled view (DECISIONS T62).

import QuartzCore
import SwiftUI

/// One die as drawn at one moment.
public struct DieFrame: Equatable, Sendable {
    public var rotation: Double = 0     // degrees about Z
    public var scale: CGFloat = 1
    public var dy: CGFloat = 0          // points, up is negative
    /// The face to draw instead of the die's own value while the plan runs:
    /// 0 blank (still in the cup), else the kernel's face for this moment.
    public var face: Int?
    public init(rotation: Double = 0, scale: CGFloat = 1, dy: CGFloat = 0, face: Int? = nil) {
        self.rotation = rotation
        self.scale = scale
        self.dy = dy
        self.face = face
    }
    public static let rest = DieFrame()
}

@MainActor
public protocol DiceMotion: AnyObject {
    /// A frame can still change: the tray's timeline runs.
    var animating: Bool { get }
    /// Die `i` at `t`, or nil: draw it at rest.
    func frame(die i: Int, at t: CFTimeInterval) -> DieFrame?
}

@MainActor
public final class BeatPlayer: ObservableObject, DiceMotion {

    /// The current plan, or nil: the tray shows the settled view.
    @Published public private(set) var plan: TbBeatsSnap?
    @Published public private(set) var animating = false

    private var began: CFTimeInterval = 0
    private var endWork: DispatchWorkItem?
    /// The clock, replaceable by a test.
    public var now: () -> CFTimeInterval = { CACurrentMediaTime() }

    public init() {}

    /// Play `plan` from now. nil (the kernel laid nothing out) or a plan with
    /// no beats clears: the settled view, no motion.
    public func play(_ plan: TbBeatsSnap?) {
        endWork?.cancel()
        endWork = nil
        guard let plan, !plan.beat.isEmpty else {
            clear()
            return
        }
        self.plan = plan
        began = now()
        animating = true
        let serial = plan.serial
        let work = DispatchWorkItem { [weak self] in self?.ended(serial) }
        endWork = work
        DispatchQueue.main.asyncAfter(deadline: .now() + Double(plan.totalMs) / 1000, execute: work)
    }

    public func clear() {
        endWork?.cancel()
        endWork = nil
        plan = nil
        animating = false
    }

    private func ended(_ serial: Int) {
        guard let plan, plan.serial == serial else { return }
        clear()
    }

    /// Milliseconds into the current plan.
    public func ms(at t: CFTimeInterval? = nil) -> Int {
        guard plan != nil else { return 0 }
        return max(0, Int(((t ?? now()) - began) * 1000))
    }

    /// Is the kernel's current plan the one this player holds? A build this
    /// player never played makes the samples unreadable.
    private var current: Bool {
        guard let plan else { return false }
        return plan.serial == Tb.beatsSerial
    }

    /// The dice the plan rolls, as a mask (every SETTLE beat's).
    public var rolled: Int {
        plan?.beat.filter { $0.kind == TB_BK_SETTLE }.reduce(0) { $0 | $1.mask } ?? 0
    }

    public func frame(die i: Int, at t: CFTimeInterval) -> DieFrame? {
        frame(die: i, ms: ms(at: t))
    }

    /// Die `i` at `ms` into the plan, or nil when the plan does not roll it
    /// (a kept die never moves) or no plan is current.
    public func frame(die i: Int, ms: Int) -> DieFrame? {
        guard current, let plan, i >= 0, i < TrayModel.diceCount, rolled & (1 << i) != 0,
              let board = Tb.beatFrame(ms), i < board.dice.count else { return nil }
        var f = DieFrame(face: board.dice[i])
        for (k, b) in plan.beat.enumerated() where b.kind == TB_BK_SETTLE && b.mask & (1 << i) != 0 {
            guard let s = Tb.beatSample(k, part: i, ms: ms), s.apply != 0 else { continue }
            f.rotation = s.rot
            f.scale = CGFloat(s.scale)
            f.dy = CGFloat(s.dy)
        }
        return f
    }
}
