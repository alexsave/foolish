// CuePlayer.swift - the iOS sound engine: a cue id in, a sound out, on time.
//
// WHAT THIS OWNS: the AVAudioEngine and when it runs, the decoded files, the
// player nodes, the master level and the mute, and turning "the kernel's
// answer for this strike" into a buffer scheduled at an exact sample on the
// engine's clock.
//
// WHAT IT DOES NOT, and must not learn: which cue a beat makes or when (the
// kernel's plan, c/src/anim_plan.h "sound cues"), and which take a cue plays,
// when its file starts, how loud and how fast (c/src/cue_voice.h, asked through
// fio_cue_strike over soundgen's generated table). There is no take table, no
// anchor, no gain and no event type in this file. It is the web's
// src/audio/cueAudio.ts with its rules moved into C, so the two hosts cannot
// disagree; docs/AUDIO.md is the guide.
//
// THE CLOCK. A strike is scheduled, never timed with a sleep: every take is
// `scheduleBuffer(_:at:)` at a sample time of its player's own timeline, read
// off the node's last render and carried forward to this instant by the host
// clock the render was stamped with. A take the kernel says starts "now" is
// started two render quanta ahead (`nowSample`), which keeps it whole: a sample
// time already passing would be played late or clipped, and a take is never
// cut (Alex: "play them in full").
//
// THE FILES are soundgen's (ios/scripts/bundle_sounds.sh copies them into this
// framework as sounds/<name>.m4a), decoded once into AVAudioPCMBuffers at their
// own rate. Core Audio honours the files' edit lists, so a decoded take starts
// on the WAV's first sample (docs/AUDIO.md "The priming, measured") and is as
// long as the table says, which `decode` checks. A reversed copy is made the
// first time a revert needs one, and kept.
//
// ONE PLAYER NODE PER VOICE. A player plays its scheduled buffers one after
// another, so two takes that overlap need two players; each rate the table
// has gets a small pool, made when the engine starts. A strike takes a player
// that has nothing playing or pending, so setting its level cannot touch
// another take's tail.
//
// THE ORDER THE FILES LOAD IN is the kernel's: `want` is anim_cues_next for
// the board on screen (fio_cues_next), and those cues' files are decoded first,
// in the table's order, then the rest. A struck take that has not loaded yet is
// decoded on the spot (a few milliseconds from the bundle).
//
// THE LIFECYCLE is the iMessage extension's: its memory is tight and it is torn
// down between bubbles. Nothing is created until a board appears (`attach`);
// when the last board goes away (`detach`), the extension resigns or memory runs
// short, the engine stops and every buffer is dropped (`shutdown`), and the next
// board loads them again.
//
// THE SESSION is `.ambient`: the ringer switch silences the game, and it mixes
// with whatever the player is listening to instead of stopping it.
//
// WHEN IT IS SILENT, each a deliberate no-op: while muted (nothing is loaded
// either); with the feature flag off (`CuePlayer.on`); before a board has
// appeared; when the engine cannot start; when a file is missing; when every
// player of a rate is busy.

import AVFoundation
import CFoolish
import Foundation

// Public for the two calls the extension's view controller makes (`shared`,
// `shutdown`): FoolishKit is a dynamic framework, and every public symbol is an
// export the linker keeps (project.yml, the Foolish target's note).
public final class CuePlayer {
    public static let shared = CuePlayer()

    /// THE SHIPPING DEFAULT (owner rule: every new change behind a flag). In a
    /// debug build `sound=0` in `dev.flags` turns the whole engine off, and
    /// `sound.silent=1` (or HARNESS_SOUND_SILENT in the harness) keeps it
    /// running and scheduling with its output at zero - what a simulator run
    /// measures with, so nothing reaches the Mac's speakers.
    static let onByDefault = true
    static var on: Bool {
        #if DEBUG || SOLO_TESTING
        return MessageDevBoard.flag("sound", shipping: onByDefault)
        #else
        return onByDefault
        #endif
    }
    static var silent: Bool {
        #if DEBUG || SOLO_TESTING
        // HARNESS_SOUND_SILENT for the harness, which has no App Group to read
        // `dev.flags` from.
        if ProcessInfo.processInfo.environment["HARNESS_SOUND_SILENT"] != nil { return true }
        return MessageDevBoard.flag("sound.silent", shipping: false)
        #else
        return false
        #endif
    }

