// TableModel.swift - what the table screen shows, and what a touch on it does.
//
// EVERY TOUCH IS ONE KERNEL CALL (Pk), and everything drawn is read back from
// the kernel afterwards (`refresh`): the masked view, the frame, the draft's
// own events, the words. This file keeps only what is the SCREEN's and not the
// game's: which card is selected, whether the suit picker is up, a toast, and
// the brief "drawn cards stay" line (U23). It decides no rule: whether a card
// plays, whether a draw may come back, whether a bubble can be sent, which
// pill stands where - each is the kernel's answer.
//
// It does not know there is a conversation. It says "stage this" (`onStage`)
// with the kernel's caption and whether the drawer should collapse after,
// and MessagesViewController does the rest.

import CPickemup
import Combine
import SwiftUI

@MainActor
public final class TableModel: ObservableObject {

    /// A request to put the resident bubble into the input field.
    public struct Stage: Equatable {
        public let caption: String
        /// The drawer collapses by itself once the move has rested (foolish's
        /// 250ms + settle + 500ms): a play, a pass, a lone Last card!.
        public let collapse: Bool
        /// When it may: the kernel's settle for the plan the move started
        /// (PkBeats.settle_ms), in milliseconds from the stage.
        public let settleMs: Int
        public init(caption: String, collapse: Bool, settleMs: Int = PK_T_COLLAPSE_WAIT + PK_T_COLLAPSE_REST) {
            self.caption = caption
            self.collapse = collapse
            self.settleMs = settleMs
        }
    }

    /// The motion: the kernel's timeline for every touch and every arrival.
    public let player = BeatPlayer()

    @Published public private(set) var table: PkApiTableSnap?
    @Published public private(set) var view: PkViewSnap?
    /// My open bubble's own events (channel A), for the staged-turn strip.
    @Published public private(set) var draft: [PkEventSnap] = []
    /// What the newest sealed bubble did (the Caught you! / Wrong call stamps).
    @Published public private(set) var newest: PkSinceSnap?
    @Published public private(set) var names: [String] = []
    @Published public private(set) var buried: [Int] = []

    /// The hand position a tap selected (foolish's red ring; U9's Play).
    @Published public var selected: Int?
    /// The wild waiting for its suit (U14): nothing is staged until it has one.
    @Published public private(set) var pickerFor: Int?
    @Published public private(set) var toast: String?
    /// U23: an undo the floor refused, shown as SUB_DRAWN_STAY for a moment.
    @Published public private(set) var drawnStay = false

    public var onStage: ((Stage) -> Void)?

    public init() { refresh() }

    // MARK: reading

    /// Read everything back from the resident. Called after every touch, and
    /// by the controller after it adopts a bubble.
    public func refresh() {
        let t = Pk.table()
        table = t
        let live = (t?.phase ?? PK_PHASE_WAITING) != PK_PHASE_WAITING
        view = live ? Pk.view() : nil
        draft = live && (t?.draft ?? 0) != 0 ? Pk.draftPlan() : []
        let bubbles = t?.bubbles ?? 0
        newest = live && bubbles > 0 ? Pk.since(from: bubbles - 1, to: bubbles) : nil
        names = (0..<(t?.seat.count ?? 0)).map { Pk.words(PK_API_W_SEAT, $0) }
        buried = live ? Pk.buried() : []
        if let s = selected, s >= (view?.myHand.count ?? 0) { selected = nil }
        if let p = pickerFor, p >= (view?.myHand.count ?? 0) { pickerFor = nil }
    }

    public var phase: Int { table?.phase ?? PK_PHASE_WAITING }
    public var seatCount: Int { table?.seat.count ?? 0 }
    public var me: Int? { table.flatMap { $0.me == PK_SEAT_NONE ? nil : $0.me } }
    public var isOver: Bool { (view?.over ?? 0) != 0 }
    public var myTurn: Bool {
        guard let v = view, v.over == 0, v.me != PK_SEAT_NONE else { return false }
        return v.turn == v.me
    }
    public var hand: [Int] { view?.myHand ?? [] }

    public var headline: String {
        if pickerFor != nil { return Pk.string("HEAD_PICK_SUIT") }
        return Pk.words(PK_API_W_HEADLINE)
    }
    public var subline: String {
        if pickerFor != nil { return "" }
        if drawnStay { return Pk.string("SUB_DRAWN_STAY") }
        return Pk.words(PK_API_W_SUBLINE)
    }
    public var deckLeft: String { Pk.words(PK_API_W_DECK_LEFT) }
    public var direction: String { (view?.showDir ?? 0) != 0 ? Pk.words(PK_API_W_DIR) : "" }

