// BridgeKernel.swift - the TallyKernel seam over the real kernel (Tb, tb_api.h).
//
// One bridge call per question, and every value the screens draw is copied
// out of the generated readers into the plain models (TrayModel, CardModel,
// LobbyModel): the phase, the dice, the keep marks, every card, every verdict
// and every sentence are the kernel's. What this file adds is the mapping,
// never a rule: a category is scored when the kernel's `filled` bit says so,
// the bonus shows once the kernel's `bonus_known` says it can no longer
// change, and a verdict is a bridge call (tb_api_can_keep, tb_api_can_score).
//
// T11: `view()` over a staged KEEP reports the rerolling dice as 0 because the
// kernel's draft view does (tb_view.h); nothing here can hold a value for
// them. They exist from `commit()` (tb_api_mark_sent) on.

import CTallybones
import Foundation

@MainActor
public final class BridgeKernel: TallyKernel {

    public init() {}

    // MARK: identity and records

    public func identify(me participant: Data) { Tb.me(participant) }
    public func sender(of text: String?, isDM: Bool, iSent: Bool) { Tb.sender(of: text, isDM: isDM, iSent: iSent) }
    public func setNickname(_ name: String) { Tb.nickname(name) }
    public func nameIsOK(_ name: String) -> Bool { Tb.nameVerdict(name) == TB_NAME_OK }
    public func loadSeats(_ bytes: Data?) { Tb.loadSeats(bytes) }
    public func seatsIfDirty() -> Data? { Tb.seatsIfDirty() }

    // MARK: the resident

    public var layoutMatches: Bool { Tb.layoutMatches }
    public var text: String? { Tb.text }
    public var hasDraft: Bool { (Tb.table()?.staged ?? 0) != 0 }

    public func adopt(_ text: String, arrival: Bool) -> Int { Tb.adopt(text, arrival: arrival) }
    public func commit() { Tb.markSent() }
    public func cancelDraft() { Tb.cancel() }
    public func newGame(dm: Bool) -> Bool { Tb.newGame(dm: dm) }
    public func sameGame(_ a: String, _ b: String) -> Bool { Tb.sameGame(a, b) }
    public func prefer(_ a: String, over b: String) -> Int { Tb.prefer(a, over: b) }

    // MARK: reads

    public func view() -> TallyView {
        guard let t = Tb.table(), t.readable != 0 else { return TallyView() }
        let seats = t.seat.count
        let me: Int? = t.me == TB_SEAT_NONE ? nil : t.me
        let names = (0..<seats).map { Tb.words(TB_API_W_SEAT, $0) }
        var v = TallyView(readable: true, me: me, names: names)

        if t.phase == TB_PHASE_WAITING {
            v.lobby = LobbyModel(offer: Self.offer(t.offered), canLeave: t.canExit != 0,
                                 rows: (0..<seats).map { Tb.words(TB_API_W_LOBBY_ROW, $0) },
                                 publicRows: (0..<seats).map { Tb.words(TB_API_W_PUBLIC_ROW, $0) }, footnote: nil)
            v.draft = .none
            v.caption = Tb.words(TB_API_W_INVITE, 0)
            return v
        }

        guard let k = Tb.view() else { return TallyView() }
        let over = t.phase == TB_PHASE_FINISHED
        // my turn on the RESIDENT: nothing of mine staged over it
        let mine = t.myTurn != 0 && t.staged == 0
        let canKeep = mine && Tb.canKeep(0)
        let canScore = mine && Category.allCases.contains { Tb.canScore($0.rawValue) }
        v.tray = TrayModel(dice: k.dice,
                           kept: (0..<TrayModel.diceCount).map { k.kept & (1 << $0) != 0 },
                           roll: k.roll,
                           turn: k.turn == TB_SEAT_NONE || over ? nil : k.turn,
                           phase: over ? .over : k.rollsLeft == 0 ? .choosing : .rolling,
                           canKeep: canKeep, canRoll: canKeep, canScore: canScore)
        v.cards = (0..<min(seats, k.seat.count)).map { s in
            let c = k.seat[s]
            return CardModel(seat: s, name: names[s],
                             slots: (0..<Category.count).map { c.filled & (1 << $0) != 0 ? c.score[$0] : nil },
                             numbersSum: c.upper,
                             bonus: c.bonusKnown != 0 ? c.bonus : nil,
                             total: c.total)
        }
        if k.draft != 0 {
            switch k.pendingKind {
            case TB_M_KEEP: v.draft = .keep
            case TB_M_SCORE: v.draft = Category(rawValue: k.pendingArg).map { .score($0) } ?? .none
            default: v.draft = .lobby
            }
        }
        v.headline = Tb.words(TB_API_W_HEADLINE)
        v.rollLine = Tb.words(TB_API_W_SUBLINE)
        v.caption = Tb.words(TB_API_W_STAGED_CAPTION)
        return v
    }

