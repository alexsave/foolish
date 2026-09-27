// Tb.swift - the Swift face of the kernel bridge (tallybones/c/ios/include/tb_api.h),
// in the shape of pickemup/ios/PickemupKit/Kernel/Pk.swift.
//
// SWIFT HOLDS NO RULE. Every question about the game - what is legal, what a
// die shows, what a line says, what a row would score - is one call into C,
// and every struct the kernel hands back is read through the generated
// readers (Generated/TallybonesKernel.swift, structgen), never by offset here.
//
// THE RESIDENT MESSAGE IS ONE SLOT (tb_api.h). `read` and `adopt` ADOPT:
// decoding a bubble replaces whatever was resident. Nothing reads, awaits and
// then stages; every caller reads and acts in one synchronous run on the main
// thread, which is also why this is a caseless enum with no state of its own.
//
// T11: no call here takes a staged move and returns dice for it. The reroll
// of a staged KEEP exists from `markSent` (the send echo) on, or from an
// `adopt` of the sent bubble on another phone.

import CTallybones
import Foundation
import Security

public enum Tb {

    // MARK: the pair

    /// The library and the readers were generated for one layout. A mismatch
    /// is a stale xcframework or a stale Generated/: refuse, never read at a
    /// wrong offset.
    public static let layoutMatches: Bool = tb_api_layout_hash() == SG_LAYOUT_HASH

    /// EVERY GENERATED READ GOES THROUGH HERE, so a mismatched pair reads
    /// nothing at all (nil). Which SCREEN a mismatch shows is
    /// TallybonesHost.readable's; these are two vectors, not two fixes.
    private static func snap<T>(_ p: UnsafeRawPointer?, _ reader: (UnsafeRawPointer) throws -> T) -> T? {
        guard layoutMatches, let p else { return nil }
        return try? reader(p)
    }

    // MARK: who I am

    public static func me(_ id: Data) {
        id.withUnsafeBytes { raw in
            let p = raw.bindMemory(to: UInt8.self)
            tb_api_me(p.baseAddress, Int32(p.count))
        }
    }

    public static func nickname(_ name: String) {
        let b = Array(name.utf8)
        b.withUnsafeBufferPointer { tb_api_nickname($0.baseAddress, Int32(b.count)) }
    }

    /// TB_NAME_OK (0), EMPTY, TOO_LONG or BAD.
    public static func nameVerdict(_ name: String) -> Int {
        let b = Array(name.utf8)
        return Int(b.withUnsafeBufferPointer { tb_api_name_verdict($0.baseAddress, Int32(b.count)) })
    }

    // MARK: this device's seat records (fixed-layout bytes, kept unread)

    public static func loadSeats(_ bytes: Data?) {
        guard let bytes, !bytes.isEmpty else { tb_api_seats_load(nil, 0); return }
        bytes.withUnsafeBytes { raw in
            let p = raw.bindMemory(to: UInt8.self)
            tb_api_seats_load(p.baseAddress, Int32(p.count))
        }
    }

    public static func seatsIfDirty() -> Data? {
        guard tb_api_seats_dirty() != 0 else { return nil }
        var buf = [UInt8](repeating: 0, count: Int(TB_API_REC_BYTES))
        let n = tb_api_seats_save(&buf, Int32(buf.count))
        return n >= 0 ? Data(buf.prefix(Int(n))) : nil
    }

    // MARK: the resident message

    /// A new lobby with me in seat 0, from the host's secure random (T6).
    @discardableResult
    public static func newGame(dm: Bool) -> Bool {
        var seed = [UInt8](repeating: 0, count: 32)
        guard SecRandomCopyBytes(kSecRandomDefault, seed.count, &seed) == errSecSuccess else { return false }
        return newGame(dm: dm, seed: seed)
    }

    /// The same from a given seed: the tests' reproducible game.
    @discardableResult
    public static func newGame(dm: Bool, seed: [UInt8]) -> Bool {
        guard seed.count == 32 else { return false }
        var s = seed
        return tb_api_new(&s, dm ? 1 : 0) == Int32(TB_EOK)
    }

    /// ADOPT `text`: 0, or a negative TB_E* and nothing changed
    /// (TB_ESTAGED: my own staged, unsent bubble).
    @discardableResult
    public static func read(_ text: String) -> Int { Int(tb_api_read(text)) }

    /// ADOPT `text` and lay out what it brings (the plan is `beatsNow`).
    @discardableResult
    public static func adopt(_ text: String, arrival: Bool) -> Int { Int(tb_api_adopt(text, arrival ? 1 : 0)) }

    public static func check(_ text: String) -> Int { Int(tb_api_check(text)) }

