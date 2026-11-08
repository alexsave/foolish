// What the sound engine really plays, read off its own output.
//
// CuePlayer is rendered by hand here (AVAudioEngine's offline mode): the same
// graph, players and scheduling the board uses, with no session and no speaker,
// so a test reads the very samples a strike puts out and holds them to the take
// the kernel chose (c/src/cue_voice.h, fio_cue_strike). The rules themselves -
// which take, the tight fallback, the strokes, the speeds - are the kernel's
// and are held in c/tests/anim_plan_test.c; what is pinned here is the half
// only Swift can get wrong: that a take starts on the sample it was aimed at,
// plays whole, backwards when it should, at its level, and that the files the
// app bundles are the ones the table names.
import AVFoundation
import XCTest
import CFoolish
@testable import FoolishKit

final class CuePlayerTests: XCTestCase {

    private var player: CuePlayer!

    override func setUp() {
        super.setUp()
        player = CuePlayer()
    }

    override func tearDown() {
        player.shutdown()
        player = nil
        super.tearDown()
    }

    /// The master level the engine starts at, which every output sample carries.
    private var master: Float { CuePlayer.defaultVolume }

    /// The worst difference between `out` from `at` and `take` scaled by `gain`,
    /// and the loudest sample of `out` before `at` (it must be silence).
    private func compare(_ out: [Float], at: Int, _ take: [Float], gain: Float) -> (err: Float, before: Float) {
        var err: Float = 0
        for k in 0..<take.count where at + k < out.count { err = max(err, abs(out[at + k] - gain * take[k])) }
        let before = out[0..<max(0, min(at, out.count))].map(abs).max() ?? 0
        return (err, before)
    }

    private func peak(_ x: [Float]) -> Float { x.map(abs).max() ?? 0 }

    // ---- the files --------------------------------------------------------------

    /// Every take the kernel's table names is in the bundle, decodes from its
    /// first sample, and is exactly as long as the table says (Core Audio
    /// honours the edit list: docs/AUDIO.md "The priming, measured").
    func testEveryTakeTheTableNamesIsBundledAtItsLength() throws {
        XCTAssertGreaterThan(player.takes.count, 100, "the kernel links soundgen's table")
        try player.startOffline()
        for (i, t) in player.takes.enumerated() {
            let s = player.samples(i)
            XCTAssertEqual(s.count, t.nSamples, "\(t.name).m4a decodes to its take's length")
            XCTAssertGreaterThan(peak(s), 0.0005, "\(t.name).m4a is not silence")
        }
    }

    // ---- where a take lands -------------------------------------------------

    /// A lift struck a second ahead plays an ORDINARY take, starting its anchor
    /// before the moment, to the sample, whole, at the master level.
    func testATakeStartsItsAnchorBeforeItsMomentToTheSample() throws {
        try player.startOffline(rate: 48000)
        _ = try player.render(4800)
        let got = player.strike(Int(ANIM_CUE_LIFT), in: 1.0)
        XCTAssertEqual(got.count, 1)
        let s = try XCTUnwrap(got.first)
        let row = player.takes[s.take]
        XCTAssertEqual(row.cue, Int(ANIM_CUE_LIFT))
        XCTAssertEqual(row.rate, 48000, "a lift is a 48 kHz take, so the engine here does no conversion")
        // The moment is a second from the strike: the file starts its anchor early.
        let wantStart = 48000 - Int((Double(row.anchorUs) / 1e6 * 48000).rounded())
        XCTAssertEqual(Int(s.startSample - s.nowSample), wantStart, "started its anchor before the moment")
        XCTAssertEqual(s.momentAfter, 1.0, accuracy: 1.0 / 48000, "its anchor falls on the moment")
        let out = try player.render(48000 * 2)
        let (err, before) = compare(out, at: wantStart, player.samples(s.take), gain: master)
        XCTAssertLessThan(before, 1e-6, "nothing before the take's first sample")
        XCTAssertLessThan(err, 1e-4, "the output IS the take from its first sample, at the master level")
    }

