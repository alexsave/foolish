// BridgeKernel.swift - the seam's one kernel: CChuiniu (chuiniu/c/ios/include/cn_api.h)
// read into the model the screens draw.
//
// SWIFT HOLDS NO RULE. Every question about the game (what is legal, what a
// seat may see, what a line says, who lost) is one call into C, and every
// struct the kernel hands back is read through the generated readers
// (Generated/ChuiniuKernel.swift, structgen), never by offset here. Where the
// model's shape differs from the kernel's, the mapping is a representation
// and says so (a face with no legal raise is 0 in C and above maxQuantity in
// Menu).
//
// THE RESIDENT GAME IS ONE SLOT (cn_api.h). `cn_api_read` ADOPTS. Nothing
// here reads, awaits and then stages; every caller reads and acts in one
// synchronous run on the main thread.
//
// THIS DEVICE'S SEAT RECORDS AND NICKNAME are kept here, in the extension's
// own defaults as pickemup's are (Release has no App Group), as the kernel's
// fixed-layout bytes and a string: no JSON. In a Debug build `dev.seat`
// switches the whole person (ChuiniuDev.person).

import CChuiniu
import CoreGraphics
import Foundation
import Security

@MainActor
public final class BridgeKernel: Kernel {

    // MARK: the pair

    /// The library and the readers were generated for one layout. A mismatch
    /// is a stale xcframework or a stale Generated/: every read answers nil
    /// and every adopt is refused, never a read at a wrong offset.
    public static let layoutMatches: Bool = cn_api_layout_hash() == SG_LAYOUT_HASH

    private static func snap<T>(_ p: UnsafeRawPointer?, _ reader: (UnsafeRawPointer) throws -> T) -> T? {
        guard layoutMatches, let p else { return nil }
        return try? reader(p)
    }

    // MARK: this device

    private let store: UserDefaults
    /// Whose records are loaded into the kernel (the dev person, or "").
    private var loadedFor: String?
    private var nick = ""

    /// The display's own state, never sent: the committed move count at which
    /// this phone pressed Next round on a reveal (R8 keeps the table revealed
    /// until the opener bids; this phone looks ahead at its own new dice).
    private var lookedAhead: Int?
    /// The newest plan holds a reveal: its LIFT, and where it hands over to
    /// the next round's shake.
    private var revealEndMs: Int?
    public private(set) var motionStart: Date?

    /// Whether the Debug `dev.seat` file may switch the person. Tests say no:
    /// a test host on the rig's simulator shares the rig's App Group.
    private let devPerson: Bool

    public init(store: UserDefaults = .standard, devPerson: Bool = true) {
        self.store = store
        self.devPerson = devPerson
        loadPerson()
    }

    private var person: String {
#if DEBUG
        return devPerson ? ChuiniuDev.person ?? "" : ""
#else
        return ""
#endif
    }

    private func key(_ base: String) -> String { person.isEmpty ? base : "\(base).\(person)" }

    /// Hand the kernel this person's records and nickname, once per person.
    private func loadPerson() {
        let p = person
        guard loadedFor != p else { return }
        // handed over as a Swift array: the bytes are the kernel's to read
        let rec = [UInt8](store.data(forKey: key("chuiniu.seats.v1")) ?? Data())
        cn_api_seats_load(rec, Int32(rec.count))
        loadedFor = p
        var n = store.string(forKey: key("chuiniu.nickname")) ?? ""
#if DEBUG
        if !p.isEmpty { n = p } else if n.isEmpty, devPerson, let dev = ChuiniuDev.nickname { n = dev }
#endif
        setNick(n)
    }

    /// Store what the kernel recorded since the last flush, if anything.
    private func flush() {
        guard cn_api_seats_dirty() != 0 else { return }
        var buf = [UInt8](repeating: 0, count: Int(CN_API_REC_BYTES))
        let n = cn_api_seats_save(&buf, Int32(buf.count))
        if n >= 0 { store.set(Data(buf.prefix(Int(n))), forKey: key("chuiniu.seats.v1")) }
    }