    /// Is the card at `pos` dimmed? Not my turn, or it does not play: a hint,
    /// never a lock (UI.html "A card that does not match").
    public func dimmed(_ pos: Int) -> Bool {
        guard myTurn, let v = view, pos < v.myPlayable.count else { return true }
        return v.myPlayable[pos] == 0
    }

    /// U9, from the kernel's layout.
    public var pills: (trailing: PkLayout.Pill, leading: PkLayout.Pill) {
        guard let v = view, pickerFor == nil, v.over == 0 else { return (.none, .none) }
        return PkLayout.pills(canDraw: v.canDraw != 0, myTurn: myTurn, selected: selected != nil,
                              canPass: v.canPass != 0, canUndo: v.canUndo != 0)
    }

    /// The Last card! pill (U11): I may say it now, and have not in this bubble.
    public var maySay: Bool { (view?.myExposed ?? 0) != 0 && (view?.draftSaid ?? 0) == 0 }

    // MARK: the board as shown: the settled view, or a plan's frame

    /// What the board draws: the kernel's settled view, or while a plan
    /// plays, the frame of it (pk_beats_frame). Nothing here is computed.
    public struct Shown: Equatable {
        public var deckN = 0
        public var top: Int?
        public var stackN = 0
        public var suit = 0
        public var dir = PK_DIR_CW
        public var turn: Int?
        public var hand: [Int] = []
        public var unseen: Set<Int> = []
        public var stampHold = 0
        public var fansEmpty = 0
        public var buriedHold = 0
        /// Per seat, how many backs have turned at the end reveal; nil before
        /// a reveal begins (or with no plan: every card of a finished table).
        public var revealShown: [Int]?
        public var hold = 0
        public var playing = false

        public func holds(_ h: Int) -> Bool { hold & h != 0 }
        public func stampHeld(_ seat: Int) -> Bool { stampHold & (1 << seat) != 0 }
        public func fanEmpty(_ seat: Int) -> Bool { fansEmpty & (1 << seat) != 0 }
    }

    public func shown(_ frame: PkBeatFrameSnap?) -> Shown {
        var s = Shown()
        if let f = frame {
            s.deckN = f.deckN
            s.top = f.top == PK_CARD_NONE ? nil : f.top
            s.stackN = f.stackN
            s.suit = f.suit
            s.dir = f.dir
            s.turn = f.turn == PK_SEAT_NONE ? nil : f.turn
            s.hand = f.myHand
            s.unseen = Set(f.myUnseen.enumerated().filter { $0.element != 0 }.map(\.offset))
            s.stampHold = f.stampHold
            s.fansEmpty = f.fansEmpty
            s.buriedHold = f.buriedHold
            s.revealShown = f.revealing != 0 ? f.revealShown : nil
            s.hold = f.hold
            s.playing = true
            return s
        }
        guard let v = view else { return s }
        s.deckN = v.deckN
        s.top = v.top == PK_CARD_NONE ? nil : v.top
        s.stackN = v.stackN
        s.suit = v.liveSuit
        s.dir = v.dir
        s.turn = v.over == 0 ? v.turn : nil
        s.hand = v.myHand
        return s
    }

    // MARK: the staged-turn strip (U10): one chip per kind of thing, counted

    public struct Strip: Equatable {
        public var draws = 0
        public var reshuffled = false
        public var played: Int?
        public var chosen: Int?
        public var said = false
        public var called: Int?
        public var isEmpty: Bool { draws == 0 && !reshuffled && played == nil && !said && called == nil }
    }

    public var strip: Strip {
        var s = Strip()
        guard let me else { return s }
        for e in draft {
            switch e.kind {
            case PK_EV_DRAW where e.seat == me: s.draws += 1
            case PK_EV_RESHUFFLE_SHUFFLE: s.reshuffled = true
            case PK_EV_PLAY where e.seat == me: s.played = e.card
            case PK_EV_WILD_SUIT where e.seat == me: s.chosen = e.suit
            case PK_EV_SAY_IT where e.seat == me: s.said = true
            case PK_EV_CALL_OUT where e.other == me: s.called = e.seat
            default: break
            }
        }
        return s
    }

    // MARK: stamps under a badge (U12): speech and verdicts only

    public enum Stamp: Equatable { case last, caught, wrong, out }

    public func stamp(_ seat: Int) -> Stamp? { Self.stamp(seat, view: view, newest: newest) }