    /// With no time before its moment a lift plays a TIGHT take, from now, whole.
    func testATakeWithNoTimeStartsNowWholeAndTight() throws {
        try player.startOffline(rate: 48000)
        _ = try player.render(1024)
        let s = try XCTUnwrap(player.strike(Int(ANIM_CUE_LIFT), in: 0).first)
        XCTAssertEqual(player.takes[s.take].role, Int(CUE_ROLE_TIGHT), "the tight take, pressed closer to its moment")
        XCTAssertEqual(s.startSample, s.nowSample, "from now")
        let take = player.samples(s.take)
        let out = try player.render(take.count + 4800)
        let (err, _) = compare(out, at: 0, take, gain: master)
        XCTAssertLessThan(err, 1e-4, "whole: its first sample is the first sample out")
    }

    /// A refused attack is PLACE backwards: its anchor measured from the other
    /// end, so the reversed landing still peaks on the moment.
    func testARevertIsTheUndoneTakeBackwards() throws {
        try player.startOffline(rate: 32000)
        _ = try player.render(1024)
        let s = try XCTUnwrap(player.strike(Int(ANIM_CUE_REVERT), in: 0.5, undoes: Int(ANIM_CUE_PLACE)).first)
        let row = player.takes[s.take]
        XCTAssertEqual(row.cue, Int(ANIM_CUE_PLACE))
        XCTAssertTrue(s.backwards)
        let back = Double(row.nSamples) - Double(row.anchorUs) / 1e6 * 32000
        let wantStart = Int((0.5 * 32000 - back).rounded())
        XCTAssertEqual(Double(s.startSample - s.nowSample), Double(wantStart), accuracy: 1)
        let out = try player.render(32000 * 2)
        let rev = player.samples(s.take, backwards: true)
        XCTAssertEqual(rev.first, player.samples(s.take).last, "reversed sample for sample")
        let (err, before) = compare(out, at: Int(s.startSample - s.nowSample), rev, gain: master)
        XCTAssertLessThan(before, 1e-6)
        XCTAssertLessThan(err, 1e-4, "the output is the take reversed")
    }

    /// A card dealt to the seat across a four-seat table lands at that seat's
    /// level (fio_seat_gain_pm, 455).
    func testADealtCardLandsAtItsSeatsLevel() throws {
        try player.startOffline(rate: 32000)
        _ = try player.render(1024)
        let gain = Int(fio_seat_gain_pm(0, 2, 4))
        let s = try XCTUnwrap(player.strike(Int(ANIM_CUE_DEAL_LAND), in: 0.2, gainPm: gain).first)
        XCTAssertEqual(s.gain, 0.455, accuracy: 1e-6)
        let out = try player.render(32000)
        let (err, _) = compare(out, at: Int(s.startSample - s.nowSample), player.samples(s.take), gain: master * 0.455)
        XCTAssertLessThan(err, 1e-4, "the take at the master level times the seat's")
    }

    /// A sweep of ten cards is gathered in four strokes first, started their own
    /// anchor before the same moment, over whatever else plays.
    func testASweepIsGatheredInStrokesFirst() throws {
        try player.startOffline(rate: 32000)
        _ = try player.render(1024)
        let got = player.strike(Int(ANIM_CUE_SWEEP), in: 5.0, nCards: 10)
        XCTAssertEqual(got.count, 2, "the lift and its strokes")
        let lift = got[0], strokes = got[1]
        XCTAssertEqual(player.takes[strokes.take].role, Int(CUE_ROLE_PRELUDE))
        XCTAssertEqual(player.takes[strokes.take].level, 4, "ten cards, one stroke per 2.5")
        XCTAssertEqual(strokes.momentAfter, 5.0, accuracy: 1.0 / 32000)
        XCTAssertEqual(lift.momentAfter, 5.0, accuracy: 1.0 / 32000)
        XCTAssertLessThan(strokes.startSample, lift.startSample)
        let out = try player.render(32000 * 7)
        // Until the lift starts the output is the strokes alone.
        let at = Int(strokes.startSample - strokes.nowSample)
        let alone = Array(player.samples(strokes.take).prefix(Int(lift.startSample - strokes.startSample)))
        let (err, before) = compare(out, at: at, alone, gain: master)
        XCTAssertLessThan(before, 1e-6)
        XCTAssertLessThan(err, 1e-4)
    }

