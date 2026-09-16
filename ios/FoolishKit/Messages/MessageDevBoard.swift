// MessageDevBoard.swift — DEBUG-only seeded board state for verification runs.
//
// NEVER COMPILED INTO A SHIPPING BUILD. The whole file is inside
// `#if DEBUG || SOLO_TESTING`, the same gate MessageDebugFlags uses: a
// TestFlight/App Store build defines neither condition, so none of this exists
// in the product.
//
// WHY IT EXISTS. Some board animations only misbehave at states that are tedious
// to reach by hand. The round-12 case is the pickup sweep: the owner reported
// that "pickup animation sometimes quickly rearranges into grid before moving to
// hand for many players", and reproducing it needs a table carrying ten cards
// with several COVERED pairs among them - minutes of careful tapping per
// attempt, and not reliably reachable at all.
//
// SO THE STATE IS A CONSTANT, NOT A SEARCH. The owner's instruction: "go run
// some seeds (in C not the simulator) and try to find a replay code that ends up
// with a bunch of cards on the table. Then simply feed that replay code to the
// seeded fixed state, seat yourself as defender, and hit pickup. THAT SIMPLE."
//
// That is exactly what this does, and it is better than the alternative in every
// way that matters. `c/tests/msg_wire_test --fatboard 10 2` searches deals in
// microseconds and prints ONE FMSG envelope as hex; this file just reads that
// hex and opens it. There is no search logic on the device, no dependence on
// what the deal happens to allow, and the same board comes up every single run -
// which is what makes a filmed before/after comparable at all.
//
// (An earlier version played the game forward through the kernel ON DEVICE at
// launch. It worked, and it was the wrong shape: slower, non-deterministic
// across deals, and it put a legality search in the extension to produce
// something a build-time constant could state outright.)
//
// IT ALSO SKIPS THE WHOLE FLOW. With the flag set, the extension does not show
// setup, does not create a lobby, does not join or start: `load()` opens this
// chain directly, seated as the DEFENDER, so the entire verification run is
// "open the extension, hit Pickup, record" (owner: "use build flags to skip the
// create game / join game / start game stuff and jump straight to the game
// state").
//
// HOW TO USE IT (simulator or device):
//   c/build/msg_wire_test --fatboard 10 2 > /tmp/fat.hex
//   cp /tmp/fat.hex <AppGroup>/dev.fatboard
//   …then just open the extension.
// `ios/Tools/msgrig.sh fatboard` does all of it, including the search.
//
// A FILE, not a UserDefaults key, for the same reason `dev.seed` is one:
// `defaults write` from outside the sandbox lands in the wrong domain, and
// cfprefsd caches App Group preferences until a reboot.

#if DEBUG || SOLO_TESTING
import Foundation

public enum MessageDevBoard {
    private static let appGroup = "group.cards.foolish.msg"
    private static let flagFile = "dev.fatboard"
    private static let claimFile = "dev.claimed"
    private static let stagedFile = "dev.staged"
    private static let soloNameFile = "dev.soloname"
    private static let seatFile = "dev.seat"
    private static let replayFile = "dev.replay"
    private static let stageFile = "dev.stage"
    private static let slowmoFile = "dev.slowmo"
    private static let rulerFile = "dev.ruler"
    private static let collapseFile = "dev.collapse"
#if RIG_RESEED
    private static let reseedFile = "dev.reseed"
#endif

    /// The seeded chain, or nil when the flag file is absent - which is the
    /// normal case, including every ordinary DEBUG run.
    ///
    /// Hex, because it survives a shell pipeline, a `cp`, and an editor without
    /// anyone having to agree on a base32 alphabet or a padding rule; the C side
    /// prints it and this reads it, and there is no third opinion.
    public static var seededPayload: Data? {
        guard let raw = seededHex else { return nil }
        return hex(raw)
    }

    /// The flag file's contents, trimmed - the exact string the rig wrote.
    /// Split out of `seededPayload` so the claim receipt below can echo the
    /// SAME characters back, rather than a re-encoding of the bytes that a
    /// shell comparison would then have to agree with about case.
    private static var seededHex: String? {
        guard let dir = FileManager.default
            .containerURL(forSecurityApplicationGroupIdentifier: appGroup),
              let raw = try? String(contentsOf: dir.appendingPathComponent(flagFile),
                                    encoding: .utf8)
        else { return nil }
        return raw.trimmingCharacters(in: .whitespacesAndNewlines)
    }