    /// The web's default master level (src/audio/cueAudio.ts DEFAULT_VOLUME).
    static let defaultVolume: Float = 0.7

    /// soundgen's table as this build's kernel links it: one row a file.
    let takes: [CueTakeRowSnap]
    /// How long before its moment each cue's earliest take starts, by ANIM_CUE_*.
    private let leadUs: [Int]

    private(set) var muted = false
    private(set) var volume: Float = CuePlayer.defaultVolume

    private let bundle: Bundle
    private var engine: AVAudioEngine?
    private var mix: AVAudioMixerNode?
    private var voices: [Voice] = []
    private var users = 0
    private var configObserver: NSObjectProtocol?
    /// True while the engine renders by hand (the tests), where no host time runs.
    private var offline = false

    /// Decoded files by take index, and their reversed copies. Guarded by
    /// `lock`: the loader decodes off the main thread.
    private let lock = NSLock()
    private var buffers: [Int: AVAudioPCMBuffer] = [:]
    private var reversed: [Int: AVAudioPCMBuffer] = [:]
    private var missing: Set<Int> = []
    private var wanted: UInt32 = 0
    /// Bumped by `shutdown`, so a load in flight for an engine that is gone
    /// drops what it decoded.
    private var generation = 0
    private var loading = false
    private let loader = DispatchQueue(label: "cards.foolish.cue.load", qos: .utility)

    final class Voice {
        let node = AVAudioPlayerNode()
        let rate: Double
        /// The player-time sample its last scheduled take ends at.
        var freeAt: AVAudioFramePosition = 0
        init(rate: Double) { self.rate = rate }
    }

    /// One take on its way, for the trace and the tests: where it starts and
    /// where its anchor falls, in its player's samples.
    struct Scheduled: Equatable {
        let take: Int
        let name: String
        let rate: Double
        let backwards: Bool
        /// Player-time samples: the start, and "now" when it was scheduled.
        let startSample: AVAudioFramePosition
        let nowSample: AVAudioFramePosition
        /// Where the cue's moment falls, in seconds from the strike.
        let momentAfter: Double
        let gain: Float
    }

    init(bundle: Bundle = Bundle(for: CuePlayer.self)) {
        self.bundle = bundle
        var rows: [CueTakeRowSnap] = []
        for i in 0..<Int(fio_cue_n_takes()) {
            if let p = fio_cue_take(Int32(i)), let row = try? readCueTakeRow(p) { rows.append(row) }
        }
        takes = rows
        leadUs = (0..<Int(ANIM_N_CUES)).map { Int(fio_cue_lead_us(Int32($0))) }
        fio_cue_seed(UInt32.random(in: 1...UInt32.max))
    }

    /// How long before its moment a cue must be handed over for its earliest
    /// take to start on time, in seconds.
    func lead(_ kind: Int) -> Double {
        kind >= 0 && kind < leadUs.count ? Double(leadUs[kind]) / 1e6 : 0
    }

    // ---- the setting -------------------------------------------------------------

    /// Mute or unmute. Muting stops what is sounding and scheduled and loads
    /// nothing more; unmuting starts the engine again if a board is up.
    func setMuted(_ m: Bool) {
        guard m != muted else { return }
        muted = m
        if m { shutdown(keepUsers: true) } else if users > 0 { start() }
    }

    /// The master level, 0...1.
    func setVolume(_ v: Float) {
        volume = min(1, max(0, v))
        mix?.outputVolume = volume
    }

    // ---- the lifecycle -------------------------------------------------------------

    /// A board appeared: start the engine and begin loading.
    func attach() {
        users += 1
        if users == 1 { start() }
    }

