// Pk.swift - the Swift face of the kernel bridge (pickemup/c/ios/include/pk_api.h).
//
// SWIFT HOLDS NO RULE. Every question about the game - what is legal, what a
// seat may see, what a line says, where a card goes - is one call into C, and
// every struct the kernel hands back is read through the generated readers
// (Generated/PickemupKernel.swift, structgen), never by offset here.
//
// THE RESIDENT GAME IS ONE SLOT (pk_api.h). `read` ADOPTS: decoding a bubble
// replaces whatever was resident. Nothing in this module reads, awaits and
// then stages; every caller reads and acts in one synchronous run on the main
// thread, which is also why this is a caseless enum and not an object with
// state of its own.

import CPickemup
import Foundation
import Security

public enum Pk {

    // MARK: the pair

    /// The library and the readers were generated for one layout (D46). A
    /// mismatch is a stale xcframework or a stale Generated/: refuse, never
    /// read at a wrong offset.
    public static let layoutMatches: Bool = pk_api_layout_hash() == SG_LAYOUT_HASH

    /// EVERY GENERATED READ GOES THROUGH HERE, so a mismatched pair reads
    /// nothing at all (nil), whoever asks and whenever: the model's first
    /// refresh runs before any screen is chosen. Which SCREEN a mismatch shows
    /// is PickemupHost.readable's; these are two vectors, not two fixes.
    private static func snap<T>(_ p: UnsafeRawPointer?, _ reader: (UnsafeRawPointer) throws -> T) -> T? {
        guard layoutMatches, let p else { return nil }
        return try? reader(p)
    }

    // MARK: who I am

    /// This device's identity for this conversation (Messages'
    /// localParticipantIdentifier), hashed per game into a seat tag in C.
    public static func me(_ id: Data) {
        id.withUnsafeBytes { raw in
            let p = raw.bindMemory(to: UInt8.self)
            pk_api_me(p.baseAddress, Int32(p.count))
        }
    }

    public static func nickname(_ name: String) {
        let b = Array(name.utf8)
        b.withUnsafeBufferPointer { pk_api_nickname($0.baseAddress, Int32(b.count)) }
    }

    /// PK_NAME_OK (0), EMPTY, TOO_LONG or BAD.
    public static func nameVerdict(_ name: String) -> Int {
        let b = Array(name.utf8)
        return Int(b.withUnsafeBufferPointer { pk_api_name_verdict($0.baseAddress, Int32(b.count)) })
    }

    // MARK: this device's seat records (fixed-layout bytes, kept unread)

    public static func loadSeats(_ bytes: Data?) {
        guard let bytes, !bytes.isEmpty else { pk_api_seats_load(nil, 0); return }
        bytes.withUnsafeBytes { raw in
            let p = raw.bindMemory(to: UInt8.self)
            pk_api_seats_load(p.baseAddress, Int32(p.count))
        }
    }

    /// The records, when a call since the last save changed them.
    public static func seatsIfDirty() -> Data? {
        guard pk_api_seats_dirty() != 0 else { return nil }
        var buf = [UInt8](repeating: 0, count: Int(PK_API_REC_BYTES))
        let n = pk_api_seats_save(&buf, Int32(buf.count))
        return n >= 0 ? Data(buf.prefix(Int(n))) : nil
    }

    // MARK: the resident message

    /// A new lobby with me in seat 0, from the host's secure random.
    @discardableResult
    public static func newGame(dm: Bool) -> Bool {
        var seed = [UInt8](repeating: 0, count: 32)
        guard SecRandomCopyBytes(kSecRandomDefault, seed.count, &seed) == errSecSuccess else { return false }
        return newGame(dm: dm, seed: seed)
    }

    /// The same from a given seed: the tests' and the rig's reproducible deal.
    @discardableResult
    public static func newGame(dm: Bool, seed: [UInt8]) -> Bool {
        guard seed.count == 32 else { return false }
        var s = seed
        return pk_api_new(&s, dm ? 1 : 0) == Int32(PK_EOK)
    }

    /// ADOPT `text`. 0 or a negative PK_E*, and nothing changes on a refusal.
    @discardableResult
    public static func read(_ text: String) -> Int { Int(pk_api_read(text)) }

    /// ADOPT `text` and lay out what it brings (pk_api_adopt, I29): which
    /// bubbles play, from where, and whether a staged play of mine lost a
    /// race is the kernel's comparison of the chain on screen with the one
    /// adopted. The plan is `beatsNow`. 0, or a negative PK_E* and nothing
    /// changed, the playing plan included.
    public static func adopt(_ text: String, arrival: Bool) -> Int { Int(pk_api_adopt(text, arrival ? 1 : 0)) }

    /// Would `text` read? Adopts nothing.
    public static func check(_ text: String) -> Int { Int(pk_api_check(text)) }

    /// The sender fact about exactly this message, or nil to clear it.
    public static func sender(of text: String?, isDM: Bool = false, iSent: Bool = false) {
        guard let text else { pk_api_sender(nil, 0, -1); return }
        pk_api_sender(text, isDM ? 1 : 0, iSent ? 1 : 0)
    }