    private func setNick(_ name: String) {
        nick = name
        let b = Array(name.utf8)
        b.withUnsafeBufferPointer { cn_api_nickname($0.baseAddress, Int32(b.count)) }
    }

    public func me(_ participant: Data) {
        loadPerson()
        let id = person.isEmpty ? [UInt8](participant) : Array("dev.seat:\(person)".utf8)
        cn_api_me(id, Int32(id.count))
    }

    public func nickname(_ name: String) {
        guard nameAccepted(name) else { return }
        store.set(name, forKey: key("chuiniu.nickname"))
        setNick(name)
    }

    public var currentNickname: String { nick }

    public func sender(_ url: URL?, isDM: Bool, iSent: Bool) {
        // One simulator playing two people has one real participant, so
        // Messages' sender says nothing about the person in `dev.seat`.
        guard let text = url?.absoluteString, person.isEmpty else { cn_api_sender(nil, 0, -1); return }
        cn_api_sender(text, isDM ? 1 : 0, iSent ? 1 : 0)
    }

    // MARK: words

    private static func line(_ what: Int32, _ arg: Int = 0) -> String {
        var buf = [CChar](repeating: 0, count: 1024)
        let n = cn_api_words(what, Int32(arg), &buf, Int32(buf.count))
        return n >= 0 ? String(cString: buf) : ""
    }

    /// One table entry by its key's NAME: the index is the generated key
    /// list's, the text the kernel's own table.
    static func string(_ key: String) -> String {
        guard let i = ChuiniuStringKeys.firstIndex(of: key) else { return "" }
        var buf = [CChar](repeating: 0, count: 512)
        let n = cn_api_string(Int32(i), &buf, Int32(buf.count))
        return n >= 0 ? String(cString: buf) : ""
    }

    public func word(_ w: Word) -> String {
        switch w {
        case .gameTitle, .lobbyTitle: return Self.string("GAME_NAME")
        case .lobbyWaiting: return Self.string("LOBBY_WAITING")
        case .lobbyFull: return Self.string("LOBBY_FULL")
        case .join: return Self.string("BTN_JOIN")
        case .start: return Self.string("BTN_START")
        case .raise: return Self.string("BTN_RAISE")
        case .call: return Self.string("BTN_CALL")
        case .nextRound: return Self.string("BTN_NEXT")
        case .loses: return Self.string("STAMP_LOSES")
        case .out: return Self.string("STAMP_OUT")
        case .namePrompt: return Self.string("NAME_PROMPT")
        }
    }

    public func errorText(_ code: Int) -> String { Self.line(CN_API_W_ERROR, code) }

#if DEBUG
    /// TESTS ONLY: the kernel's tests-only CN_API_ALL view (every seat's dice),
    /// so a test reaches it through the bridge and never imports CChuiniu
    /// (scripts/lint_architecture.sh). Not in a Release build.
    static func everyonesView() -> CnViewSnap? { snap(cn_api_view(CN_API_ALL), readCnView) }
#endif

    // MARK: the model

    public var table: TableModel {
        guard let t = Self.snap(cn_api_table(), readCnApiTable), t.readable != 0 else { return .empty }
        return Self.model(t, Self.snap(cn_api_view(CN_API_ME), readCnView), lookedAhead: lookedAhead)
    }