    /// A board went away. The last one takes the engine and the buffers with it.
    func detach() {
        users = max(0, users - 1)
        if users == 0 { shutdown() }
    }

    /// The extension resigned, or memory is short: everything goes, and comes
    /// back with the next board.
    public func shutdown(keepUsers: Bool = false) {
        if !keepUsers { users = 0 }
        if let o = configObserver { NotificationCenter.default.removeObserver(o) }
        configObserver = nil
        engine?.stop()
        engine = nil
        mix = nil
        voices = []
        offline = false
        lock.lock()
        buffers = [:]; reversed = [:]; missing = []
        generation += 1
        lock.unlock()
        try? AVAudioSession.sharedInstance().setActive(false, options: .notifyOthersOnDeactivation)
    }

    /// Memory is short: drop every decoded take but keep the engine. A struck
    /// take is decoded again on the spot, and the next `want` reloads the rest.
    public func purge() {
        lock.lock()
        let bytes = buffers.values.reduce(0) { $0 + Int($1.frameLength) * 4 }
        buffers = [:]; reversed = [:]
        generation += 1
        lock.unlock()
        AnimLog.say("sound: purged \(bytes / 1024) KB of decoded takes")
    }

    /// Whether the engine is up, for the trace and the tests.
    var running: Bool { engine.map { offline || $0.isRunning } ?? false }

    private func start() {
        guard Self.on, !muted, engine == nil else { return }
        let session = AVAudioSession.sharedInstance()
        do {
            try session.setCategory(.ambient, mode: .default)
            try session.setActive(true)
        } catch {
            AnimLog.say("sound: no audio session (\(error))")
        }
        guard build(AVAudioEngine()) else { return }
        do {
            try engine?.start()
        } catch {
            AnimLog.say("sound: the engine did not start (\(error))")
            shutdown(keepUsers: true)
            return
        }
        for v in voices { v.node.play() }
        // A route change (headphones, a call) stops the engine; build it again.
        configObserver = NotificationCenter.default.addObserver(
            forName: .AVAudioEngineConfigurationChange, object: engine, queue: .main) { [weak self] _ in
                guard let self, self.engine != nil else { return }
                AnimLog.say("sound: the engine's configuration changed - restarting it")
                let keep = self.users
                self.shutdown(keepUsers: true)
                self.users = keep
                if keep > 0 { self.start() }
            }
        AnimLog.say("sound: engine up, \(voices.count) players, session \(session.category.rawValue), "
                    + "io \(Int(session.ioBufferDuration * 1000)) ms, out \(Int(session.sampleRate)) Hz, silent=\(Self.silent)")
        pump()
    }

    /// The graph: every player into one mixer (the master level), into the
    /// engine's. A pool per rate the table has, sized for what a plan strikes
    /// together (six cards of a deal off the stock and six landing, a sweep's
    /// strokes over the last covers).
    private func build(_ e: AVAudioEngine) -> Bool {
        let m = AVAudioMixerNode()
        e.attach(m)
        e.connect(m, to: e.mainMixerNode, format: nil)
        m.outputVolume = volume
        e.mainMixerNode.outputVolume = Self.silent ? 0 : 1
        var made: [Voice] = []
        for rate in Set(takes.map(\.rate)).sorted() {
            guard let format = AVAudioFormat(standardFormatWithSampleRate: Double(rate), channels: 1) else { return false }
            let n = takes.filter { $0.rate == rate }.count >= 40 ? 12 : 8
            for _ in 0..<n {
                let v = Voice(rate: Double(rate))
                e.attach(v.node)
                e.connect(v.node, to: m, format: format)
                made.append(v)
            }
        }
        engine = e; mix = m; voices = made
        return true
    }

    // ---- loading -------------------------------------------------------------------

    /// The cues that could sound next from the board on screen, one bit per
    /// ANIM_CUE_* (fio_cues_next): their files load first.
    func want(_ mask: UInt32) {
        lock.lock(); let changed = mask != wanted || buffers.isEmpty; wanted = mask; lock.unlock()
        if changed { pump() }
    }