    /// The resident as the link for MSMessage.url (a draft sealed into a copy;
    /// the draft stays open, so it can still be undone while staged).
    public static var text: String? {
        var buf = [CChar](repeating: 0, count: Int(PK_API_TEXT_MAX))
        let n = pk_api_text(&buf, Int32(buf.count))
        guard n > 0 else { return nil }
        return String(cString: buf)
    }

    /// My staged draft went out: seal it into the resident.
    @discardableResult
    public static func commit() -> Bool { pk_api_commit() == 1 }

    // MARK: the lobby (my new seat, or a negative PK_E*)

    public static func join() -> Int { Int(pk_api_join()) }
    public static func leave() -> Int { Int(pk_api_leave()) }
    public static func start() -> Int { Int(pk_api_start()) }
    public static func joinStart() -> Int { Int(pk_api_join_start()) }

    // MARK: staging my bubble (true if the rules took it)

    public static func draw() -> Bool { pk_api_draw() == 1 }
    /// `suit` 0..3 for a wild that is not my last card, else PK_NO_SUIT.
    public static func play(_ pos: Int, suit: Int = PK_NO_SUIT) -> Bool { pk_api_play(Int32(pos), Int32(suit)) == 1 }
    public static func sayIt() -> Bool { pk_api_say_it() == 1 }
    public static func callOut(_ seat: Int) -> Bool { pk_api_catch(Int32(seat)) == 1 }

    /// What a tap on a seat's fan did (U13, I30): the kernel calls, takes
    /// the call back, or moves it, and a refused move keeps the old call.
    public enum FanTap: Equatable { case refused, called, uncalled, moved }
    public static func tapFan(_ seat: Int) -> FanTap {
        switch Int(pk_api_tap_fan(Int32(seat))) {
        case Int(PK_API_FAN_CALLED): return .called
        case Int(PK_API_FAN_UNCALLED): return .uncalled
        case Int(PK_API_FAN_MOVED): return .moved
        default: return .refused
        }
    }
    public static func pass() -> Bool { pk_api_pass() == 1 }
    public static func undo() -> Bool { pk_api_undo() == 1 }
    public static func unsay() -> Bool { pk_api_unsay() == 1 }
    public static func uncall() -> Bool { pk_api_uncall() == 1 }
    /// Back to the draft's floor (D9): the X on the staged bubble.
    public static func cancel() -> Bool { pk_api_cancel() == 1 }
    public static func canPlay(_ pos: Int) -> Bool { pk_api_can_play(Int32(pos)) == 1 }

    // MARK: my own arrangement of my hand (O9, IOS_DECISIONS I38)

    /// Drag the card drawn at slot `from` to slot `to`. The phone's own order,
    /// kept in its record by the kernel and never sent.
    public static func arrangeMove(from: Int, to: Int) -> Bool { pk_api_arrange_move(Int32(from), Int32(to)) == 1 }
    /// The hand position drawn at `slot` (the kernel's map, never Swift's).
    public static func arrangedPos(_ slot: Int) -> Int? {
        let p = Int(pk_api_arranged_pos(Int32(slot)))
        return p >= 0 ? p : nil
    }
    /// A play at `pos` asks for a suit (a wild that is not the last card).
    public static func isWild(_ pos: Int) -> Bool { pk_api_is_wild(Int32(pos)) == 1 }

    // MARK: reading it

    public static let viewerMe = Int(PK_API_ME)
    public static let viewerAll = Int(PK_API_ALL)
    public static let viewerSpectator = Int(PK_API_SPECTATOR)

    public static func table() -> PkApiTableSnap? {
        snap(pk_api_table(), readPkApiTable)
    }

    /// The masked view: no other seat's count while the game is played (D22).
    public static func view(_ viewer: Int = viewerMe) -> PkViewSnap? {
        snap(pk_api_view(Int32(viewer)), readPkView)
    }

    /// Events of bubbles (from, to], masked for `viewer`; from -1 has the deal.
    public static func plan(_ viewer: Int = viewerMe, from: Int, to: Int) -> [PkEventSnap] {
        snap(pk_api_plan(Int32(viewer), Int32(from), Int32(to)), readPkApiEvents)?.ev ?? []
    }

    /// My open bubble's own events (channel A).
    public static func draftPlan(_ viewer: Int = viewerMe) -> [PkEventSnap] {
        snap(pk_api_plan_draft(Int32(viewer)), readPkApiEvents)?.ev ?? []
    }

    public static func since(from: Int, to: Int) -> PkSinceSnap? {
        snap(pk_api_since(Int32(from), Int32(to)), readPkSince)
    }

    /// The start cards still face up under the deck (D14, U16).
    public static func buried() -> [Int] {
        var out = [UInt8](repeating: 0, count: 8)
        let n = Int(pk_api_buried(&out))
        return out.prefix(n).map(Int.init)
    }