    /// The model of one bridge state: the frame, the view and this phone's
    /// look-ahead. A pure mapping, so a test can hand it a real position.
    static func model(_ t: CnApiTableSnap, _ view: CnViewSnap?, lookedAhead: Int?) -> TableModel {
        let me: Int? = t.me == CN_SEAT_NONE ? nil : t.me
        let caption = line(CN_API_W_HEADLINE)
        let bubble = line(CN_API_W_STAGED_CAPTION)

        guard t.phase != CN_PHASE_WAITING, let v = view else {
            let seats = t.seat.indices.map { s in
                SeatModel(id: s, name: line(CN_API_W_SEAT, s), dice: 0, alive: true, isTurn: false,
                          isMe: s == me, lobbyRow: line(CN_API_W_LOBBY_ROW, s))
            }
            return TableModel(phase: .lobby, seats: seats, me: me, myDice: [], bid: nil, bidText: "", bidder: nil,
                              reveal: nil, caption: caption, bubbleCaption: bubble, menu: nil,
                              offered: offer(t.offered), rollID: 0, winner: nil)
        }

        let phase: Phase
        switch v.phase {
        case CN_PH_OVER: phase = .over
        case CN_PH_REVEALED: phase = lookedAhead == t.moves ? .bidding : .revealed
        default: phase = .bidding
        }
        let seats = t.seat.indices.map { s in
            SeatModel(id: s, name: line(CN_API_W_SEAT, s), dice: v.diceN[s], alive: v.diceN[s] > 0,
                      isTurn: v.phase != CN_PH_OVER && v.turn == s, isMe: s == me)
        }
        let bid = v.bidQ > 0 ? Bid(quantity: v.bidQ, face: v.bidF) : nil

        var menu: Menu?
        if v.myTurn != 0 {
            // THE KERNEL'S TABLE, re-spelled: C says 0 for a face with no
            // legal raise, Menu says a number above maxQuantity.
            let maxQ = v.canRaise != 0 ? v.maxQ : v.total
            let byFace = v.minQFace.map { $0 == 0 ? maxQ + 1 : $0 }
            let lowest = v.canRaise != 0 ? Bid(quantity: v.minQ, face: v.minF) : Bid(quantity: maxQ, face: 6)
            menu = Menu(minimumRaise: lowest, minQuantityByFace: byFace, maxQuantity: maxQ,
                        callAllowed: v.canCall != 0)
        }

        var reveal: Reveal?
        if v.revealed != 0 {
            let stride = CN_START_DICE
            let dice = t.seat.indices.map { s in Array(v.shown[(s * stride)..<(s * stride + v.shownN[s])]) }
            let counts = t.seat.indices.map { s in
                v.shownCounts[(s * stride)..<(s * stride + v.shownN[s])].map { $0 != 0 }
            }
            reveal = Reveal(dice: dice, counts: counts, bid: Bid(quantity: v.callQ, face: v.callF),
                            tally: line(CN_API_W_REVEAL_COUNT), outcome: line(CN_API_W_OUTCOME),
                            loser: v.callLoser,
                            nextAllowed: v.phase == CN_PH_REVEALED && me != nil && v.myDice.count > 0)
        }

        let staged = t.staged == CN_API_STAGED_BID ? Bid(quantity: t.stagedQ, face: t.stagedF) : nil
        return TableModel(phase: phase, seats: seats, me: me, myDice: v.myDice, bid: bid,
                          bidText: bid.map(bidWords) ?? "",
                          bidder: bid == nil ? nil : v.bidder, stagedBid: staged,
                          stagedBidText: staged.map(bidWords) ?? "", reveal: reveal, caption: caption,
                          bubbleCaption: bubble, menu: menu, offered: .waiting, rollID: v.round + 1,
                          winner: v.phase == CN_PH_OVER ? v.winner : nil)
    }

    private static func bidWords(_ b: Bid) -> String { line(CN_API_W_BID, b.quantity * 8 + b.face) }

    private static func offer(_ o: Int) -> LobbyOffer {
        switch o {
        case CN_LOBBY_START: return .start
        case CN_LOBBY_JOIN: return .join
        case CN_LOBBY_FULL: return .full
        case CN_LOBBY_INVITE: return .alone
        default: return .waiting
        }
    }

    // MARK: touches

    public func newGame(dm: Bool) -> Bool {
        var seed = [UInt8](repeating: 0, count: 32)
        guard SecRandomCopyBytes(kSecRandomDefault, seed.count, &seed) == errSecSuccess else { return false }
        return newGame(dm: dm, seed: seed)
    }

    /// The same from a given seed: the tests' reproducible dice.
    public func newGame(dm: Bool, seed: [UInt8]) -> Bool {
        guard seed.count == 32 else { return false }
        var s = seed
        let ok = cn_api_new(&s, dm ? 1 : 0) == Int32(CN_EOK)
        settled()
        flush()
        return ok
    }