    /// The kernel's answer for a board, asked with its seven scalars.
    func want(_ v: GameView, mySeat: Int) {
        let covered = v.battles.filter { $0.defense != nil }.count
        want(UInt32(fio_cues_next(Int32(v.status), Int32(mySeat), Int32(v.players.count),
                                  Int32(v.battles.count), Int32(covered), v.hasFlipped ? 1 : 0,
                                  Int32(v.deckCount))))
    }

    /// The next take to decode: a wanted cue's in the table's order, then any.
    private func nextToLoad() -> Int? {
        var later: Int?
        for (i, t) in takes.enumerated() where buffers[i] == nil && !missing.contains(i) {
            if wanted & (1 << UInt32(t.cue)) != 0 { return i }
            if later == nil { later = i }
        }
        return later
    }

    /// Decode everything, wanted first, one file at a time off the main thread.
    private func pump() {
        guard engine != nil, !muted else { return }
        lock.lock()
        if loading { lock.unlock(); return }
        loading = true
        let gen = generation
        lock.unlock()
        loader.async { [weak self] in
            while let self {
                self.lock.lock()
                guard gen == self.generation, let i = self.nextToLoad() else {
                    self.loading = false
                    self.lock.unlock()
                    return
                }
                self.lock.unlock()
                let b = self.decode(i)
                self.lock.lock()
                if gen == self.generation {
                    if let b { self.buffers[i] = b } else { self.missing.insert(i) }
                }
                self.lock.unlock()
            }
        }
    }

    /// One file, decoded at its own rate, or nil when it is missing or not the
    /// length the table says.
    private func decode(_ i: Int) -> AVAudioPCMBuffer? {
        let t = takes[i]
        guard let url = bundle.url(forResource: t.name, withExtension: "m4a", subdirectory: "sounds"),
              let file = try? AVAudioFile(forReading: url),
              let b = AVAudioPCMBuffer(pcmFormat: file.processingFormat, frameCapacity: AVAudioFrameCount(file.length)),
              (try? file.read(into: b)) != nil
        else {
            AnimLog.say("sound: \(t.name).m4a did not load")
            return nil
        }
        if Int(b.frameLength) != t.nSamples || Int(file.processingFormat.sampleRate) != t.rate {
            AnimLog.say("sound: \(t.name).m4a is \(b.frameLength) samples at \(Int(file.processingFormat.sampleRate)) Hz, "
                        + "the table says \(t.nSamples) at \(t.rate)")
        }
        return b
    }

    /// Take `i`'s buffer, decoding it now if the loader has not got to it.
    private func buffer(_ i: Int) -> AVAudioPCMBuffer? {
        lock.lock()
        if let b = buffers[i] { lock.unlock(); return b }
        let gone = missing.contains(i)
        lock.unlock()
        if gone { return nil }
        let b = decode(i)
        lock.lock()
        if let b { buffers[i] = b } else { missing.insert(i) }
        lock.unlock()
        return b
    }

    /// Take `i` back to front, made once.
    private func backwards(_ i: Int, _ b: AVAudioPCMBuffer) -> AVAudioPCMBuffer? {
        lock.lock(); defer { lock.unlock() }
        if let r = reversed[i] { return r }
        guard let r = AVAudioPCMBuffer(pcmFormat: b.format, frameCapacity: b.frameLength),
              let src = b.floatChannelData?[0], let dst = r.floatChannelData?[0] else { return nil }
        let n = Int(b.frameLength)
        for k in 0..<n { dst[k] = src[n - 1 - k] }
        r.frameLength = b.frameLength
        reversed[i] = r
        return r
    }

