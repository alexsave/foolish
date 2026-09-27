// TumblePlayer.swift - the dice roll: a short tumble for a subset of dice.
//
// THE SEAM FOR THE KERNEL'S TIMELINE (T9, pickemup's BeatPlayer). The tray
// never animates itself: every frame it asks a `DiceMotion` what each die
// looks like at this millisecond, and draws that. Today the only source is
// this Swift tumble; KERNEL: once tb_beats.h lays out the dice-settle beat, a
// BeatPlayer (copied from pickemup/ios/PickemupKit/Board/BeatPlayer.swift)
// conforms to DiceMotion by sampling tb_api_beat_sample for the die's
// anchor ("die.i"), and the tray and TallyTable change by one line each.
//
// The tumble (T13): 600ms per die, dice starting 30ms apart in slot order.
// The die turns 1.5 revolutions (alternate dice the other way) on an
// ease-out, swells to 1.18 at 30% and settles back, and its face flicks
// through pip values every 80ms until 70% of the way, then shows the value
// it landed on. The flicked faces are decoration only: they are a fixed
// function of the die and the time, and never a hint at anything.

import QuartzCore
import SwiftUI

/// One die as drawn at one moment.
public struct DieFrame: Equatable, Sendable {
    public var rotation: Double = 0     // degrees about Z
    public var scale: CGFloat = 1
    /// The face to draw instead of the die's own value, while it tumbles.
    public var face: Int?
    public init(rotation: Double = 0, scale: CGFloat = 1, face: Int? = nil) {
        self.rotation = rotation
        self.scale = scale
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
public final class TumblePlayer: ObservableObject, DiceMotion {
    public static let durationMs = 600
    public static let staggerMs = 30
    public static let flickMs = 80
    public static let revolutions = 1.5
    public static let swell: CGFloat = 0.18

    @Published public private(set) var animating = false
    @Published public private(set) var dice: Set<Int> = []
    private var began: CFTimeInterval = 0
    private var endWork: DispatchWorkItem?
    /// The clock, replaceable by a test.
    public var now: () -> CFTimeInterval = { CACurrentMediaTime() }

    public init() {}

    /// Tumble these dice from now. A new tumble replaces one in flight (the
    /// clear-not-revert rule: the old one is dropped, never unwound).
    public func tumble(_ which: Set<Int>) {
        endWork?.cancel()
        guard !which.isEmpty else { clear(); return }
        dice = which
        began = now()
        animating = true
        let total = Self.durationMs + Self.staggerMs * (TrayModel.diceCount - 1)
        let work = DispatchWorkItem { [weak self] in self?.clear() }
        endWork = work
        DispatchQueue.main.asyncAfter(deadline: .now() + Double(total) / 1000, execute: work)
    }

    public func clear() {
        endWork?.cancel()
        endWork = nil
        dice = []
        animating = false
    }

    public func frame(die i: Int, at t: CFTimeInterval) -> DieFrame? {
        guard animating, dice.contains(i) else { return nil }
        return Self.sample(die: i, ms: Int((t - began) * 1000))
    }

    /// The tumble of die `i` at `ms` after the tumble began: pure, so a test
    /// (and the kernel's timeline, later) can pin it.
    public nonisolated static func sample(die i: Int, ms: Int) -> DieFrame {
        let local = ms - staggerMs * i
        guard local > 0 else { return DieFrame(face: flick(die: i, step: 0)) }
        guard local < durationMs else { return .rest }
        let p = Double(local) / Double(durationMs)
        let eased = 1 - pow(1 - p, 3)
        let sign: Double = i % 2 == 0 ? 1 : -1
        let rotation = sign * 360 * revolutions * eased
        // up to the swell by 30%, back to 1 by the end
        let s = p < 0.3 ? p / 0.3 : (1 - p) / 0.7
        let face = p < 0.7 ? flick(die: i, step: local / flickMs) : nil
        return DieFrame(rotation: rotation, scale: 1 + swell * CGFloat(s), face: face)
    }

    /// A fixed, meaningless sequence of faces for the flicker.
    nonisolated static func flick(die i: Int, step: Int) -> Int { (i * 5 + step * 4 + 2) % 6 + 1 }
}