    /// OUT once it is over; else the newest bubble's verdict on this seat
    /// (Caught you! on the caught, Wrong call on the caller); else LAST while
    /// the seat has said it and holds that one card (the kernel's `said`).
    public static func stamp(_ seat: Int, view: PkViewSnap?, newest: PkSinceSnap?) -> Stamp? {
        guard let v = view else { return nil }
        if v.over != 0 { return v.winner == seat ? .out : nil }
        if let n = newest, n.caught == seat { return .caught }
        if let n = newest, n.wrong == seat { return .wrong }
        if v.said & (1 << seat) != 0 { return .last }
        return nil
    }

    public func mayCall(_ seat: Int) -> Bool { (view?.canCall ?? 0) & (1 << seat) != 0 }
    public func calling(_ seat: Int) -> Bool {
        guard let v = view, v.draftOpen != 0 else { return false }
        return v.draftCall == seat
    }

    // MARK: touches

    /// Tap the deck, drop a card dragged off it in the hand, or press Draw:
    /// one kernel action, DRAW. No collapse and no stage: a turn with a draw
    /// in it cannot be sealed until it plays or passes (pk_can_seal).
    public func draw() {
        guard Pk.draw() else { Haptics.fire(.reject); return }
        Haptics.fire(.pickUp)
        drawnStay = false
        refresh()
        player.play(Pk.beatsStage())
        stageIfSendable(collapse: false)
    }

    /// A tap only selects (UI.html "Two ways to throw"); again, or another
    /// card, moves or drops it.
    public func tap(_ pos: Int) {
        guard pickerFor == nil else { return }
        selected = selected == pos ? nil : pos
    }

    /// Play the card at `pos`: dropped on the pile, or selected then Play.
    public func play(_ pos: Int) {
        guard pickerFor == nil, pos < hand.count else { return }
        guard Pk.canPlay(pos) else { reject(); return }
        selected = nil
        if Pk.isWild(pos) {
            pickerFor = pos                    // U14: nothing staged until a suit
            player.play(Pk.beatsHost(PK_HM_PICKER_OPEN, hand[pos], pos))
            return
        }
        guard Pk.play(pos) else { reject(); return }
        Haptics.fire(.drop)
        refresh()
        player.play(Pk.beatsStage())
        stageIfSendable(collapse: true)
    }

    public func playSelected() {
        guard let s = selected else { return }
        play(s)
    }

    /// The suit picker's tile: only now is the wild played (D17).
    public func choose(_ suit: Int) {
        guard let pos = pickerFor else { return }
        pickerFor = nil
        guard Pk.play(pos, suit: suit) else { reject(); refresh(); return }
        Haptics.fire(.drop)
        refresh()
        player.play(Pk.beatsStage(wildPlaced: true, picked: suit))
        stageIfSendable(collapse: true)
    }

    /// The x or the scrim: the card goes home and nothing was staged.
    public func cancelPicker() {
        guard let pos = pickerFor else { return }
        pickerFor = nil
        player.play(pos < hand.count ? Pk.beatsHost(PK_HM_PICKER_CANCEL, hand[pos], pos) : nil)
    }

    public func pass() {
        guard Pk.pass() else { Haptics.fire(.reject); return }
        refresh()
        player.play(Pk.beatsStage())
        stageIfSendable(collapse: true)
    }

    /// The Undo pill. Drawn cards never come back (D8): the kernel refuses
    /// below the draft's floor, and the screen says so (U23).
    public func undo() {
        let played = stagedPlay
        guard Pk.undo() else { refusedBelowFloor(); return }
        refresh()
        // grid "Undo a staged card": the card flies home; anything else that
        // came back moves nothing
        if let played, played.pos < hand.count, hand[played.pos] == played.card {
            player.play(Pk.beatsHost(PK_HM_UNDO, played.card, played.pos))
        } else {
            player.clear()
            Pk.beatsMark()
        }
        stageIfSendable(collapse: false)
    }

    /// My staged play, if the draft has one: the card and where it came from.
    public var stagedPlay: (card: Int, pos: Int)? {
        guard let me, let e = draft.last(where: { $0.kind == PK_EV_PLAY && $0.seat == me }) else { return nil }
        return (e.card, e.i)
    }

    public func sayIt() {
        guard Pk.sayIt() else { Haptics.fire(.reject); return }
        refresh()
        player.play(Pk.beatsStage())
        let alone = strip.played == nil && strip.draws == 0 && strip.called == nil
        stageIfSendable(collapse: alone)
    }

