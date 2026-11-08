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
// THE CLOCK. A strike is scheduled, never timed with a sleep. Every take is
// handed to the kernel's output mix (c/src/cue_mix.h, through fio_mix_*) with
// the output frame it starts on, and the engine's one source node sums the
// mix into each block it renders, so the frame given is the frame heard, to
// the sample. "Now" is the frame heard at this host time, read off the last
// block's timestamp (fio_mix_frame_at). A take that would start in a block
// already rendered starts in the next one, whole: a take is never cut (Alex:
// "play them in full").
//
// NOT AVAudioPlayerNode. Scheduled at a sample time on a player, a take came
// out 25 samples late and 3 dB down through one mixer, and 60 ms late through
// one more node, on the engine's own offline render; summed here it lands on
// its frame exactly (FoolishTests/CuePlayerTests).
//
// THE FILES are soundgen's (ios/scripts/bundle_sounds.sh copies them into this
// framework as sounds/<name>.m4a). Core Audio honours the files' edit lists, so
// a decoded take starts on the WAV's first sample (docs/AUDIO.md "The priming,
// measured") and is as long as the table says, which `decode` checks. Each is
// converted once, as it is decoded, to the output's rate (AVAudioConverter at
// its best quality), so the mix never resamples. A reversed copy is made when
// a revert needs one.
//
// WHAT IS DECODED is the kernel's answer for the board on screen
// (fio_cues_next, `want`); "Loading" below says the rest.
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
// slot of the mix is taken (32 takes sounding or pending at once).

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

    /// The web's mute, read from the player's setting (FPrefs.soundOn).
    private(set) var muted = !FPrefs.storedSoundOn
    private(set) var volume: Float = CuePlayer.defaultVolume

    private let bundle: Bundle
    private var engine: AVAudioEngine?
    private var source: AVAudioSourceNode?
    /// The kernel's output mix (fio_mix_new), summed by `source`'s callback.
    private var mixer: UnsafeMutableRawPointer?
    /// The output's frames a second, and what each take is converted to.
    private var outRate: Double = 0
    /// The samples of every take in the mix, by slot, kept alive until the mix
    /// says the slot is free.
    private var inMix: [Int32: AVAudioPCMBuffer] = [:]
    private var users = 0
    private var configObserver: NSObjectProtocol?
    /// True while the engine renders by hand (the tests), where no host time runs.
    private var offline = false
    private static let ticksPerSecond: Double = 1 / AVAudioTime.seconds(forHostTime: 1)

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

    /// One take on its way, for the trace and the tests: where it starts and
    /// where its anchor falls, in output frames.
    struct Scheduled: Equatable {
        let take: Int
        let name: String
        let rate: Double
        let backwards: Bool
        /// Output frames: the start, and the frame heard as it was scheduled.
        let startSample: AVAudioFramePosition
        let nowSample: AVAudioFramePosition
        /// Where the cue's moment falls, in seconds from the strike.
        let momentAfter: Double
        let gain: Float
        /// The host time it was scheduled from, so a trace can place `momentAfter`.
        let struck: UInt64
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
        engine?.mainMixerNode.outputVolume = Self.silent ? 0 : volume
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
        source = nil
        // The callback has stopped with the engine, so the mix is nobody's now.
        if let m = mixer { fio_mix_free(m) }
        mixer = nil
        inMix = [:]
        offline = false
        lock.lock()
        buffers = [:]; reversed = [:]; missing = []
        generation += 1
        lock.unlock()
        if !offline { try? AVAudioSession.sharedInstance().setActive(false, options: .notifyOthersOnDeactivation) }
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
        AnimLog.say("sound: engine up at \(Int(outRate)) Hz, session \(session.category.rawValue), "
                    + "io \(Int(session.ioBufferDuration * 1000)) ms, out \(Int(session.sampleRate)) Hz, silent=\(Self.silent)")
        pump()
    }

    /// The graph: one source node, at the output's own rate and width, into the
    /// engine's mixer (the master level). Its callback is the kernel's mix: the
    /// one mono sum, copied to every channel (a mono take is as loud on each
    /// side as the web plays it, where a mixer's pan law would take 3 dB off).
    private func build(_ e: AVAudioEngine) -> Bool {
        let out = e.outputNode.outputFormat(forBus: 0)
        let rate = out.sampleRate > 0 ? out.sampleRate : 48000
        let channels = max(1, out.channelCount)
        guard let format = AVAudioFormat(standardFormatWithSampleRate: rate, channels: channels),
              let m = fio_mix_new(rate) else { return false }
        let src = AVAudioSourceNode(format: format) { _, stamp, frames, abl -> OSStatus in
            let list = UnsafeMutableAudioBufferListPointer(abl)
            guard let first = list.first?.mData?.assumingMemoryBound(to: Float.self) else { return noErr }
            let host = stamp.pointee.mFlags.contains(.hostTimeValid) ? stamp.pointee.mHostTime : 0
            fio_mix_render(m, first, Int32(frames), host)
            for b in list.dropFirst() {
                b.mData?.copyMemory(from: first, byteCount: Int(frames) * MemoryLayout<Float>.size)
            }
            return noErr
        }
        e.attach(src)
        e.connect(src, to: e.mainMixerNode, format: format)
        e.mainMixerNode.outputVolume = Self.silent ? 0 : volume
        engine = e; source = src; mixer = m; outRate = rate
        return true
    }

    // ---- loading -------------------------------------------------------------------
    //
    // WHAT IS RESIDENT is what the kernel says could sound next (fio_cues_next,
    // `want`): those cues' own and tight takes are decoded ahead, in the
    // table's order, and kept. Everything else is decoded when it is struck,
    // off the main thread, and the strike then follows the web's rule for a
    // file that was still on its way: started if that is no more than LATE_S
    // behind the time it was aimed at, dropped otherwise. A struck take of a
    // cue nobody wanted stays until the board's answer changes. The STROKES
    // before a sweep or a pickup (the preludes, about 60% of every sample the
    // game has) are never resident: decoded for their strike and let go once
    // the player has them.

    /// How far behind its aim a strike that waited for its file may still start
    /// (the web's cueAudio.ts LATE_S).
    static let lateS = 0.08

    /// The cues that could sound next from the board on screen, one bit per
    /// ANIM_CUE_* (fio_cues_next): their takes are decoded and kept; what no
    /// cue in the answer plays any more is let go.
    func want(_ mask: UInt32) {
        lock.lock()
        let changed = mask != wanted
        wanted = mask
        if changed {
            for i in buffers.keys where !resident(i) { buffers[i] = nil; reversed[i] = nil }
        }
        lock.unlock()
        pump()
    }

    /// The kernel's answer for a board, asked with its seven scalars.
    func want(_ v: GameView, mySeat: Int) {
        let covered = v.battles.filter { $0.defense != nil }.count
        want(UInt32(fio_cues_next(Int32(v.status), Int32(mySeat), Int32(v.players.count),
                                  Int32(v.battles.count), Int32(covered), v.hasFlipped ? 1 : 0,
                                  Int32(v.deckCount))))
    }

    /// Whether take `i` is one the board's answer keeps decoded. Under `lock`.
    private func resident(_ i: Int) -> Bool {
        let t = takes[i]
        return t.role != CUE_ROLE_PRELUDE && wanted & (1 << UInt32(t.cue)) != 0
    }

    /// The next take to decode ahead. Under `lock`.
    private func nextToLoad() -> Int? {
        takes.indices.first { buffers[$0] == nil && !missing.contains($0) && resident($0) }
    }

    /// Decode the resident set, one file at a time off the main thread.
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
                    if let b { if self.resident(i) { self.buffers[i] = b } } else { self.missing.insert(i) }
                }
                self.lock.unlock()
            }
        }
    }

    /// One file, decoded and converted to the output's rate, or nil when it is
    /// missing, not the length the table says, or there is no engine to say
    /// the rate.
    private func decode(_ i: Int) -> AVAudioPCMBuffer? {
        let t = takes[i]
        let rate = outRate
        guard rate > 0,
              let url = bundle.url(forResource: t.name, withExtension: "m4a", subdirectory: "sounds"),
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
        return Self.convert(b, to: rate)
    }

    /// `b` at `rate`, by Core Audio's best converter, its first sample still the
    /// take's first (the converter's priming is its own: `.normal` assumes the
    /// silence before the take and adds no delay, which the tests hold to a
    /// fraction of a millisecond). The same buffer when the rates already agree.
    static func convert(_ b: AVAudioPCMBuffer, to rate: Double) -> AVAudioPCMBuffer? {
        if b.format.sampleRate == rate { return b }
        guard let to = AVAudioFormat(standardFormatWithSampleRate: rate, channels: b.format.channelCount),
              let c = AVAudioConverter(from: b.format, to: to) else { return nil }
        c.sampleRateConverterQuality = AVAudioQuality.max.rawValue
        c.sampleRateConverterAlgorithm = AVSampleRateConverterAlgorithm_Mastering
        c.primeMethod = .normal
        let want = AVAudioFrameCount((Double(b.frameLength) * rate / b.format.sampleRate).rounded())
        guard let out = AVAudioPCMBuffer(pcmFormat: to, frameCapacity: want + 1024) else { return nil }
        var given = false
        var err: NSError?
        let status = c.convert(to: out, error: &err) { _, st in
            if given { st.pointee = .endOfStream; return nil }
            given = true
            st.pointee = .haveData
            return b
        }
        guard status != .error, err == nil else { return nil }
        out.frameLength = min(out.frameLength, want)
        return out
    }

    /// Take `i`'s buffer if it is decoded (nil and false), or that it never
    /// will be (nil and true).
    private func held(_ i: Int) -> (AVAudioPCMBuffer?, gone: Bool) {
        lock.lock(); defer { lock.unlock() }
        return (buffers[i], missing.contains(i))
    }

    /// Keep a take decoded for a strike: a prelude never, anything else until
    /// the board's answer changes.
    private func keep(_ i: Int, _ b: AVAudioPCMBuffer?, gen: Int) {
        lock.lock(); defer { lock.unlock() }
        guard gen == generation else { return }
        if let b { if takes[i].role != CUE_ROLE_PRELUDE { buffers[i] = b } } else { missing.insert(i) }
    }

    /// `b` back to front; kept beside a resident take, made fresh otherwise.
    private func backwards(_ i: Int, _ b: AVAudioPCMBuffer) -> AVAudioPCMBuffer? {
        lock.lock(); defer { lock.unlock() }
        if let r = reversed[i] { return r }
        guard let r = AVAudioPCMBuffer(pcmFormat: b.format, frameCapacity: b.frameLength),
              let src = b.floatChannelData?[0], let dst = r.floatChannelData?[0] else { return nil }
        let n = Int(b.frameLength)
        for k in 0..<n { dst[k] = src[n - 1 - k] }
        r.frameLength = b.frameLength
        if buffers[i] != nil { reversed[i] = r }
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
    /// was scheduled at once - the take, and the strokes before it - and hands
    /// every take scheduled, at once or after its file decoded, to `heard`.
    /// Nothing, for every silent case in this file's header.
    @discardableResult
    func strike(_ kind: Int, in seconds: Double, nCards: Int = 1, undoes: Int = 0,
                gainPm: Int = 1000, heard: ((Scheduled) -> Void)? = nil) -> [Scheduled] {
        guard Self.on, !muted, let engine, offline || engine.isRunning else { return [] }
        let now = mach_absolute_time()
        let leadUs = Int32(clamping: Int((max(0, seconds) * 1e6).rounded()))
        guard let p = fio_cue_strike(Int32(kind), leadUs, Int32(clamping: nCards), Int32(undoes), Int32(gainPm)),
              let s = try? readCueStrike(p), s.take >= 0 else { return [] }
        var out: [Scheduled] = []
        let want = [(s.take, s.startUs, s.gainPm, s.ratePm, s.backwards != 0)]
            // The strokes that gather the table first, over whatever is sounding.
            + (s.prelude >= 0 ? [(s.prelude, s.preludeStartUs, s.preludeGainPm, s.preludeRatePm, false)] : [])
        for (i, startUs, g, r, back) in want {
            let (b, gone) = held(i)
            if gone { continue }
            if let b {
                if let a = play(i, b, startUs: startUs, gainPm: g, ratePm: r, backwards: back, now: now) {
                    out.append(a); heard?(a)
                }
                continue
            }
            // Not decoded. Offline (the tests) the clock stands still while it
            // decodes, so it is decoded here; live, off the main thread, and
            // the strike waits for it by the web's rule.
            let gen = generation
            if offline {
                let d = decode(i)
                keep(i, d, gen: gen)
                if let d, let a = play(i, d, startUs: startUs, gainPm: g, ratePm: r, backwards: back, now: now) {
                    out.append(a); heard?(a)
                }
                continue
            }
            loader.async { [weak self] in
                guard let self else { return }
                let d = self.decode(i)
                DispatchQueue.main.async {
                    self.keep(i, d, gen: gen)
                    guard let d, gen == self.generation, self.engine != nil, !self.muted else { return }
                    let late = Self.waited(aimedUs: startUs, since: now, at: mach_absolute_time())
                    guard let startNow = late.startUs else {
                        AnimLog.say(String(format: "sound: %@ decoded %.0f ms behind its aim - dropped",
                                           self.takes[i].name, late.behindMs))
                        return
                    }
                    if let a = self.play(i, d, startUs: startNow, gainPm: g, ratePm: r, backwards: back,
                                         now: mach_absolute_time()) { heard?(a) }
                }
            }
        }
        return out
    }

    /// A strike that waited for its file, `elapsed` after it was struck: when
    /// its take now starts (from now, never negative), or nil when that is more
    /// than LATE_S behind where it was aimed. The web's rule, as a pure answer.
    static func waited(aimedUs: Int, since struck: UInt64, at now: UInt64) -> (startUs: Int?, behindMs: Double) {
        let elapsedUs = now > struck ? AVAudioTime.seconds(forHostTime: now - struck) * 1e6 : 0
        let left = Double(aimedUs) - elapsedUs
        if left >= 0 { return (Int(left.rounded()), 0) }
        return (-left <= lateS * 1e6 ? 0 : nil, -left / 1000)
    }

    /// The output frame heard at host time `host`: the last block's stamp
    /// carried forward. Offline there is no host clock, and it is the next
    /// frame to be rendered.
    private func frame(at host: UInt64) -> AVAudioFramePosition {
        guard let m = mixer else { return 0 }
        return offline ? fio_mix_now(m) : fio_mix_frame_at(m, host, Self.ticksPerSecond)
    }

    private func play(_ i: Int, _ forwards: AVAudioPCMBuffer, startUs: Int, gainPm: Int, ratePm: Int,
                      backwards: Bool, now host: UInt64) -> Scheduled? {
        let t = takes[i]
        guard let m = mixer,
              let shaped = backwards ? self.backwards(i, forwards) : forwards,
              let b = atSpeed(shaped, ratePm), let x = b.floatChannelData?[0] else { return nil }
        // Give back the samples of every take the mix has finished with.
        let live = fio_mix_live(m)
        inMix = inMix.filter { live & (1 << UInt32($0.key)) != 0 }
        let now = frame(at: host)
        let start = now + AVAudioFramePosition((Double(startUs) / 1e6 * outRate).rounded())
        let gain = Float(gainPm) / 1000
        let slot = fio_mix_add(m, x, Int32(b.frameLength), start, gain)
        guard slot >= 0 else {
            AnimLog.say("sound: the mix is full - \(t.name) dropped")
            return nil
        }
        inMix[slot] = b
        let len = Double(t.nSamples) / Double(t.rate)
        let anchor = (backwards ? len - Double(t.anchorUs) / 1e6 : Double(t.anchorUs) / 1e6) * 1000 / Double(ratePm)
        return Scheduled(take: i, name: t.name, rate: outRate, backwards: backwards, startSample: start, nowSample: now,
                         momentAfter: Double(start - now) / outRate + anchor, gain: gain, struck: host)
    }

    /// Stop everything scheduled or sounding (a mute).
    func cancel() {
        if let m = mixer { fio_mix_clear(m) }
    }

    // ---- the tests' engine -----------------------------------------------------

    #if DEBUG
    /// The same graph rendered by hand at `rate`, for a test to read the samples
    /// a strike really produces. No session, no speaker.
    func startOffline(rate: Double = 48000, channels: AVAudioChannelCount = 1) throws {
        shutdown()
        let e = AVAudioEngine()
        guard let format = AVAudioFormat(standardFormatWithSampleRate: rate, channels: channels) else { return }
        try e.enableManualRenderingMode(.offline, format: format, maximumFrameCount: 4096)
        offline = true
        guard build(e) else { return }
        try e.start()
        users = 1
    }

    /// Render `frames` more samples of the offline engine: its first channel.
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
        guard let b = held(i).0 ?? decode(i),
              let s = (backwards ? self.backwards(i, b) : b)?.floatChannelData?[0] else { return [] }
        return Array(UnsafeBufferPointer(start: s, count: Int(b.frameLength)))
    }

    /// Take `i` as its file holds it, at its own rate, before any conversion.
    func fileSamples(_ i: Int) -> [Float] {
        guard let url = bundle.url(forResource: takes[i].name, withExtension: "m4a", subdirectory: "sounds"),
              let f = try? AVAudioFile(forReading: url),
              let b = AVAudioPCMBuffer(pcmFormat: f.processingFormat, frameCapacity: AVAudioFrameCount(f.length)),
              (try? f.read(into: b)) != nil, let x = b.floatChannelData?[0] else { return [] }
        return Array(UnsafeBufferPointer(start: x, count: Int(b.frameLength)))
    }

    /// Whether take `i` is decoded and kept.
    func isResident(_ i: Int) -> Bool { held(i).0 != nil }

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