    /// `b` at `ratePm` (a stand-in's wander): the kernel's resampler.
    private func atSpeed(_ b: AVAudioPCMBuffer, _ ratePm: Int) -> AVAudioPCMBuffer? {
        if ratePm == 1000 { return b }
        let n = Int(b.frameLength)
        let len = Int(fio_cue_resampled_len(Int32(n), Int32(ratePm)))
        guard len > 0, let out = AVAudioPCMBuffer(pcmFormat: b.format, frameCapacity: AVAudioFrameCount(len)),
              let src = b.floatChannelData?[0], let dst = out.floatChannelData?[0],
              fio_cue_resample(src, Int32(n), Int32(ratePm), dst, Int32(len)) == Int32(len) else { return nil }
        out.frameLength = AVAudioFrameCount(len)
        return out
    }

    // ---- playing -------------------------------------------------------------------

    /// Strike cue `kind` (an ANIM_CUE_*) so that its MOMENT is `seconds` from
    /// now, standing for `nCards`; `undoes` is a REVERT's undone cue, `gainPm`
    /// the strike's own level (fio_seat_gain_pm for a dealt card). Answers what
    /// was scheduled - the take, and the strokes before it - and nothing for
    /// every silent case in this file's header.
    @discardableResult
    func strike(_ kind: Int, in seconds: Double, nCards: Int = 1, undoes: Int = 0,
                       gainPm: Int = 1000) -> [Scheduled] {
        guard Self.on, !muted, let engine, offline || engine.isRunning else { return [] }
        let now = mach_absolute_time()
        let leadUs = Int32(clamping: Int((max(0, seconds) * 1e6).rounded()))
        guard let p = fio_cue_strike(Int32(kind), leadUs, Int32(clamping: nCards), Int32(undoes), Int32(gainPm)),
              let s = try? readCueStrike(p), s.take >= 0 else { return [] }
        var out: [Scheduled] = []
        if let a = play(s.take, startUs: s.startUs, gainPm: s.gainPm, ratePm: s.ratePm,
                        backwards: s.backwards != 0, leadUs: Int(leadUs), now: now) { out.append(a) }
        // The strokes that gather the table first, over whatever is sounding.
        if s.prelude >= 0, let a = play(s.prelude, startUs: s.preludeStartUs, gainPm: s.preludeGainPm,
                                         ratePm: s.preludeRatePm, backwards: false, leadUs: Int(leadUs), now: now) {
            out.append(a)
        }
        return out
    }

    /// This player's "now" as a player-time sample, and the earliest sample a
    /// take may be handed: its last render carried to `host` by the host clock
    /// the render was stamped with (either way: a stamp can be the time that
    /// render reaches the speaker, after now), and never before that render
    /// plus a margin, so a player is not handed a time it has already rendered.
    /// Offline there is no host clock, and the engine's own count says where
    /// it is.
    private func nowSample(_ v: Voice, host: UInt64) -> (now: AVAudioFramePosition, floor: AVAudioFramePosition)? {
        guard let nt = v.node.lastRenderTime, nt.isSampleTimeValid,
              let pt = v.node.playerTime(forNodeTime: nt) else { return nil }
        if offline, let e = engine {
            let s = pt.sampleTime + (e.manualRenderingSampleTime - nt.sampleTime)
            return (s, s)
        }
        var s = pt.sampleTime
        if nt.isHostTimeValid {
            let ahead = host >= nt.hostTime ? AVAudioTime.seconds(forHostTime: host - nt.hostTime)
                                            : -AVAudioTime.seconds(forHostTime: nt.hostTime - host)
            s += AVAudioFramePosition(ahead * v.rate)
        }
        let quantum = max(0.005, AVAudioSession.sharedInstance().ioBufferDuration)
        return (s, max(s, pt.sampleTime) + AVAudioFramePosition(2 * quantum * v.rate))
    }