    public func nameAccepted(_ name: String) -> Bool {
        let b = Array(name.utf8)
        return b.withUnsafeBufferPointer { cn_api_name_verdict($0.baseAddress, Int32(b.count)) } == Int32(CN_NAME_OK)
    }

    public func join(name: String) -> Bool {
        if !name.isEmpty { nickname(name) }
        guard let t = Self.snap(cn_api_table(), readCnApiTable) else { return false }
        // the kernel's verdict: a join that fills the table starts it too
        let seat = t.canJoinStart != 0 ? cn_api_join_start() : cn_api_join()
        flush()
        return seat >= 0
    }

    public func start() -> Bool {
        let ok = cn_api_start() == Int32(CN_EOK)
        flush()
        return ok
    }

    public func raise(quantity: Int, face: Int) -> Bool { cn_api_raise(Int32(quantity), Int32(face)) == 1 }
    public func call() -> Bool { cn_api_call() == 1 }

    public func nextRound() -> Bool {
        guard let t = Self.snap(cn_api_table(), readCnApiTable), t.gamePhase == CN_PH_REVEALED else { return false }
        lookedAhead = t.moves
        return false                                  // a look: nothing to stage
    }

    // MARK: the conversation

    public func adoptBubble(_ url: URL) -> Int {
        guard Self.layoutMatches else { return Int(CN_EFORMAT) }
        let e = Int(cn_api_adopt(url.absoluteString))
        if e == 0 { lookedAhead = nil; began(Self.snap(cn_api_beats_now(), readCnBeats)) }
        flush()
        return e
    }

    public func stagedURL() -> URL? {
        guard let text = Self.text else { return nil }
        return URL(string: text)
    }

    private static var text: String? {
        var buf = [CChar](repeating: 0, count: Int(CN_API_TEXT_MAX))
        let n = cn_api_text(&buf, Int32(buf.count))
        return n > 0 ? String(cString: buf) : nil
    }

    public func keepsStaged(_ url: URL) -> Bool {
        guard let t = Self.snap(cn_api_table(), readCnApiTable), t.staged != CN_API_STAGED_NONE else { return false }
        return Self.text == url.absoluteString
    }

    public func sent(_ url: URL) {
        if keepsStaged(url) {
            // MY MOVE WENT: it joins the resident, and its own motion plays
            // here (a call's reveal is mine to see only now, K8)
            guard cn_api_commit() == 1, let t = Self.snap(cn_api_table(), readCnApiTable) else { return }
            began(Self.snap(cn_api_beats(Int32(t.moves - 1), Int32(t.moves)), readCnBeats))
        } else if Self.text != url.absoluteString {
            // THE SENT BYTES ARE THE AUTHORITY: an older bubble left in the
            // field is the move that happened
            if cn_api_read(url.absoluteString) == 0 { lookedAhead = nil }
            settled()
        }
        flush()
    }

    public func cancelStaged() { _ = cn_api_cancel() }

    public func sameGame(_ a: URL, _ b: URL) -> Bool { cn_api_same_game(a.absoluteString, b.absoluteString) == 1 }

    public func isNewer(_ a: URL, than b: URL) -> Bool { cn_api_prefer(b.absoluteString, a.absoluteString) > 0 }

    // MARK: the motion

    public var stagedSettleMs: Int { Self.snap(cn_api_beats_staged(), readCnBeats)?.totalMs ?? 0 }

    private func settled() {
        revealEndMs = nil
        motionStart = nil
    }

    /// A plan was built: remember when, and whether it lifts the cups.
    private func began(_ beats: CnBeatsSnap?) {
        settled()
        guard let b = beats, b.beat.contains(where: { $0.kind == CN_BK_LIFT }) else { return }
        let lift = b.beat.firstIndex { $0.kind == CN_BK_LIFT } ?? 0
        let shake = b.beat[lift...].first { $0.kind == CN_BK_SHAKE }
        revealEndMs = shake?.startMs ?? b.totalMs
        motionStart = Date()
    }

