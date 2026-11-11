// CueKernel.swift - the kernel's sound calls, for the app layers.
//
// The app layers (FoolishKit, FoolishApp, FoolishMessages) may not import the C bridge: CFoolish
// stays inside sdk/swift/ (ios/scripts/lint_architecture.sh, §7.1). The sound player and the boards
// that strike role and finish cues need these calls, so they are here and only here, as the
// kernel's own answers with no rule of their own: which take a strike plays, the one mix every
// take is summed in, the resampler, which cues a board could sound next, what a turn of the marks
// or the end of a game sounds like.

import Foundation
import CFoolish

public enum CueKernel {
    // ---- the table and the voice (c/src/cue_voice.h) ------------------------------------------

    /// How loud a card landing at `seat` is to the viewer at `viewer`, in thousandths (1000: its own seat).
    public static func seatGainPm(viewer: Int, seat: Int, seats: Int) -> Int {
        Int(fio_seat_gain_pm(Int32(viewer), Int32(seat), Int32(seats)))
    }

    /// Every take of the table soundgen wrote for this build.
    public static func takes() -> [CueTakeRowSnap] {
        var rows: [CueTakeRowSnap] = []
        for i in 0..<Int(fio_cue_n_takes()) {
            if let p = fio_cue_take(Int32(i)), let row = try? readCueTakeRow(p) { rows.append(row) }
        }
        return rows
    }

    /// How long before its moment each cue's earliest take starts, in microseconds, by cue id.
    public static func leadUs() -> [Int] {
        (0..<Int(ANIM_N_CUES)).map { Int(fio_cue_lead_us(Int32($0))) }
    }

    /// Seed the voice's generator (which take, how far a stand-in wanders).
    public static func seed(_ s: UInt32) { fio_cue_seed(s) }

    /// What a strike plays, or nil when it has nothing to play.
    public static func strike(_ kind: Int, leadUs: Int, nCards: Int, undoes: Int, gainPm: Int) -> CueStrikeSnap? {
        guard let p = fio_cue_strike(Int32(kind), Int32(clamping: leadUs), Int32(clamping: nCards),
                                     Int32(undoes), Int32(gainPm)),
              let s = try? readCueStrike(p), s.take >= 0 else { return nil }
        return s
    }

    /// One bit per cue id: the cues a board could sound next (a hint for what to keep decoded).
    public static func cuesNext(status: Int, mySeat: Int, players: Int, battles: Int, covered: Int,
                                hasFlipped: Bool, deck: Int) -> UInt32 {
        UInt32(fio_cues_next(Int32(status), Int32(mySeat), Int32(players), Int32(battles), Int32(covered),
                             hasFlipped ? 1 : 0, Int32(deck)))
    }

    /// What a turn of the role marks sounds like (ANIM_CUE_NONE for none).
    public static func roleCue(shownDefender: Int, shownFirstAttacker: Int, shownGoodMask: Int,
                               defender: Int, firstAttacker: Int, goodMask: Int) -> Int {
        Int(fio_role_cue(Int32(shownDefender), Int32(shownFirstAttacker), Int32(shownGoodMask),
                         Int32(defender), Int32(firstAttacker), Int32(goodMask)))
    }

    /// What the end of the game sounds like to this viewer (ANIM_CUE_NONE for none).
    public static func finishCue(elimination: [UInt8], gameOver: Int, players: Int, mySeat: Int) -> Int {
        elimination.withUnsafeBufferPointer { p in
            Int(fio_finish_cue(p.baseAddress, Int32(elimination.count), Int32(gameOver),
                               Int32(players), Int32(mySeat)))
        }
    }

    // ---- the resampler ------------------------------------------------------------------------

    public static func resampledLength(_ n: Int, ratePm: Int) -> Int {
        Int(fio_cue_resampled_len(Int32(n), Int32(ratePm)))
    }

    /// `src` (n samples) at `ratePm` into `dst` (capacity `cap`); how many samples were written.
    public static func resample(_ src: UnsafePointer<Float>, n: Int, ratePm: Int,
                                into dst: UnsafeMutablePointer<Float>, cap: Int) -> Int {
        Int(fio_cue_resample(src, Int32(n), Int32(ratePm), dst, Int32(cap)))
    }

    // ---- the one mix every take is summed in (c/src/cue_mix.h) --------------------------------

    /// The kernel's mix at the output's rate. Free it by letting go of it, once the engine has
    /// stopped calling `render`.
    public final class Mixer: @unchecked Sendable {
        private let raw: UnsafeMutableRawPointer

        public init?(rate: Double) {
            guard let m = fio_mix_new(rate) else { return nil }
            raw = m
        }
        deinit { fio_mix_free(raw) }

        /// Sum the next `frames` into `out` (the audio callback's; `host` the block's host time, or 0).
        public func render(_ out: UnsafeMutablePointer<Float>, frames: Int, host: UInt64) {
            fio_mix_render(raw, out, Int32(frames), host)
        }
        /// The next frame to be rendered.
        public var now: Int64 { fio_mix_now(raw) }
        /// The output frame heard at host time `at`.
        public func frame(at: UInt64, ticksPerSecond: Double) -> Int64 { fio_mix_frame_at(raw, at, ticksPerSecond) }
        /// One bit per slot still sounding.
        public var live: UInt32 { fio_mix_live(raw) }
        /// Place `frames` samples at `start`; the slot, or a negative number when the mix is full.
        public func add(_ x: UnsafePointer<Float>, frames: Int, start: Int64, gain: Float) -> Int32 {
            fio_mix_add(raw, x, Int32(frames), start, gain)
        }
        public func drop(slot: Int32) { fio_mix_drop(raw, slot) }
        public func clear() { fio_mix_clear(raw) }
    }
}