    /// The sender fact about exactly this message, or nil to clear it.
    public static func sender(of text: String?, isDM: Bool = false, iSent: Bool = false) {
        guard let text else { tb_api_sender(nil, 0, -1); return }
        tb_api_sender(text, isDM ? 1 : 0, iSent ? 1 : 0)
    }

    /// The link for MSMessage.url: the resident, my staged move as its newest
    /// bubble when there is one.
    public static var text: String? {
        var buf = [CChar](repeating: 0, count: Int(TB_API_TEXT_MAX))
        let n = tb_api_text(&buf, Int32(buf.count))
        guard n > 0 else { return nil }
        return String(cString: buf)
    }

    /// THE SEND ECHO (didStartSending): my staged bubble is resident now and
    /// its roll is derived (T11). Lays out its motion.
    @discardableResult
    public static func markSent() -> Bool { tb_api_mark_sent() == 1 }

    // MARK: the lobby (each changes the resident at once)

    /// My new seat, or a negative TB_E*.
    public static func join() -> Int { Int(tb_api_stage_join()) }
    public static func joinStart() -> Int { Int(tb_api_stage_join_start()) }
    /// 0 in the lobby; in a live game 1 when a LEAVE bubble is staged.
    public static func leave() -> Int { Int(tb_api_stage_leave()) }
    public static func start() -> Int { Int(tb_api_stage_start()) }

    // MARK: staging my bubble (true if the rules took it)

    public static func stageKeep(_ mask: Int) -> Bool { tb_api_stage_keep(Int32(mask)) == 1 }
    public static func stageScore(_ cat: Int) -> Bool { tb_api_stage_score(Int32(cat)) == 1 }
    @discardableResult
    public static func cancel() -> Bool { tb_api_cancel() == 1 }
    public static func canKeep(_ mask: Int) -> Bool { tb_api_can_keep(Int32(mask)) == 1 }
    public static func canScore(_ cat: Int) -> Bool { tb_api_can_score(Int32(cat)) == 1 }
    /// What `cat` scores with the dice as the view shows them, or nil (a die
    /// unknown, the row taken, nobody's turn).
    public static func scoreIf(_ cat: Int) -> Int? {
        let n = Int(tb_api_score_if(Int32(cat)))
        return n >= 0 ? n : nil
    }

    // MARK: reading it

    public static func table() -> TbApiTableSnap? { snap(tb_api_table(), readTbApiTable) }
    public static func view() -> TbViewSnap? { snap(tb_api_view(), readTbView) }

    public static func ranks() -> [Int] {
        var out = [UInt8](repeating: 0, count: 8)
        let n = Int(tb_api_ranks(&out))
        return out.prefix(n).map(Int.init)
    }

    // MARK: the motion (tb_beats.h): every duration, curve and order is C's

    public static func beatsNow() -> TbBeatsSnap? { snap(tb_api_beats_now(), readTbBeats) }
    public static var beatsSerial: Int { Int(tb_api_beats_serial()) }
    public static func beatFrame(_ ms: Int) -> TbBeatFrameSnap? {
        snap(tb_api_beats_frame(UInt32(max(ms, 0))), readTbBeatFrame)
    }
    public static func beatSample(_ i: Int, part: Int = 0, ms: Int) -> TbBeatSampleSnap? {
        snap(tb_api_beat_sample(Int32(i), Int32(part), UInt32(max(ms, 0))), readTbBeatSample)
    }

    // MARK: the words (every line is the kernel's)

    /// One composed line (TB_API_W_*), "" when the kernel has none to say.
    public static func words(_ what: Int32, _ arg: Int = 0) -> String {
        var buf = [CChar](repeating: 0, count: 1024)
        let n = tb_api_words(what, Int32(arg), &buf, Int32(buf.count))
        return n >= 0 ? String(cString: buf) : ""
    }

    /// One table entry by its key's NAME ("BTN_JOIN"): the index is the
    /// generated key list's, the text the kernel's own table.
    public static func string(_ key: String) -> String {
        guard let i = TallybonesStringKeys.firstIndex(of: key) else { return "" }
        var buf = [CChar](repeating: 0, count: 512)
        let n = tb_api_string(Int32(i), &buf, Int32(buf.count))
        return n >= 0 ? String(cString: buf) : ""
    }

    // MARK: two messages

    /// <0 mine, >0 the tapped one, 0 the same.
    public static func prefer(_ mine: String, over tapped: String) -> Int { Int(tb_api_prefer(mine, tapped)) }
    public static func sameGame(_ a: String, _ b: String) -> Bool { tb_api_same_game(a, b) == 1 }
    public static func common(_ a: String, _ b: String) -> Int { Int(tb_api_common(a, b)) }
}