    /// ONCE PER PROCESS. The seed answers "what does the extension open ONTO",
    /// and after that the surface belongs to whatever the run does next - which
    /// for a finished board is New game, whose rematch lobby a second reading
    /// would throw away and re-open the finished game over the top of. A film of
    /// what happens AFTER the seeded state was impossible until this.
    ///
    /// Per PROCESS, not per file: `msgrig.sh reopen` kills Messages precisely so
    /// the seeded state can be opened cold again, and that still works.
    ///
    /// AND IT LEAVES A RECEIPT (`dev.claimed`), which is not bookkeeping - it is
    /// the only way a driver can know this happened at all.
    ///
    /// "Once per process" is a promise about a process the rig does not own.
    /// `rig.sh chain` re-seeds between every send and relies on leaving the
    /// thread to kill the appex, because only a DEAD appex claims the next
    /// seed; when that does not take, the extension re-opens on the seed it
    /// already claimed and stages THAT - so every bubble in the transcript is
    /// one move behind, silently, and each frame still looks individually
    /// plausible (a defender does not change within a bout, so even the caption
    /// row reads correctly). A whole chain shoot was read three times as a
    /// caption bug on that evidence.
    ///
    /// The receipt turns it into a fact the driver can check before it presses
    /// Send: the hex actually claimed, by the process that claimed it. The rig
    /// deletes the file, seeds, re-opens, and refuses to send until this says
    /// the seed it asked for. Written best-effort - a failed write costs the
    /// rig a retry, never a frame.
    public static func claimSeededPayload() -> Data? {
        guard let raw = seededHex, let p = hex(raw) else { return nil }
        // ONCE PER PROCESS. The RIG_RESEED build relaxes it to once per
        // DISTINCT payload; without that flag this compiles to `guard !claimed`
        // exactly as before.
#if RIG_RESEED
        guard !claimed || (reseeds && raw != claimedHex) else { return nil }
#else
        guard !claimed else { return nil }
#endif
        claimed = true
        claimedHex = raw
        if let dir = FileManager.default
            .containerURL(forSecurityApplicationGroupIdentifier: appGroup) {
            try? Data(raw.utf8).write(to: dir.appendingPathComponent(claimFile))
        }
        return p
    }
    private static var claimed = false
    private static var claimedHex: String?

#if RIG_RESEED
    /// `dev.reseed`: LET A LIVE APPEX READ A NEW SEED.
    ///
    /// Off unless the file exists, which is the whole point. "Once per process"
    /// above is a real rule with a real reason - a second reading would throw
    /// away whatever the run did next and re-open the seeded state over the top
    /// of it - and it stays the default for every ordinary DEBUG run.
    ///
    /// What it costs the rig is the other half: the ONLY way to make the next
    /// seed readable is to kill the appex, and the only way to do that is to
    /// leave the thread - so every seeded frame pays a leave, a blind
    /// conversation-row probe, and a re-open, for a state change that is a file
    /// write. With this set, a driver writes `dev.fatboard` and the surface
    /// picks it up where it stands.
    ///
    /// It is keyed on the CONTENTS, not on a touch: re-reading the same hex is
    /// still refused, so the ordinary "opened onto a seed, then the run did
    /// something" case cannot be clobbered by a stray re-read. Only a payload
    /// this process has genuinely not claimed re-arms it.
    ///
    /// Never in Release: this whole type is `#if DEBUG || SOLO_TESTING`.
    public static var reseeds: Bool {
        guard let dir = FileManager.default
            .containerURL(forSecurityApplicationGroupIdentifier: appGroup)
        else { return false }
        return FileManager.default.fileExists(atPath: dir.appendingPathComponent(reseedFile).path)
    }

    /// True when `dev.reseed` is on and the flag file names a payload this
    /// process has not opened onto. The watcher in MessagesRootView polls this;
    /// with the flag absent it is always false, so the watcher never arms.
    public static var hasUnclaimedReseed: Bool {
        guard reseeds, let raw = seededHex else { return false }
        return raw != claimedHex
    }
#endif