    /// Two strikes a stagger apart overlap on two players: neither is cut or
    /// delayed by the other.
    func testOverlappingStrikesPlayTogether() throws {
        try player.startOffline(rate: 32000)
        _ = try player.render(1024)
        let a = try XCTUnwrap(player.strike(Int(ANIM_CUE_PLACE), in: 0.1).first)
        let b = try XCTUnwrap(player.strike(Int(ANIM_CUE_PLACE), in: 0.1 + Double(ANIM_SOUND_STAGGER_MS) / 1000).first)
        XCTAssertNotEqual(a.take, b.take, "a cue does not play the take it played last")
        XCTAssertEqual(b.momentAfter - a.momentAfter, Double(ANIM_SOUND_STAGGER_MS) / 1000, accuracy: 1.0 / 32000)
        let out = try player.render(32000)
        var sum = [Float](repeating: 0, count: out.count)
        for s in [a, b] {
            let x = player.samples(s.take), at = Int(s.startSample - s.nowSample)
            for k in 0..<x.count where at + k < sum.count { sum[at + k] += master * x[k] }
        }
        let err = zip(out, sum).map { abs($0 - $1) }.max() ?? 1
        XCTAssertLessThan(err, 1e-4, "the output is the two takes summed")
    }

    // ---- when it is silent --------------------------------------------------

    /// Muted, a strike schedules nothing and nothing is decoded.
    func testMutedSchedulesAndLoadsNothing() throws {
        try player.startOffline(rate: 32000)
        player.setMuted(true)
        XCTAssertTrue(player.strike(Int(ANIM_CUE_PLACE), in: 0.1).isEmpty)
        XCTAssertEqual(player.loadedBytes.files, 0, "a muted player is sent nothing")
        XCTAssertFalse(player.running, "and the engine is stopped")
    }

    /// The board going away stops the engine and gives every decoded take back.
    func testDetachDropsTheEngineAndTheBuffers() throws {
        try player.startOffline(rate: 32000)
        _ = try player.render(1024)
        XCTAssertFalse(player.strike(Int(ANIM_CUE_COVER), in: 0.1).isEmpty)
        XCTAssertGreaterThan(player.loadedBytes.files, 0)
        player.detach()
        XCTAssertFalse(player.running)
        XCTAssertEqual(player.loadedBytes.files, 0)
        XCTAssertTrue(player.strike(Int(ANIM_CUE_COVER), in: 0.1).isEmpty, "no board, no sound")
    }

    /// What every sound costs in memory once decoded, measured on the process
    /// (phys_footprint) and counted in samples: the number the extension has to
    /// fit beside its textures (docs/AUDIO.md "iOS and iMessage").
    func testWhatEverySoundCostsInMemory() throws {
        let before = FlightRecorder.footprintMB()
        try player.startOffline(rate: 48000)
        player.want(UInt32.max)
        player.waitLoaded()
        let loaded = player.loadedBytes
        let after = FlightRecorder.footprintMB()
        print(String(format: "CUE MEMORY: %d takes, %.2f MB of samples, footprint %.2f -> %.2f MB (+%.2f)",
                     loaded.files, Double(loaded.bytes) / 1_048_576, before, after, after - before))
        XCTAssertEqual(loaded.files, player.takes.count, "the loader decodes every take")
        XCTAssertLessThan(Double(loaded.bytes) / 1_048_576, 16, "every sound decoded stays under 16 MB of samples")
    }
}