    public func revealMotion(atMs ms: Int) -> RevealMotion? {
        guard let end = revealEndMs else { return nil }
        guard ms < end, let f = Self.snap(cn_api_beats_frame(UInt32(max(ms, 0))), readCnBeatFrame)
        else { return RevealMotion(cupsUp: true, lit: Int.max, done: true) }
        return RevealMotion(cupsUp: f.cupsUp != 0, lit: f.highlightN, done: false)
    }
}

// MARK: - the stage

/// The bridge's stage (cn_api_stage_*): one a process, as the renderer is.
/// The arena is CN_STAGE_ARENA bytes this class allocates and frees; the
/// texture pack is ChuiniuKit's cn_tex.pack, copied once into memory that
/// outlives the stage. Every struct is read through the generated readers,
/// and a stale pair (BridgeKernel.layoutMatches) reads nothing.
@MainActor
public final class BridgeStage: TableStage {
    public static let shared = BridgeStage()

    private var arena: UnsafeMutableRawPointer?
    private var pack: UnsafeMutablePointer<UInt8>?
    private var packCount = 0
    private var inited = false

    private init() {}

    private static func snap<T>(_ p: UnsafeRawPointer?, _ reader: (UnsafeRawPointer) throws -> T) -> T? {
        guard BridgeKernel.layoutMatches, let p else { return nil }
        return try? reader(p)
    }

    /// The arena and the pack, the first time and after a purge.
    private func ready() -> Bool {
        guard BridgeKernel.layoutMatches else { return false }
        if pack == nil {
            guard let url = Bundle(for: BridgeStage.self).url(forResource: "cn_tex", withExtension: "pack"),
                  let data = try? Data(contentsOf: url) else { return false }
            let p = UnsafeMutablePointer<UInt8>.allocate(capacity: data.count)
            data.copyBytes(to: p, count: data.count)
            pack = p; packCount = data.count
        }
        if arena == nil {
            let a = UnsafeMutableRawPointer.allocate(byteCount: CN_STAGE_ARENA, alignment: 16)
            let rc = inited ? cn_api_stage_attach(a, CN_STAGE_ARENA) : cn_api_stage_init(a, CN_STAGE_ARENA, pack, packCount)
            guard rc == 0 else { a.deallocate(); return false }
            arena = a; inited = true
        }
        return true
    }

    public func begin(_ screen: StageScreen, drawer: CGSize, scale: CGFloat, roll: Bool) -> CnStageHudSnap? {
        guard ready() else { return nil }
        return Self.snap(cn_api_stage_begin(Int32(screen.rawValue), Float(drawer.width), Float(drawer.height), Float(scale), roll ? 1 : 0), readCnStageHud)
    }

    public func frame(atMs ms: Int, peek: Double) -> StageFrame? {
        guard ready(), cn_api_stage_prepare(UInt32(max(ms, 0)), Float(peek)) == 1 else { return nil }
        let bands = CN_STAGE_BANDS
        for pass in 0..<CN_STAGE_PASSES {
            DispatchQueue.concurrentPerform(iterations: bands) { i in
                cn_api_stage_band(Int32(pass), Int32(i), Int32(bands))
            }
        }
        guard let shot = Self.snap(cn_api_stage_shot(), readCnStageShot), shot.ok == 1,
              let px = cn_api_stage_pixels() else { return nil }
        // the pixels are the kernel's until the next prepare: copied out here
        let bytes = Data(bytes: px, count: shot.w * shot.h * 4)
        guard let provider = CGDataProvider(data: bytes as CFData),
              let image = CGImage(width: shot.w, height: shot.h, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: shot.w * 4,
                                  space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.last.rawValue),
                                  provider: provider, decode: nil, shouldInterpolate: true, intent: .defaultIntent)
        else { return nil }
        return StageFrame(shot: shot, image: image)
    }

    public func done(atMs ms: Int) -> Bool { cn_api_stage_done(UInt32(max(ms, 0))) == 1 }

    public func purge() {
        guard let a = arena else { return }
        cn_api_stage_purge()
        a.deallocate()
        arena = nil
    }
}