    /// The finished table's order, winner first; empty while it is played.
    public static func ranks() -> [Int] {
        var out = [UInt8](repeating: 0, count: 8)
        let n = Int(pk_api_ranks(&out))
        return out.prefix(n).map(Int.init)
    }

    // MARK: the motion (pk_beats.h): every duration, curve and order is C's

    private static func beatsSnap(_ p: UnsafeRawPointer?) -> PkBeatsSnap? {
        snap(p, readPkBeats)
    }

    /// Channels C, D and E: bubbles (from, to], from the board at the end of
    /// `from`. `open` is a bubble opened (100ms lead), else an arrival (16ms).
    public static func beats(from: Int, to: Int, open: Bool, viewer: Int = viewerMe) -> PkBeatsSnap? {
        beatsSnap(pk_api_beats(Int32(viewer), Int32(from), Int32(to), Int32(open ? PK_BEATS_OPEN : PK_BEATS_ARRIVAL)))
    }

    /// Channel A: what my newest tap did. `picked` is the suit tile tapped
    /// before a wild went down (its ring and collapse lead the plan).
    public static func beatsStage(wildPlaced: Bool = false, picked: Int? = nil) -> PkBeatsSnap? {
        var flags = wildPlaced ? PK_BFL_WILD_PLACED : 0
        if let picked { flags |= PK_BFL_PICKED | ((picked & 3) << 8) }
        return beatsSnap(pk_api_beats_stage(Int32(flags)))
    }

    /// Channel B: after the commit, what staging held.
    public static func beatsSend() -> PkBeatsSnap? { beatsSnap(pk_api_beats_send()) }

    /// A motion no event describes (PK_HM_*).
    public static func beatsHost(_ what: Int, _ a: Int = 0, _ b: Int = 0) -> PkBeatsSnap? {
        beatsSnap(pk_api_beats_host(Int32(what), Int32(a), Int32(b)))
    }

    /// A lost race: my staged card home as a retraction ghost, then (from, to].
    public static func beatsConflict(card: Int, pos: Int, from: Int, to: Int) -> PkBeatsSnap? {
        beatsSnap(pk_api_beats_conflict(Int32(card), Int32(pos), Int32(from), Int32(to)))
    }

    /// The current plan, or nil when the newest build laid nothing out.
    public static func beatsNow() -> PkBeatsSnap? { beatsSnap(pk_api_beats_now()) }

    /// Remember the draft as it is (after a change that moves nothing).
    public static func beatsMark() { pk_api_beats_mark() }
    public static var beatsSerial: Int { Int(pk_api_beats_serial()) }

    /// The current plan's board at `ms`.
    public static func beatFrame(_ ms: Int) -> PkBeatFrameSnap? {
        snap(pk_api_beats_frame(UInt32(max(ms, 0))), readPkBeatFrame)
    }

    /// Beat `i` of the current plan at `ms`, one of its parts.
    public static func beatSample(_ i: Int, part: Int = 0, ms: Int) -> PkBeatSampleSnap? {
        snap(pk_api_beat_sample(Int32(i), Int32(part), UInt32(max(ms, 0))), readPkBeatSample)
    }

    // MARK: the words (every line is the kernel's)

    /// One composed line (PK_API_W_*), "" when the kernel has none to say.
    public static func words(_ what: Int32, _ arg: Int = 0) -> String {
        var buf = [CChar](repeating: 0, count: 1024)
        let n = pk_api_words(what, Int32(arg), &buf, Int32(buf.count))
        return n >= 0 ? String(cString: buf) : ""
    }

    /// The rules page's lines, as many as the kernel has (PK_RULES_N): it
    /// answers -1 past the last, so the host holds no count of its own.
    public static var rules: [String] {
        var out: [String] = []
        var buf = [CChar](repeating: 0, count: 1024)
        while out.count < 64, pk_api_words(PK_API_W_RULE, Int32(out.count), &buf, Int32(buf.count)) >= 0 {
            out.append(String(cString: buf))
        }
        return out
    }

    /// The invitation's caption, for a lobby this device just made.
    public static var inviteCaption: String { words(PK_API_W_INVITE, 0) }

    /// One table entry by its key's NAME ("BTN_DRAW"): the index is the
    /// generated key list's, and the text the kernel's own table.
    public static func string(_ key: String) -> String {
        guard let i = PickemupStringKeys.firstIndex(of: key) else { return "" }
        var buf = [CChar](repeating: 0, count: 512)
        let n = pk_api_string(Int32(i), &buf, Int32(buf.count))
        return n >= 0 ? String(cString: buf) : ""
    }

    // MARK: two messages

    /// <0 mine, >0 the tapped one, 0 the same (4.8).
    public static func prefer(_ mine: String, over tapped: String) -> Int { Int(pk_api_prefer(mine, tapped)) }
    public static func sameGame(_ a: String, _ b: String) -> Bool { pk_api_same_game(a, b) == 1 }
    public static func common(_ a: String, _ b: String) -> Int { Int(pk_api_common(a, b)) }
}