    private func play(_ i: Int, startUs: Int, gainPm: Int, ratePm: Int, backwards: Bool,
                      leadUs: Int, now host: UInt64) -> Scheduled? {
        let t = takes[i]
        guard let forwards = buffer(i),
              let shaped = backwards ? self.backwards(i, forwards) : forwards,
              let b = atSpeed(shaped, ratePm) else { return nil }
        let rate = Double(t.rate)
        // A player with nothing playing or pending, so its level is this take's alone.
        var pick: (Voice, AVAudioFramePosition, AVAudioFramePosition)?
        for v in voices where v.rate == rate {
            guard let n = nowSample(v, host: host) else { continue }
            if v.freeAt <= n.now { pick = (v, n.now, n.floor); break }
        }
        guard let (voice, now, floor) = pick else {
            AnimLog.say("sound: every \(Int(rate)) Hz player is busy - \(t.name) dropped")
            return nil
        }
        let start = max(floor, now + AVAudioFramePosition((Double(startUs) / 1e6 * rate).rounded()))
        let gain = Float(gainPm) / 1000
        voice.node.volume = min(1, gain)
        voice.freeAt = start + AVAudioFramePosition(b.frameLength)
        voice.node.scheduleBuffer(b, at: AVAudioTime(sampleTime: start, atRate: rate), options: [], completionHandler: nil)
        let len = Double(t.nSamples) / rate
        let anchor = (backwards ? len - Double(t.anchorUs) / 1e6 : Double(t.anchorUs) / 1e6) * 1000 / Double(ratePm)
        return Scheduled(take: i, name: t.name, rate: rate, backwards: backwards, startSample: start, nowSample: now,
                         momentAfter: Double(start - now) / rate + anchor, gain: gain)
    }

    /// Stop everything scheduled or sounding (a mute).
    func cancel() {
        for v in voices {
            v.node.stop()
            v.freeAt = 0
            v.node.play()
        }
    }

    // ---- the tests' engine -----------------------------------------------------

    #if DEBUG
    /// The same graph rendered by hand at `rate`, for a test to read the samples
    /// a strike really produces. No session, no speaker.
    func startOffline(rate: Double = 48000) throws {
        shutdown()
        let e = AVAudioEngine()
        guard let format = AVAudioFormat(standardFormatWithSampleRate: rate, channels: 1) else { return }
        try e.enableManualRenderingMode(.offline, format: format, maximumFrameCount: 4096)
        offline = true
        guard build(e) else { return }
        e.mainMixerNode.outputVolume = 1
        try e.start()
        for v in voices { v.node.play() }
        users = 1
    }

    /// Render `frames` more samples of the offline engine.
    func render(_ frames: Int) throws -> [Float] {
        guard let e = engine, offline,
              let buf = AVAudioPCMBuffer(pcmFormat: e.manualRenderingFormat, frameCapacity: 4096) else { return [] }
        var out: [Float] = []
        out.reserveCapacity(frames)
        while out.count < frames {
            let n = AVAudioFrameCount(min(4096, frames - out.count))
            guard try e.renderOffline(n, to: buf) == .success, let d = buf.floatChannelData?[0] else { break }
            out.append(contentsOf: UnsafeBufferPointer(start: d, count: Int(buf.frameLength)))
        }
        return out
    }

    /// Take `i`'s decoded samples, as the engine would play them.
    func samples(_ i: Int, backwards: Bool = false) -> [Float] {
        guard let b = buffer(i), let s = (backwards ? self.backwards(i, b) : b)?.floatChannelData?[0] else { return [] }
        return Array(UnsafeBufferPointer(start: s, count: Int(b.frameLength)))
    }

    /// How many files are decoded, and their samples' bytes.
    var loadedBytes: (files: Int, bytes: Int) {
        lock.lock(); defer { lock.unlock() }
        return (buffers.count, buffers.values.reduce(0) { $0 + Int($1.frameLength) * 4 }
                + reversed.values.reduce(0) { $0 + Int($1.frameLength) * 4 })
    }

    /// Block until the loader has decoded everything it will.
    func waitLoaded(timeout: Double = 30) {
        let end = Date().addingTimeInterval(timeout)
        while Date() < end {
            lock.lock(); let busy = loading || nextToLoad() != nil; lock.unlock()
            if !busy { return }
            Thread.sleep(forTimeInterval: 0.01)
        }
    }
    #endif
}
