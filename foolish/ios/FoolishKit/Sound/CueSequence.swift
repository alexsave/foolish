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
// `startMs`, and every cue of the beat is placed from it.
//
// A CUE WHOSE TAKE STARTS BEFORE ITS BEAT OPENS (the grab and the thumb slide
// before the next card leaves, the strokes before a sweep that start under the
// last cover and its hold) is placed from the beat before, against where that
// beat says the next one will open. The walk is not the plan, measured: a beat
// with nothing to fly takes no time where the plan gives it a flight's, and
// every flown beat runs ~30 ms over its plan (the builder, the ledger writes,
// the task hops). So the forecast is the walk's own: the next beat opens now
// when this one flies nothing, else this beat's plan length later plus the
// overrun the walk has shown so far in this stream. And when a beat opens
// anyway at another time than forecast, every take placed for it that has not
// begun to sound is taken back out and placed again from the real opening; a
// take already sounding stays where it is, which is the one error between a
// sound and its picture left here, and what the trace says at each beat.
//
// EVERY STRIKE IS HANDED OVER ONCE (a cue's (step, kind) is unique, and each
// of its hits is its own strike), unless it is taken back to be placed again.
// A dealt card's landing is struck at its seat's level (fio_seat_gain_pm, the
// kernel's geometry, with the viewer's seat).

import Foundation

final class CueSequence {
    private let plan: AnimPlan
    private let viewer: Int
    private let seats: Int
    private let player: CuePlayer
    /// Strikes handed over, by (step, kind, hit).
    private var done = Set<Int>()
    /// Strikes placed before their own beat opened, with what was scheduled,
    /// so the beat can take them back.
    private var ahead: [Int: (step: Int, got: [CuePlayer.Scheduled])] = [:]
    /// Where the beat starting at each step was forecast to open (host time).
    private var forecast: [Int: UInt64] = [:]
    /// How far the walk has run over its plan per flown beat, in seconds.
    private var overrun = 0.0
    /// When the first beat opened, and its plan time: the trace's reference.
    private var first: (host: UInt64, ms: Int)?

    init(_ plan: AnimPlan, viewer: Int, seats: Int, player: CuePlayer = .shared) {
        self.plan = plan
        self.viewer = viewer
        self.seats = seats
        self.player = player
    }

    private static func hitKey(_ c: AnimPlan.Cue, _ h: Int) -> Int { (c.step * 256 + c.kind) * 8 + h }

    /// The beat of steps `beat` opens: its flight starts `paint` seconds from
    /// now (BoardAnimator.play paints first, for the plan's gap); `empty` when
    /// it flies nothing, which the walk then passes through at once.
    func opens(_ beat: Range<Int>, paint: Double, empty: Bool = false) {
        guard beat.lowerBound < plan.steps.count, !plan.cues.isEmpty else { return }
        let startMs = plan.steps[beat.lowerBound].startMs
        let nextMs = beat.upperBound < plan.steps.count ? plan.steps[beat.upperBound].startMs : Int.max
        let now = mach_absolute_time()
        let opensAt = now + UInt64(paint / Self.hostTick)
        let ms = { (a: UInt64, b: UInt64) in (Double(a) - Double(b)) * Self.hostTick * 1000 }

        // How the walk did against the forecast for this beat: learn its overrun
        // (flown beats only), and the trace.
        if let f = forecast[beat.lowerBound] {
            let off = ms(opensAt, f) / 1000
            if !empty { overrun = max(-0.1, min(0.2, overrun + 0.5 * off)) }
            if AnimLog.on, let b = first {
                AnimLog.say(String(format: "sound: beat %d opens at %+.1f ms (plan %+d), %+.1f ms off its forecast%@",
                                   beat.lowerBound, ms(opensAt, b.host), startMs - b.ms, off * 1000, empty ? ", flies nothing" : ""))
            }
        }
        if first == nil { first = (opensAt, startMs) }

        // Takes placed ahead for this beat or a later one that have not begun:
        // back out, to be placed again below from where the beat really is.
        var replaced = 0
        for (k, a) in ahead where a.step >= beat.lowerBound {
            if a.got.allSatisfy({ player.unschedule($0) }) { done.remove(k); ahead[k] = nil; replaced += 1 }
        }
        if replaced > 0 { AnimLog.say("sound: beat \(beat.lowerBound) places \(replaced) strikes again") }

        // Where the next beat will open, as this walk goes.
        var nextAt: UInt64?
        if nextMs != Int.max {
            // Flying nothing, the walk goes straight on, and the next flight
            // opens after its own paint; else a plan's length after this one.
            let gap = empty ? flightGap : boardSeconds(nextMs - startMs) + overrun
            nextAt = (empty ? now : opensAt) + UInt64(max(0, gap) / Self.hostTick)
            forecast[beat.upperBound] = nextAt
        }

        for c in plan.cues where c.step >= beat.lowerBound {
            let own = c.step < beat.upperBound
            if !own, nextMs == Int.max || Double(c.atMs) - player.lead(c.kind) * 1000 >= Double(nextMs) { continue }
            var gain = 1000
            if c.kind == Int(ANIM_CUE_DEAL_LAND), c.step < plan.steps.count {
                gain = CueKernel.seatGainPm(viewer: viewer, seat: plan.steps[c.step].seat, seats: seats)
            }
            for h in 0..<max(1, c.hits) where !done.contains(Self.hitKey(c, h)) {
                let at = c.atMs + h * Int(ANIM_SOUND_STAGGER_MS)
                // Its own beat's cue from this opening; a later beat's from where
                // that beat is forecast to open.
                let origin = own ? opensAt : (nextAt ?? opensAt)
                let base = own ? startMs : nextMs
                let seconds = max(0, ms(origin, now) / 1000 + boardSeconds(at - base))
                let planned = first.map { boardMs(at - $0.ms) } ?? 0
                let aimed = first.map { ms(now, $0.host) + seconds * 1000 } ?? 0
                let firstHost = first?.host ?? now
                let kind = c.kind, step = c.step
                let got = player.strike(c.kind, in: seconds, nCards: c.nCards, undoes: c.undoes, gainPm: gain) { s in
                    guard AnimLog.on else { return }
                    let moment = ms(s.struck, firstHost) + s.momentAfter * 1000
                    AnimLog.say(String(format: "sound: cue %d step %d hit %d at_ms %d%@: %@%@ moment %+.1f ms from beat 0, "
                                       + "the plan %+.1f (off %+.1f; late %+.1f), start frame %lld, now %lld",
                                       kind, step, h, at, own ? "" : " (ahead)", s.name, s.backwards ? " backwards" : "",
                                       moment, planned, moment - planned, moment - aimed, s.startSample, s.nowSample))
                }
                done.insert(Self.hitKey(c, h))
                if !own, !got.isEmpty { ahead[Self.hitKey(c, h)] = (c.step, got) }
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