    /// THE SECOND RECEIPT: the payload that actually reached the input field.
    ///
    /// A claim receipt only says what the extension OPENED onto. What Send
    /// transmits is whatever `stage()` last inserted, and those are separated
    /// by the whole expanded tail - a settle wait, a collapse, a transition -
    /// which is over a second. A driver that presses Send as soon as a Send
    /// button exists therefore transmits the PREVIOUS bubble, every time, and
    /// the transcript comes out one move behind with a correct claim receipt
    /// beside it. That is the lag that was filed as a caption bug three times;
    /// closing the claim half of it was not enough, because the claim lands
    /// early and the insert lands late.
    ///
    /// So `stage()` says so. The rig deletes this, seeds, opens, and waits for
    /// THIS to name the payload it asked for before it presses anything. No
    /// sleep can stand in for it: the tail's length depends on the animation
    /// the seeded state happens to play.
    ///
    /// Best-effort, like the claim receipt: a failed write costs the rig a
    /// wait, never a frame.
    public static func noteStaged(_ payload: Data) {
        guard let dir = FileManager.default
            .containerURL(forSecurityApplicationGroupIdentifier: appGroup) else { return }
        let s = payload.map { String(format: "%02x", $0) }.joined()
        try? Data(s.utf8).write(to: dir.appendingPathComponent(stagedFile))
    }

    /// WHAT TO CALL THE PUPPET SEAT that `addSoloSeat` adds.
    ///
    /// It was the literal "Solo 2", which is fine for a developer filling a
    /// lobby and wrong in a photograph: a full lobby is the only way to reach
    /// the shipping Start/Exit row, so every store frame of a lobby had a
    /// placeholder name sitting in the roster next to a real one. Reading it
    /// from the group lets the rig seat "Kate" there and keeps the cast
    /// consistent with every other frame in the set.
    ///
    /// Unset, nothing moves - the caller keeps its own default.
    public static var soloName: String? {
        guard let dir = FileManager.default
            .containerURL(forSecurityApplicationGroupIdentifier: appGroup),
              let raw = try? String(contentsOf: dir.appendingPathComponent(soloNameFile),
                                    encoding: .utf8)
        else { return nil }
        let t = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        return t.isEmpty ? nil : t
    }

    /// Which seat to sit at, or nil to sit at the defender's.
    ///
    /// The default suits the pickup case (only the defender may pick up), but
    /// the DEAL case needs the opposite chair: it is an ATTACKER saying good
    /// that closes the bout and triggers the round transition, and the deal is
    /// what the animation under test belongs to.
    public static var seededSeat: Int? {
        guard let dir = FileManager.default
            .containerURL(forSecurityApplicationGroupIdentifier: appGroup),
              let raw = try? String(contentsOf: dir.appendingPathComponent(seatFile),
                                    encoding: .utf8),
              let n = Int(raw.trimmingCharacters(in: .whitespacesAndNewlines))
        else { return nil }
        return n
    }

    /// Should a seeded open REPLAY the bubble it opens (round 16)?
    ///
    /// Normally no, and that is the older and commoner case: a seeded board is
    /// a state to act on, not a move anyone just watched, so opening it must not
    /// animate whatever its last action happened to be (the film would start
    /// with an animation nobody asked for). The animation IS the subject for the
    /// bubble-delta work, though - "open the bubble for the second cover and you
    /// should see only that cover" is a claim about exactly this replay - so the
    /// presence of `dev.replay` turns it back on for a run that means to film it.
    ///
    /// A file, like every other flag here, for the reasons in the header note.
    /// Should a seeded open ALSO STAGE the chain it opened, as a bubble?
    ///
    /// For store photography. Every frame is a drawer over a chat, and a
    /// transcript whose last bubble belongs to some other game makes the frame
    /// a lie - the owner's words on seeing one: "they show impossible
    /// sequences ... a start game bubble, then a start game bubble, and then
    /// the view shows a game very much halfway through". Staging the seeded
    /// chain and sending it puts THIS board in the transcript, so the bubble
    /// above the drawer is the move that produced what is under it.
    ///
    /// Nothing else changes: it is the ordinary stage path, with the ordinary
    /// bubble, and the human still presses Send.
    public static var seededStages: Bool {
        guard let dir = FileManager.default
            .containerURL(forSecurityApplicationGroupIdentifier: appGroup)
        else { return false }
        return FileManager.default.fileExists(atPath: dir.appendingPathComponent(stageFile).path)
    }

    public static var seededReplays: Bool {
        guard let dir = FileManager.default
            .containerURL(forSecurityApplicationGroupIdentifier: appGroup)
        else { return false }
        return FileManager.default.fileExists(atPath: dir.appendingPathComponent(replayFile).path)
    }