    /// The strip's "You say" chip: un-say while the bubble is open.
    public func unsay() {
        guard Pk.unsay() else { return }
        refresh()
        Pk.beatsMark()                         // un-say moves nothing (grid "Un-say")
        stageIfSendable(collapse: false)
    }

    /// Tap a fan (U13): stage Caught you! on that seat, tap it again to
    /// un-call, another to move the call. The verdict is never previewed.
    public func tapFan(_ seat: Int) {
        if calling(seat) {
            guard Pk.uncall() else { return }
            refresh()
            // grid "Un-call": the ring and the tip fade off; a staged turn's
            // held settle keeps holding
            player.play((player.plan?.held ?? 0) > 0 ? Pk.beatsStage() : Pk.beatsHost(PK_HM_UNCALL, seat))
        } else {
            guard mayCall(seat) || (view?.draftCall ?? PK_SEAT_NONE) != PK_SEAT_NONE else { return }
            if (view?.draftCall ?? PK_SEAT_NONE) != PK_SEAT_NONE { _ = Pk.uncall() }
            guard Pk.callOut(seat) else { refresh(); Haptics.fire(.reject); return }
            Haptics.fire(.pickUp)
            refresh()
            player.play(Pk.beatsStage())
        }
        stageIfSendable(collapse: false)
    }

    /// Messages' own X on the staged bubble: back to the floor, not the
    /// parent (D9). The draws stay, and the line says so.
    public func cancelStaged() {
        let hadDraws = strip.draws > 0
        _ = Pk.cancel()
        selected = nil
        pickerFor = nil
        refresh()
        player.clear()
        Pk.beatsMark()
        if hadDraws { showDrawnStay() }
    }

    // MARK: the lobby

    /// Join (and start, when my join fills the table, 4.6.5).
    public func join() {
        let fills = (table?.canJoinStart ?? 0) != 0
        let seat = fills ? Pk.joinStart() : Pk.join()
        guard seat >= 0 else { Haptics.fire(.reject); refresh(); return }
        refresh()
        onStage?(Stage(caption: fills ? Pk.words(PK_API_W_STAGED_CAPTION) : Pk.words(PK_API_W_JOINED, seat),
                       collapse: false))
        if fills { onDealt?() }
    }

    /// My tap started the game (Start, or the Join that filled the table):
    /// the table takes over from the lobby and the deal plays (grid "Start").
    public var onDealt: (() -> Void)?

    public func leave() {
        guard let me else { return }
        let caption = Pk.words(PK_API_W_LEFT, me)       // captioned while the row is still there
        guard Pk.leave() == PK_EOK else { Haptics.fire(.reject); return }
        refresh()
        onStage?(Stage(caption: caption, collapse: false))
    }

    public func start() {
        guard Pk.start() == PK_EOK else { Haptics.fire(.reject); refresh(); return }
        refresh()
        onStage?(Stage(caption: Pk.words(PK_API_W_STAGED_CAPTION), collapse: false))
        onDealt?()
    }

    /// Again, on a finished table: a new lobby in the same chat shape.
    public func again() {
        let dm = (table?.dm ?? 0) != 0
        guard Pk.newGame(dm: dm) else { return }
        refresh()
        onStage?(Stage(caption: Pk.words(PK_API_W_INVITE, 0), collapse: false))
    }

    // MARK: -

    private func stageIfSendable(collapse: Bool) {
        guard (table?.canSend ?? 0) != 0 else { return }
        let settle = player.plan?.settleMs ?? PK_T_COLLAPSE_WAIT + PK_T_COLLAPSE_REST
        onStage?(Stage(caption: Pk.words(PK_API_W_STAGED_CAPTION), collapse: collapse, settleMs: settle))
    }

    /// My staged bubble went out and was sealed (channel B): what staging
    /// held plays now.
    public func sent() { player.play(Pk.beatsSend()) }

    private func reject() {
        Haptics.fire(.reject)
        toast = Pk.string("TOAST_NO_MATCH")
        let shown = toast
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.6) { [weak self] in
            if self?.toast == shown { self?.toast = nil }
        }
    }

    private func refusedBelowFloor() {
        Haptics.fire(.reject)
        showDrawnStay()
        // U23: the newest drawn card shakes and stays
        if !hand.isEmpty, (player.plan?.held ?? 0) == 0 {
            player.play(Pk.beatsHost(PK_HM_REFUSED, 0, hand.count - 1))
        }
    }

    private func showDrawnStay() {
        drawnStay = true
        DispatchQueue.main.asyncAfter(deadline: .now() + 2.4) { [weak self] in self?.drawnStay = false }
    }
}
