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
import IOSurface
import QuartzCore
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

    /// The participant bytes last handed over (devFill puts them back).
    private var meBytes: Data?

    public func me(_ participant: Data) {
        meBytes = participant
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

    /// The bubble's one line, for BubbleLineTests: the kernel's caption for
    /// one act (`CaptionAct`), its width bound in points, and its budget.
    /// (`plateBid` and `tally` are the bubble plate's words, also one line.)
    enum CaptionAct: CaseIterable { case start, bid, call, invite, joined, left, plateBid, tally }
    static func caption(_ act: CaptionAct, who: String, quantity: Int = 0, face: Int = 0) -> String? {
        let what: Int32
        switch act {
        case .start: what = CN_API_P_START
        case .bid: what = CN_API_P_BID
        case .call: what = CN_API_P_CALL
        case .invite: what = CN_API_P_INVITE
        case .joined: what = CN_API_P_JOINED
        case .left: what = CN_API_P_LEFT
        case .plateBid: what = CN_API_P_PLATE_BID
        case .tally: what = CN_API_P_TALLY
        }
        var buf = [CChar](repeating: 0, count: 512)
        let n = cn_api_caption_probe(what, who, Int32(quantity), Int32(face), &buf, Int32(buf.count))
        return n >= 0 ? String(cString: buf) : nil
    }
    static func captionBound(_ line: String) -> Double {
        Double(cn_api_caption_width(line)) / Double(cn_api_caption_unit())
    }
    static var captionBudget: Double { Double(cn_api_caption_budget()) / Double(cn_api_caption_unit()) }

    public func word(_ w: Word) -> String {
        switch w {
        case .gameTitle, .lobbyTitle: return Self.string("GAME_NAME")
        case .lobbyWaiting: return Self.string("LOBBY_WAITING")
        case .lobbyFull: return Self.string("LOBBY_FULL")
        case .join: return Self.string("BTN_JOIN")
        case .start: return Self.string("BTN_START")
        case .leave: return Self.string("BTN_LEAVE")
        case .lobbyYou: return Self.string("LOBBY_YOU")
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
    /// THE RIG'S FULL TABLE (ChuiniuDev.takeFill): a new group lobby with this
    /// device in seat 0, joined by `seats - 1` made-up people exactly as their
    /// phones would join (each its own identity, nickname and records, in a
    /// defaults suite of its own, adopting the newest link, joining, sending),
    /// then started by this device. Every step is a kernel call; the resident
    /// ends as the start, staged. Debug only.
    public func devFill(seats: Int) -> Bool {
        let names = ["Bo", "Cy", "Di", "Ed", "Fay"]
        guard (2...6).contains(seats), let mine = meBytes, newGame(dm: false), var link = stagedURL() else { return false }
        for k in 1..<seats {
            let suite = "chuiniu.dev.fill.\(k)"
            guard let d = UserDefaults(suiteName: suite) else { return false }
            d.removePersistentDomain(forName: suite)
            let p = BridgeKernel(store: d, devPerson: false)
            p.me(Data("dev.fill:\(k)".utf8))
            p.sender(nil, isDM: false, iSent: false)
            guard p.adoptBubble(link) == 0, p.join(name: names[k - 1]), let u = p.stagedURL() else { return false }
            p.sent(u)
            link = u
        }
        // this device again: its own records, nickname and identity
        loadedFor = nil
        loadPerson()
        me(mine)
        sender(nil, isDM: false, iSent: false)
        guard adoptBubble(link) == 0 else { return false }
        return table.phase != .lobby || start()
    }

    /// TESTS ONLY: the kernel's tests-only CN_API_ALL view (every seat's dice),
    /// so a test reaches it through the bridge and never imports CChuiniu
    /// (scripts/lint_architecture.sh). Not in a Release build.
    static func everyonesView() -> CnViewSnap? { snap(cn_api_view(CN_API_ALL), readCnView) }
#endif

    // MARK: the model

    /// A new lobby the kernel refused for want of a name (cn_api_new seats
    /// me under my nickname, and an unaccepted one is none): whether it was
    /// to be a DM. Until a join names me, or another game is adopted or
    /// made, the table is that lobby still to make.
    private var unnamedNew: Bool?

    /// The lobby still to make: nobody seated, and this phone offered Join
    /// with the name field.
    static let unnamedLobby = TableModel(phase: .lobby, seats: [], me: nil, myDice: [], bid: nil, bidText: "",
                                         bidder: nil, reveal: nil, caption: "", bubbleCaption: "", menu: nil,
                                         offered: .join, rollID: 0, winner: nil)

    public var table: TableModel {
        if unnamedNew != nil { return Self.unnamedLobby }
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
            // a lobby's cup carries the dice every seat sits down with (the
            // kernel's CN_START_DICE), as the study's roster does
            let seats = t.seat.indices.map { s in
                SeatModel(id: s, name: line(CN_API_W_SEAT, s), dice: CN_START_DICE, alive: true, isTurn: false,
                          isMe: s == me)
            }
            return TableModel(phase: .lobby, seats: seats, me: me, myDice: [], bid: nil, bidText: "", bidder: nil,
                              reveal: nil, caption: caption, bubbleCaption: bubble, menu: nil,
                              offered: offer(t.offered), mayLeave: t.canExit != 0, rollID: 0, winner: nil)
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
                            nextAllowed: v.phase == CN_PH_REVEALED && me != nil && v.myDice.count > 0,
                            outcomeLoss: line(CN_API_W_OUTCOME_LOSS), outcomeWin: line(CN_API_W_OUTCOME_WIN))
        }

        let staged = t.staged == CN_API_STAGED_BID ? Bid(quantity: t.stagedQ, face: t.stagedF) : nil
        return TableModel(phase: phase, seats: seats, me: me, myDice: v.myDice, bid: bid,
                          bidText: bid.map(bidWords) ?? "",
                          bidder: bid == nil ? nil : v.bidder, stagedBid: staged,
                          stagedBidText: staged.map(bidWords) ?? "", reveal: reveal, caption: caption,
                          bubbleCaption: bubble, menu: menu, offered: .waiting, rollID: v.round + 1,
                          rollPending: cn_api_roll_pending() == 1,
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
        // refused with no name the kernel accepts: the lobby waits for one
        unnamedNew = ok || nameAccepted(nick) ? nil : dm
        pendingSeed = unnamedNew == nil ? nil : seed
        settled()
        flush()
        return ok
    }

    public func nameAccepted(_ name: String) -> Bool {
        let b = Array(name.utf8)
        return b.withUnsafeBufferPointer { cn_api_name_verdict($0.baseAddress, Int32(b.count)) } == Int32(CN_NAME_OK)
    }

    /// The seed of the lobby still to make (`unnamedNew`).
    private var pendingSeed: [UInt8]?

    public func join(name: String) -> Bool {
        if !name.isEmpty { nickname(name) }
        if let dm = unnamedNew, let seed = pendingSeed {
            // the new lobby, now that I have a name
            return newGame(dm: dm, seed: seed)
        }
        guard let t = Self.snap(cn_api_table(), readCnApiTable) else { return false }
        // the kernel's verdict: a join that fills the table starts it too
        let seat = t.canJoinStart != 0 ? cn_api_join_start() : cn_api_join()
        flush()
        return seat >= 0
    }

    public func leave() -> String? {
        guard let t = Self.snap(cn_api_table(), readCnApiTable), t.me != CN_SEAT_NONE else { return nil }
        // captioned while my row is still there (CN_API_W_LEFT)
        let caption = Self.line(CN_API_W_LEFT, t.me)
        let ok = cn_api_leave() == Int32(CN_EOK)
        flush()
        return ok ? caption : nil
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
        if e == 0 { lookedAhead = nil; unnamedNew = nil; began(Self.snap(cn_api_beats_now(), readCnBeats)) }
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

    /// The model's rollID is CnView.round + 1 (the mapping above); the kernel
    /// takes the round. Its record changed: stored now, so a launch that
    /// ends before any other flush still knows.
    public func rollSeen(rollID: Int) {
        guard rollID >= 1 else { return }
        _ = cn_api_roll_seen(Int32(rollID - 1))
        flush()
    }

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
///
/// THE ARENA IS TAKEN BY A FRAME. cn_api_stage_init opens the pack alone and
/// a begin only bakes the layout and the throws into the bridge's handle, so
/// no begin, the first of a process included, takes the arena; the first
/// frame attaches it (cn_api_stage_attach) and a purge frees it. At rest
/// (`rest`) the pages of its frame buffers go back and the textures stay.
///
/// THE STAGE'S OWN QUEUE. Every call that touches the renderer (a begin, a
/// frame, the arena, a purge, the bubble) runs on one serial queue
/// (`StageWorker`), so no two run at once and the arena has one owner. A
/// frame drawn for the display (`submit`) runs there while the main thread
/// goes on; a begin, a purge or a frame asked for at once (`frame`) waits
/// there for the one in flight. What a frame reads of the resident game (the
/// reveal's lift, from the current plan) is sampled on the main thread with
/// the clock, at the submit: the queue reads nothing of the resident.
@MainActor
public final class BridgeStage: TableStage {
    public static let shared = BridgeStage()

    private let worker = StageWorker()
    /// The table or reveal on show, to begin again after a bubble.
    private var onShow: (screen: StageScreen, drawer: CGSize, scale: CGFloat)?

    private init() {}

    private static func snap<T>(_ p: UnsafeRawPointer?, _ reader: (UnsafeRawPointer) throws -> T) -> T? {
        guard BridgeKernel.layoutMatches, let p else { return nil }
        return try? reader(p)
    }

    public var holdsArena: Bool { worker.queue.sync { worker.arena != nil } }

    /// THE COLD OPEN: the pack read and the stage opened on its own queue
    /// while the drawer opens, so the first begin finds it ready (a begin or
    /// a frame meanwhile waits there for it).
    public func warm() {
        let w = worker
        w.queue.async { _ = w.ready(arena: false) }
    }
    public var drawer: CGSize? { onShow?.drawer }

    public func begin(_ screen: StageScreen, drawer: CGSize, scale: CGFloat) -> CnStageHudSnap? {
        guard BridgeKernel.layoutMatches else { return nil }
        if screen != .bubble { onShow = (screen, drawer, scale) }
        // on the queue, after any frame in flight; the main thread waits, so
        // the resident the begin reads cannot change under it
        let w = worker
        #if DEBUG
        ChuiniuDev.launch("stage begin \(screen)")
        defer { ChuiniuDev.launch("stage begin done") }
        #endif
        return w.queue.sync { () -> CnStageHudSnap? in
            guard w.ready(arena: false) else { return nil }
            let hud = Self.snap(cn_api_stage_begin(Int32(screen.rawValue), Float(drawer.width), Float(drawer.height), Float(scale)), readCnStageHud)
            w.begun()
            return hud
        }
    }

    public func bubble(scale: CGFloat) -> BubbleFrame? {
        let back = onShow
        defer {
            // ONE ARENA, AND NONE AFTER THE BUBBLE: the picture is copied out,
            // so the arena goes; the screen on show is begun again as it was
            // (a begin takes no arena), and its next frame takes one
            purge()
            if let b = back { _ = begin(b.screen, drawer: b.drawer, scale: b.scale) }
        }
        let size = CGSize(width: CN_STAGE_BUBBLE_W, height: CN_STAGE_BUBBLE_H)
        guard let hud = begin(.bubble, drawer: size, scale: scale),
              let frame = frame(atMs: 0, peek: 0) else { return nil }
        return BubbleFrame(hud: hud, frame: frame)
    }

    public func frame(atMs ms: Int, peek: Double) -> StageFrame? {
        guard BridgeKernel.layoutMatches else { return nil }
        let t = UInt32(max(ms, 0)), lift = cn_api_stage_lift(t), w = worker
        return w.queue.sync { w.draw(t, Float(peek), lift, banded: true) }
    }

    public func submit(atMs ms: Int, peek: Double, then done: @escaping @MainActor (StageFrame?) -> Void) {
        guard BridgeKernel.layoutMatches else { done(nil); return }
        // the clock, the peek and the lift sampled now, on the main thread
        let t = UInt32(max(ms, 0)), lift = cn_api_stage_lift(t), w = worker
        w.queue.async {
            let f = w.draw(t, Float(peek), lift, banded: true)
            DispatchQueue.main.async { MainActor.assumeIsolated { done(f) } }
        }
    }

    public func frameOnOneThread(atMs ms: Int, peek: Double) -> StageFrame? {
        guard BridgeKernel.layoutMatches else { return nil }
        let t = UInt32(max(ms, 0)), lift = cn_api_stage_lift(t), w = worker
        return w.queue.sync { w.draw(t, Float(peek), lift, banded: false) }
    }

    public func peekEase(_ t: Double) -> Double { Double(cn_api_peek_ease(Float(t))) }

    public func name(seat: Int, bitmap: NameBitmap?) {
        // on the queue, in order with the frames: the next frame asked draws it (the bytes are copied in)
        let w = worker
        w.queue.async {
            guard w.ready(arena: false) else { return }
            if let b = bitmap {
                _ = cn_api_stage_name(Int32(seat), b.rgba, Int32(b.w), Int32(b.h), Float(b.wPt), Float(b.hPt))
            } else {
                _ = cn_api_stage_name(Int32(seat), nil, 0, 0, 0, 0)
            }
        }
    }

    public func done(atMs ms: Int) -> Bool { worker.queue.sync { cn_api_stage_done(UInt32(max(ms, 0))) == 1 } }

    public func purge() {
        let w = worker
        w.queue.sync { w.purge() }
    }

    public func rest() {
        // after any frame in flight; the main thread does not wait
        let w = worker
        w.queue.async { w.rest() }
    }

    /// Bytes of the arena given back at rest, and the surfaces kept (tests).
    var resting: (bytes: Int, surfaces: Int) { worker.queue.sync { (worker.rested, worker.pool.count) } }

    /// How many frames drew at once at most since the last ask (the tests':
    /// the queue is serial, so 1).
    public var drawsAtOnce: Int { worker.queue.sync { worker.takeMostAtOnce() } }
}

/// The stage's queue and what only it touches: the pack, the stage's init,
/// the arena, the picture surfaces. Not on the main actor: every method here
/// runs on `queue`.
///
/// THE PICTURES (package M). The kernel draws each frame straight into an
/// IOSurface (cn_api_stage_external, cn_api_stage_target), which the layer
/// shows as it is: no Data copy, no CGImage, and no copy of Core
/// Animation's own (`prepare_image`'s memmove). A surface a frame was drawn
/// into is that frame's (`StageSurface`): the stage draws into a pooled one
/// only when nothing else holds it (no frame: not the director's last, not a
/// test's, not a bubble's) and the render server no longer reads it
/// (`isInUse`). The director keeps one frame, the one on show, and asks for
/// the next only after it landed, so at most two pictures live: the one on
/// show and the one being drawn (a third only while the render server holds
/// an old one a moment longer).
///
/// THE ARENA AT REST. The arena is the stage's own mapping (mmap), so at rest
/// (`rest`) the pages of the frame's buffers go back to the system
/// (MADV_FREE_REUSABLE) while the textures stay; the next frame takes them
/// again (MADV_FREE_REUSE) and uploads nothing.
final class StageWorker: @unchecked Sendable {
    let queue = DispatchQueue(label: "chuiniu.stage", qos: .userInteractive)
    private(set) var arena: UnsafeMutableRawPointer?
    private var pack: UnsafeMutablePointer<UInt8>?
    private var packCount = 0
    private var inited = false
    /// The kernel draws into surfaces (else, Debug `dev.straight` or a
    /// surface whose row is not the picture's, into the arena, copied out).
    private var external = false
    /// Bytes from the arena's start given back at rest, to take again.
    private(set) var rested = 0
    /// Surfaces made for this screen's pictures; one a frame still holds is
    /// skipped (it is not uniquely the pool's).
    private(set) var pool: [StageSurface] = []
    /// The begun screen's largest picture (cn_api_stage_target_most).
    private var most = (w: 0, h: 0)
    /// frames drawing now, and the most at once (guarded by its own lock: a
    /// count that a queue that was not serial would push past 1)
    private let counting = NSLock()
    private var drawing = 0, mostAtOnce = 0

    /// The pack and the stage's init (once a process: the stage keeps
    /// pointing into the pack; its frames are Core Animation's own form,
    /// premultiplied BGRA, drawn into surfaces); with `arena`, an arena too,
    /// the first frame and the first after a purge, and its pages taken
    /// again after a rest.
    func ready(arena need: Bool) -> Bool {
        dispatchPrecondition(condition: .onQueue(queue))
        if pack == nil {
            guard let url = Bundle(for: StageWorker.self).url(forResource: "cn_tex", withExtension: "pack"),
                  let data = try? Data(contentsOf: url) else { return false }
            let p = UnsafeMutablePointer<UInt8>.allocate(capacity: data.count)
            data.copyBytes(to: p, count: data.count)
            pack = p; packCount = data.count
        }
        if !inited {
            guard cn_api_stage_init(pack, packCount) == 0 else { return false }
            #if DEBUG
            external = !ChuiniuDev.straightFrames
            cn_api_stage_output(ChuiniuDev.straightFrames ? CN_API_STAGE_RGBA : CN_API_STAGE_CA)
            #else
            external = true
            cn_api_stage_output(CN_API_STAGE_CA)
            #endif
            cn_api_stage_external(external ? 1 : 0)
            inited = true
        }
        guard need else { return true }
        if arena == nil {
            // the stage's own mapping, so its pages can be given back at rest
            guard let a = mmap(nil, CN_STAGE_ARENA, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0),
                  a != MAP_FAILED else { return false }
            guard cn_api_stage_attach(a, CN_STAGE_ARENA) == 0 else { munmap(a, CN_STAGE_ARENA); return false }
            arena = a
            rested = 0
        }
        if rested > 0, let a = arena {
            madvise(a, rested, MADV_FREE_REUSE)
            rested = 0
        }
        return true
    }

    /// A screen was begun: its largest picture, which sizes the surfaces.
    func begun() {
        dispatchPrecondition(condition: .onQueue(queue))
        var w: Int32 = 0, h: Int32 = 0
        _ = cn_api_stage_target_most(&w, &h)
        most = (Int(w), Int(h))
    }

    /// The frame at `t`: prepared with the lift sampled at the submit, its
    /// passes in CN_STAGE_BANDS bands over the cores (or on this thread),
    /// drawn into a surface of its own.
    func draw(_ t: UInt32, _ peek: Float, _ lift: Float, banded: Bool) -> StageFrame? {
        dispatchPrecondition(condition: .onQueue(queue))
        counting.lock(); drawing += 1; mostAtOnce = max(mostAtOnce, drawing); counting.unlock()
        defer { counting.lock(); drawing -= 1; counting.unlock() }
        #if DEBUG
        // the frame log's split: taking the arena (and its pages after a rest), the serial prepare (the
        // textures' upload and copies on a first frame), the bands
        let t0 = CACurrentMediaTime()
        guard ready(arena: true) else { return nil }
        let t1 = CACurrentMediaTime()
        let ok = cn_api_stage_prepare_at(t, peek, lift) == 1
        let t2 = CACurrentMediaTime()
        defer {
            let t3 = CACurrentMediaTime()
            ChuiniuDev.log.debug("stage draw ready=\(String(format: "%.2f", (t1 - t0) * 1000), privacy: .public) prepare=\(String(format: "%.2f", (t2 - t1) * 1000), privacy: .public) bands=\(String(format: "%.2f", (t3 - t2) * 1000), privacy: .public)")
        }
        guard ok,
              let p = cn_api_stage_shot(), let shot = try? readCnStageShot(p), shot.ok == 1 else { return nil }
        #else
        guard ready(arena: true), cn_api_stage_prepare_at(t, peek, lift) == 1,
              let p = cn_api_stage_shot(), let shot = try? readCnStageShot(p), shot.ok == 1 else { return nil }
        #endif
        guard external else {
            bands(banded)
            return Self.drawn(shot)
        }
        guard let held = surface(w: shot.w, h: shot.h) else {
            // a surface whose row is not the picture's (never seen): the arena's picture, copied, from now on
            #if DEBUG
            ChuiniuDev.log.error("stage: no surface with \(shot.w * 4) bytes a row; frames are copied from now on")
            #endif
            external = false
            cn_api_stage_external(0)
            return draw(t, peek, lift, banded: banded)
        }
        let s = held.surface
        s.lock(options: [], seed: nil)
        defer { s.unlock(options: [], seed: nil) }
        guard cn_api_stage_target(s.baseAddress, s.allocationSize) == 1 else { return nil }
        bands(banded)
        return StageFrame(shot: shot, surface: held)
    }

    private func bands(_ banded: Bool) {
        let bands = banded ? CN_STAGE_BANDS : 1
        for pass in 0..<CN_STAGE_PASSES {
            DispatchQueue.concurrentPerform(iterations: bands) { i in
                cn_api_stage_band(Int32(pass), Int32(i), Int32(bands))
            }
        }
    }

    /// A surface for a w by h picture: a pooled one of that row, tall enough,
    /// that no frame holds and the render server no longer reads; else a new
    /// one, as tall as the begun screen's tallest picture at this row (so the
    /// throw's changing pad never asks for another).
    private func surface(w: Int, h: Int) -> StageSurface? {
        let row = w * 4
        var keep: [StageSurface] = []
        var found: StageSurface?
        for i in pool.indices {
            let free = isKnownUniquelyReferenced(&pool[i]) && !pool[i].surface.isInUse
            let fits = pool[i].surface.bytesPerRow == row && pool[i].surface.height >= h
            if found == nil, free, fits { found = pool[i]; keep.append(pool[i]); continue }
            // another row or too short, and free: of no more use
            if !fits && free { continue }
            keep.append(pool[i])
        }
        pool = keep
        if let found { return found }
        let tall = most.w > 0 ? Int((Double(most.h) * Double(w) / Double(most.w)).rounded(.up)) + 2 : h
        let rows = max(h, tall)
        guard let s = IOSurface(properties: [.width: w, .height: rows, .bytesPerElement: 4, .bytesPerRow: row,
                                             .allocSize: row * rows, .pixelFormat: Self.bgra]),
              s.bytesPerRow == row else { return nil }
        let made = StageSurface(s)
        pool.append(made)
        return made
    }

    /// 'BGRA': premultiplied, alpha first in a little-endian word (kCVPixelFormatType_32BGRA)
    private static let bgra: UInt32 = 0x4247_5241

    /// At rest: the frame's pages back to the system, the textures kept; every
    /// surface but the one on show let go.
    func rest() {
        dispatchPrecondition(condition: .onQueue(queue))
        dropFree()
        guard let a = arena else { return }
        let r = Int(cn_api_stage_rest()) / Int(vm_page_size) * Int(vm_page_size)
        guard r > 0 else { return }
        madvise(a, r, MADV_FREE_REUSABLE)
        rested = r
    }

    /// Let go of every pooled surface no frame holds (the one on show stays,
    /// held by its frame and its layer).
    private func dropFree() {
        var keep: [StageSurface] = []
        for i in pool.indices where !isKnownUniquelyReferenced(&pool[i]) { keep.append(pool[i]) }
        pool = keep
    }

    func purge() {
        dispatchPrecondition(condition: .onQueue(queue))
        dropFree()
        guard let a = arena else { return }
        cn_api_stage_purge()
        munmap(a, CN_STAGE_ARENA)
        arena = nil
        rested = 0
    }

    func takeMostAtOnce() -> Int {
        counting.lock(); defer { counting.unlock() }
        let m = mostAtOnce
        mostAtOnce = 0
        return m
    }

    /// Core Animation's own form (in a Debug build `dev.straight` asks for the old straight RGBA)
    fileprivate static var form: CGBitmapInfo {
        #if DEBUG
        if ChuiniuDev.straightFrames { return CGBitmapInfo(rawValue: CGImageAlphaInfo.last.rawValue) }
        #endif
        return CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)
    }

    /// The picture the kernel has just drawn into the arena, copied out (Debug
    /// `dev.straight`, and the fallback above).
    private static func drawn(_ shot: CnStageShotSnap) -> StageFrame? {
        guard let px = cn_api_stage_pixels() else { return nil }
        // the pixels are the kernel's until the next prepare: copied out here
        guard let image = StageFrame.image(Data(bytes: px, count: shot.w * shot.h * 4), w: shot.w, h: shot.h) else { return nil }
        return StageFrame(shot: shot, image: image)
    }
}

extension StageFrame {
    /// w by h pixels of Core Animation's form (or Debug's straight RGBA) as an image.
    static func image(_ bytes: Data, w: Int, h: Int) -> CGImage? {
        guard let provider = CGDataProvider(data: bytes as CFData) else { return nil }
        return CGImage(width: w, height: h, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: w * 4,
                       space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: StageWorker.form,
                       provider: provider, decode: nil, shouldInterpolate: true, intent: .defaultIntent)
    }

    /// The top h rows of a surface whose row is w pixels, copied.
    static func copy(_ s: IOSurface?, w: Int, h: Int) -> CGImage? {
        guard let s, s.bytesPerRow == w * 4, s.height >= h else { return nil }
        s.lock(options: .readOnly, seed: nil)
        defer { s.unlock(options: .readOnly, seed: nil) }
        return image(Data(bytes: s.baseAddress, count: w * h * 4), w: w, h: h)
    }

    /// One clear pixel: what `image` is when nothing could be copied.
    static let blank: CGImage = image(Data(count: 4), w: 1, h: 1)!
}
