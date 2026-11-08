// CueSequence.swift - the sounds of one animated stream, handed to the engine
// beat by beat.
//
// The kernel's plan lists what a stream sounds like and when (AnimPlan.cues,
// on the same clock as every step's `startMs`). The web walks that clock with a
// frame loop and hands each cue over a little before its file has to start
// (src/audio/cueSchedule.ts). This board walks BEATS instead: it awaits each
// beat's flight and then the next (MessageTableView+Sequence.swift), and the
// time a beat really opens is whatever that walk made it. So the clock here is
// re-read at every beat: the moment a beat's flight opens is that beat's
// `startMs`, and every cue handed over then is placed from it.
//
// WHICH CUES A BEAT HANDS OVER: its own, and every later one whose earliest
// take must start before the next beat opens - the grab and the thumb slide
// before the next card leaves, the strokes before a sweep that start under the
// last cover and its hold. A cue placed from an earlier beat stays where it
// was put; how far the walk then drifts from the plan is what the trace says
// at each beat (`opens`), and it is the one error between a sound and its
// picture this file can have.
//
// EVERY CUE IS HANDED OVER ONCE, by its (step, kind), which the kernel keeps
// unique. A dealt card's landing is struck at its seat's level
// (fio_seat_gain_pm, the kernel's geometry, with the viewer's seat).

import CFoolish
import Foundation

final class CueSequence {
    private let plan: AnimPlan
    private let viewer: Int
    private let seats: Int
    private let player: CuePlayer
    private var done = Set<Int>()
    /// When the first beat opened, and its plan time: the trace's reference.
    private var first: (host: UInt64, ms: Int)?

    init(_ plan: AnimPlan, viewer: Int, seats: Int, player: CuePlayer = .shared) {
        self.plan = plan
        self.viewer = viewer
        self.seats = seats
        self.player = player
    }

    /// The beat of steps `beat` opens: its flight starts `paint` seconds from
    /// now (BoardAnimator.play paints first, for the plan's gap).
    func opens(_ beat: Range<Int>, paint: Double) {
        guard beat.lowerBound < plan.steps.count, !plan.cues.isEmpty else { return }
        let startMs = plan.steps[beat.lowerBound].startMs
        let nextMs = beat.upperBound < plan.steps.count ? plan.steps[beat.upperBound].startMs : Int.max
        let now = mach_absolute_time()
        let opensAt = now + UInt64(paint / Self.hostTick)
        if let f = first {
            let walked = (Double(opensAt) - Double(f.host)) * Self.hostTick * 1000
            AnimLog.say(String(format: "sound: beat %d opens at %+.1f ms, the plan says %+d (drift %+.1f ms)",
                               beat.lowerBound, walked, startMs - f.ms, walked - boardMs(startMs - f.ms)))
        } else {
            first = (opensAt, startMs)
        }
        for c in plan.cues where c.step >= beat.lowerBound && !done.contains(c.step * 256 + c.kind) {
            let own = c.step < beat.upperBound
            if !own, nextMs != Int.max, Double(c.atMs) - player.lead(c.kind) * 1000 >= Double(nextMs) { continue }
            done.insert(c.step * 256 + c.kind)
            var gain = 1000
            if c.kind == Int(ANIM_CUE_DEAL_LAND), c.step < plan.steps.count {
                gain = Int(fio_seat_gain_pm(Int32(viewer), Int32(plan.steps[c.step].seat), Int32(seats)))
            }
            for h in 0..<max(1, c.hits) {
                let at = c.atMs + h * Int(ANIM_SOUND_STAGGER_MS)
                let seconds = paint + boardSeconds(at - startMs)
                let got = player.strike(c.kind, in: seconds, nCards: c.nCards, undoes: c.undoes, gainPm: gain)
                guard AnimLog.on, let f = first else { continue }
                // Where the take's moment falls, from the first beat's opening,
                // against where the plan puts it from there: the walk's drift
                // up to the beat that placed it, and any lateness of a take
                // that had no time to start early.
                let nowMs = (Double(now) - Double(f.host)) * Self.hostTick * 1000
                for s in got {
                    let moment = nowMs + s.momentAfter * 1000
                    AnimLog.say(String(format: "sound: cue %d step %d hit %d at_ms %d: %@%@ moment %+.1f ms from beat 0, "
                                       + "the plan %+.1f (off %+.1f; late %+.1f), start sample %lld at %.0f Hz, now %lld",
                                       c.kind, c.step, h, at, s.name, s.backwards ? " backwards" : "",
                                       moment, boardMs(at - f.ms), moment - boardMs(at - f.ms),
                                       (s.momentAfter - seconds) * 1000, s.startSample, s.rate, s.nowSample))
                }
                if got.isEmpty { AnimLog.say("sound: cue \(c.kind) step \(c.step) hit \(h) silent") }
            }
        }
    }

    /// Plan milliseconds as this board walks them (HARNESS_SLOWMO stretches both).
    private func boardMs(_ ms: Int) -> Double { boardSeconds(ms) * 1000 }

    private static let hostTick: Double = {
        var info = mach_timebase_info_data_t()
        mach_timebase_info(&info)
        return Double(info.numer) / Double(info.denom) / 1e9
    }()
}