    /// How far to stretch every animation, or 0 for real time.
    ///
    /// `HARNESS_SLOWMO` cannot reach here: an app extension is spawned by the
    /// system, not by Messages, so `SIMCTL_CHILD_` never lands in its
    /// environment and the one knob that makes a filmed flight readable is
    /// exactly the one the real surface could not have. A file can, like every
    /// other flag on this rig. Read ONCE - the value is asked for on every
    /// flight, and a per-flight file read is a stutter in the thing being
    /// filmed.
    public static let slowmo: Double = {
        guard let dir = FileManager.default
            .containerURL(forSecurityApplicationGroupIdentifier: appGroup),
              let raw = try? String(contentsOf: dir.appendingPathComponent(slowmoFile),
                                    encoding: .utf8),
              let n = Double(raw.trimmingCharacters(in: .whitespacesAndNewlines)), n > 0
        else { return 0 }
        return n
    }()

    /// Draw the debug RULER on the surface's own box (`CollapseRuler`)?
    ///
    /// The collapse tween's curve is measured off filmed frames, and a frame
    /// can only be read if the box's two edges are visible in it - the drawer
    /// is wool above and wool below, and the host composites the transition
    /// from snapshots, so there is nothing else in the picture that says where
    /// our box is. Round 10d drew this ruler, measured, and threw it away; it
    /// is a flag now so the measurement can be repeated rather than rebuilt.
    ///
    /// Read ONCE, like `slowmo` and for the same reason: it is asked for on
    /// every layout pass of the thing being filmed.
    public static let rulerOn: Bool = {
        guard let dir = FileManager.default
            .containerURL(forSecurityApplicationGroupIdentifier: appGroup)
        else { return false }
        return FileManager.default.fileExists(atPath: dir.appendingPathComponent(rulerFile).path)
    }()

    /// The collapse driver's knobs, for a filmed sweep: `lead=0.02 hz=120
    /// resp=0.338` (seconds, Hz, seconds; any subset) in `dev.collapse`. The
    /// defaults are the shipped values in `CollapseTween`; the file exists so
    /// a sweep is a file write and a take, not a rebuild per point. Read ONCE,
    /// like `slowmo` and for the same reason.
    public struct CollapseKnobs {
        public var lead = CollapseTween.hostLead
        public var hz = CollapseTween.driveHz
        public var response = CollapseTween.hostResponse
        /// Carry the collapse on the layer instead of on the timer - see
        /// `CollapseTween.slideDuration`. A knob so both paths can be filmed
        /// on one build and scored against each other.
        ///
        /// Defaults to the SHIPPING value, not to a literal. It was a hardcoded
        /// `false` beside a `slideByDefault` that controls only Release, which
        /// meant flipping the product on would have left every debug install and
        /// every rig take without a knob file quietly measuring the old path.
        public var slide = CollapseTween.slideByDefault
    }
    public static let collapseKnobs: CollapseKnobs = {
        var k = CollapseKnobs()
        guard let dir = FileManager.default
            .containerURL(forSecurityApplicationGroupIdentifier: appGroup),
              let raw = try? String(contentsOf: dir.appendingPathComponent(collapseFile),
                                    encoding: .utf8)
        else { return k }
        for pair in raw.split(whereSeparator: { $0 == " " || $0 == "\n" }) {
            let kv = pair.split(separator: "=", maxSplits: 1)
            guard kv.count == 2, let v = Double(kv[1]) else { continue }
            switch kv[0] {
            case "lead": k.lead = v
            case "hz": k.hz = v
            case "resp": k.response = v
            case "slide": k.slide = v != 0
            default: break
            }
        }
        return k
    }()

    /// Even-length hex to bytes; nil on anything malformed, so a truncated or
    /// half-written file reads as "no seed" rather than as a damaged game.
    private static func hex(_ s: String) -> Data? {
        let chars = Array(s.utf8)
        guard chars.count >= 2, chars.count % 2 == 0 else { return nil }
        var out = Data(capacity: chars.count / 2)
        var i = 0
        while i < chars.count {
            guard let hi = nibble(chars[i]), let lo = nibble(chars[i + 1]) else { return nil }
            out.append(hi << 4 | lo)
            i += 2
        }
        return out
    }

    private static func nibble(_ c: UInt8) -> UInt8? {
        switch c {
        case 0x30...0x39: return c - 0x30              // 0-9
        case 0x61...0x66: return c - 0x61 + 10         // a-f
        case 0x41...0x46: return c - 0x41 + 10         // A-F
        default: return nil
        }
    }
}
#endif