    private static func offer(_ o: Int) -> LobbyModel.Offer {
        switch o {
        case TB_LOBBY_START:   return .start
        case TB_LOBBY_INVITE:  return .invite
        case TB_LOBBY_WAITING: return .waiting
        case TB_LOBBY_JOIN:    return .join
        case TB_LOBBY_FULL:    return .full
        default:               return .none
        }
    }

    public func preview() -> [Int?] {
        Category.allCases.map { Tb.scoreIf($0.rawValue) }
    }

    public func string(_ s: TallyString) -> String {
        switch s {
        case .lobbyTitle:   return Tb.string("GAME_NAME")
        case .join:         return Tb.string("BTN_JOIN")
        case .start:        return Tb.string("BTN_START")
        case .leave:        return Tb.string("BTN_LEAVE")
        case .lobbyWaiting: return Tb.string("LOBBY_WAITING")
        case .lobbyAlone:   return Tb.string("LOBBY_ALONE")
        case .lobbyFull:    return Tb.string("LOBBY_FULL")
        case .namePrompt:   return Tb.string("LOBBY_NAME_PROMPT")
        case .unreadable:   return Tb.string("UNREADABLE")
        case .roll:         return Tb.string("BTN_REROLL")
        case .close:        return Tb.string("BTN_CLOSE")
        case .numbersSum:   return Tb.words(TB_API_W_ROW_LABEL, TB_ROW_UPPER)
        case .bonus:        return Tb.words(TB_API_W_ROW_LABEL, TB_ROW_BONUS)
        case .total:        return Tb.words(TB_API_W_ROW_LABEL, TB_ROW_TOTAL)
        }
    }

    public func categoryName(_ c: Category) -> String { Tb.words(TB_API_W_CAT, c.rawValue) }
    public func errorLine(_ code: Int) -> String { Tb.words(TB_API_W_ERROR, code) }
    public var inviteCaption: String { Tb.words(TB_API_W_INVITE, 0) }

    // MARK: moves

    public func mayRoll(keeping mask: Int) -> Bool { !hasDraft && Tb.canKeep(mask) }

    public func roll(keeping mask: Int) -> TallyStage? {
        guard Tb.stageKeep(mask) else { return nil }
        return TallyStage(caption: Tb.words(TB_API_W_STAGED_CAPTION), collapse: true)      // T55
    }

    public func score(_ c: Category) -> TallyStage? {
        guard Tb.stageScore(c.rawValue) else { return nil }
        return TallyStage(caption: Tb.words(TB_API_W_STAGED_CAPTION), collapse: true)      // T55
    }

    /// Join, or join and start when my join fills the table (pickemup 4.6.5).
    public func join() -> TallyStage? {
        let fills = (Tb.table()?.canJoinStart ?? 0) != 0
        let seat = fills ? Tb.joinStart() : Tb.join()
        guard seat >= 0 else { return nil }
        if fills { Tb.beats(from: -1, to: 0, mode: TB_BEATS_SEND) }         // my tap started it: roll 1 settles
        return TallyStage(caption: fills ? Tb.words(TB_API_W_STAGED_CAPTION) : Tb.words(TB_API_W_JOINED, seat),
                          collapse: false)
    }

    public func leave() -> TallyStage? {
        guard let t = Tb.table(), t.me != TB_SEAT_NONE else { return nil }
        if t.phase == TB_PHASE_WAITING {
            let caption = Tb.words(TB_API_W_LEFT, t.me)       // captioned while the row is still there
            guard Tb.leave() == TB_EOK else { return nil }
            return TallyStage(caption: caption, collapse: false)
        }
        guard Tb.leave() == 1 else { return nil }             // a LEAVE bubble (T13)
        return TallyStage(caption: Tb.words(TB_API_W_STAGED_CAPTION), collapse: true)
    }

    public func start() -> TallyStage? {
        guard Tb.start() == TB_EOK else { return nil }
        Tb.beats(from: -1, to: 0, mode: TB_BEATS_SEND)                      // my tap started it: roll 1 settles
        return TallyStage(caption: Tb.words(TB_API_W_STAGED_CAPTION), collapse: false)
    }
}
