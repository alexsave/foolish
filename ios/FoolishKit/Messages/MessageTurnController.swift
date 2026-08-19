// MessageTurnController — the LOCAL half of an iMessage turn (design §5.1, §10).
//
// A turn in this game is not "make a move and it's the other player's turn":
// several seats can be legal at once (§5.1), so the model is *stage a chain*.
// This controller establishes a base game, lets my seat apply one or more legal
// actions locally, and seals the result into a payload the extension inserts as
// the next bubble — which the human, never the code, actually sends (§11.4).
//
// Two ways to start:
//   • continuation — I opened a bubble; adopt its chain and reply (the common case).
//   • genesis — I tapped New game; deal a fresh game where I am seat 0 (§5.2).
// Either way the base is re-establishable from bytes I hold (the parent payload,
// or the deal seed), which is what makes undo free: rebuild the base and replay
// the pending actions minus the undone one (§10) — there is no second copy of
// state to unwind.
//
// It decides NOTHING about rules: every legal-move set and every apply is the
// kernel's answer (via MessageKernel, the single actor over the static Game).

import Foundation
import SwiftUI

@MainActor
public final class MessageTurnController: ObservableObject {
    @Published public private(set) var view: GameView?
    @Published public private(set) var legal: [Move] = []
    @Published public private(set) var pending: [Move] = []
    @Published public private(set) var rejectTick = 0
    /// The reason the LAST move was rejected — an ENGINE_REJECT_* code
    /// (fio_last_reject), mapped to a human line by FStrings.rejectReason. Paired
    /// with `rejectTick`: the tick fires the toast, this says what it reads. 0
    /// (generic) for a non-kernel reject (e.g. a drop that hit no legal target).
    @Published public private(set) var lastRejectReason = 0
    /// False until `begin()` has established the base game once.
    @Published public private(set) var ready = false
    /// True immediately after `undo()` rebuilds the base minus the last
    /// pending action — set BEFORE `refresh()` so a `view` change caused by
    /// undo is distinguishable from a real bout end (note 10) the instant it
    /// happens. Reset at the start of every OTHER state-changing entry point
    /// (`begin`, `apply`), so it only ever describes the MOST RECENT change.
    public private(set) var lastChangeWasUndo = false

    /// notes 6/12 + round-2 #9: the animations to play when this bubble opens -
    /// the LAST move on the adopted chain, as the KERNEL's own viewer-aware
    /// evwire stream (MessageKernel.lastMoveEvents -> fio_replay_last_events_
    /// packed). Resolved HERE in `begin()`, synchronously before the board's
    /// first paint, so the view can pre-hide every card this open will move.
    ///
    /// This REPLACES the old GameView-diff reconstruction (openReplayNewHandCards
    /// / openReplayFromLog / ReplayDelta's LOG_* slicing). That diff could not
    /// recover MY OWN drawn/picked-up cards - the replayed hand looks the same
    /// from the diff's side, and the raw LOG_* stream redacts them even from the
    /// holder - so a reopened pickup animated every OTHER seat's refill but never
    /// mine (the "self deal draw" bug). The kernel, replaying with my seat as the
    /// viewer, hands them over: my cards with real identities, opponents' as backs
    /// (nil in `GameEvent.cards`). Empty when there is nothing to animate. A
    /// genesis deal's "last move" is the deal itself, so this covers it too.
    @Published public private(set) var openReplayEvents: [GameEvent] = []
    /// Every REAL card identity `openReplayEvents` moves onto the table or into
    /// my hand this open (attack/cover/pass placements, my own draws/pickups) -
    /// the set `MessageTableView.replayLastMoveOnOpen` pre-hides synchronously
    /// before the first paint, so a cover never renders already-landed for a beat
    /// (notes 6/12). Opponents' cards are nil (redacted) and need no hiding - they
    /// render as backs regardless.
    public var openReplayTouchedCardIds: Set<String> {
        var ids = Set<String>()
        for ev in openReplayEvents {
            for case let c? in ev.cards { ids.insert(c.identity) }
        }
        return ids
    }

    /// The pre-bout TABLE for a bout-ending open-replay (a pickup or discard),
    /// so the board can lay it out (invisibly) and measure each card's real slot
    /// - making the reopened sweep START from the cards laid out on the table and
    /// fly off it, exactly like watching it live. Without this the pre-bout table
    /// is never rendered on open (`view` is already the cleared board), so every
    /// swept card falls back to one shared board-centre point and they bunch into
    /// a "grouped up" clump before flying (the owner's screenshots). Empty for a
    /// non-bout-end replay (an attack/cover: those cards are still ON the table in
    /// `view`, so they measure themselves).
    ///
    /// Two shapes, matching where the kernel's own event stream keeps the table:
    ///  - discard / trash: the masked `state.battles` carried by the step just
    ///    BEFORE the trash step. A clean defence's turn is grouped from the last
    ///    cover, whose committed board still shows the full covered table
    ///    (verified in MessageEventsTests: the cover/magic steps carry the
    ///    battles, the cardsToTrash step carries the emptied table).
    ///  - pickup: a single-action turn carries no earlier table snapshot, but the
    ///    pickup step's OWN cards ARE the table cards (the kernel never masks a
    ///    pickup, so every viewer gets real identities); lay each in its own
    ///    uncovered slot. This is exactly how the web reconstructs a pickup's
    ///    table for the same animation (AnimationContext: one battle per card).
    public var openReplayPreBattles: [BattleView] {
        let evs = openReplayEvents
        guard let bi = evs.firstIndex(where: {
            $0.kind == .discard || $0.kind == .cardsToTrash || $0.kind == .pickup
        }) else { return [] }
        if evs[bi].kind == .pickup {
            return evs[bi].cards.compactMap { $0 }.map { BattleView(attack: $0, defense: nil) }
        }
        // discard / trash: walk back from the trash step to the last board that
        // still had cards on the table - that is the table about to be swept.
        for i in stride(from: bi, through: 0, by: -1) {
            if let b = evs[i].state?.battles, !b.isEmpty { return b }
        }
        return []
    }

    public let mySeat: Int
    public let names: [Int: String]

    private let kernel = MessageKernel.shared
    private let store: MessageGameStore

    /// The re-establishable base — the bytes the whole game derives from.
    private enum Base {
        case continuation(payload: Data)          // re-adopt this chain
        case genesis(seed: Data, players: Int)    // re-deal this game
    }
    private let base: Base
    private let gameId: UInt64
    private let parent8: Data
    private let joins: [MessageJoin]
    /// The bout the base chain sits at — the round every staged move here is
    /// composed against, and the tag the pending ledger carries for Rule R.
    private let baseRound: Int
    /// Rule R survivors to re-stage on top of the base at `begin()`, in order —
    /// the moves a rebase re-applied onto a freshly-adopted chain (§7.4). Empty on
    /// a plain open or a genesis.
    private let preStaged: [Move]
    /// DEPRECATED (retained only so GameSurface's call sites compile unchanged):
    /// the previously-cached chain, once used to diff my hand for the open-replay.
    /// The open-replay is now the kernel's evwire for the last move
    /// (`openReplayEvents`, resolved from the adopted chain alone), which needs no
    /// "where I last looked", so this is no longer read. Safe to remove along with
    /// its GameSurface threading in a follow-up cleanup.
    private let prevPayload: Data?

    public var gameIdString: String { String(gameId) }

    /// Continue a chain I just opened. The resident game may already be this
    /// payload (the view decoded it to resolve my seat); `begin()` re-adopts it
    /// anyway so the controller owns the base unambiguously. `preStaged` are Rule
    /// R survivors (§7.4) to replay on top; `store` is the pending-ledger home.
    /// `prevPayload` is the previously-cached chain for this game (§ open-delta
    /// replay, notes 4/9/38) — nil skips the delta computation entirely.
    public init(parentPayload: Data, parent: MessageEnvelope, mySeat: Int,
                preStaged: [Move] = [], store: MessageGameStore = .shared,
                prevPayload: Data? = nil) {
        self.base = .continuation(payload: parentPayload)
        self.gameId = UInt64(parent.gameId) ?? 0
        self.parent8 = Self.firstEight(hex: parent.digest)
        self.joins = parent.joins
        self.baseRound = parent.round
        self.preStaged = preStaged
        self.store = store
        self.mySeat = mySeat
        self.names = Dictionary(parent.joins.map { ($0.seat, $0.name) },
                                uniquingKeysWith: { a, _ in a })
        self.prevPayload = prevPayload
    }

    /// Start a brand-new game as seat 0. TEST/HARNESS ONLY since lobby v3:
    /// `startGenesis` — the v2 DM path that dealt a board straight from New
    /// game — is deleted (it let the creator reroll a bad hand), and EVERY
    /// shipping game now begins as a WAITING lobby, DM included, so no
    /// production code constructs a genesis controller any more. Kept because
    /// the turn-mechanics suites drive a controller without a lobby through
    /// it. `players` is any 2-8 — nothing about a genesis is 2-player-shaped;
    /// the suites just happen to use 2. `seed` MUST be 32 bytes (the wide
    /// ChaCha deal); `gameId` is this game's random identity; `myNickname`
    /// seats me in the joins list.
    public init(genesisSeed seed: Data, players: Int, gameId: UInt64, myNickname: String,
                store: MessageGameStore = .shared) {
        self.base = .genesis(seed: seed, players: players)
        self.gameId = gameId
        self.parent8 = Data(repeating: 0, count: 8)   // the root has no parent
        self.joins = [MessageJoin(seat: 0, name: myNickname)]
        self.baseRound = 0
        self.preStaged = []
        self.store = store
        self.mySeat = 0
        self.names = [0: myNickname]
        self.prevPayload = nil
    }

    public var canSend: Bool { !pending.isEmpty }
    /// Is there a sendable bubble right now? Either I've staged a move
    /// (`canSend`), OR it's a fresh genesis where I have no legal move — i.e. I
    /// dealt the game but I'm not the first attacker, so the ONLY way the game
    /// progresses is to send the deal to whoever IS the first attacker. Without
    /// this, a creator who doesn't hold the lowest trump is stuck on a board with
    /// no move and no send (B4 bug: "Start game" left you unable to send).
    public var canStage: Bool { canSend || (isGenesis && !iCanAct) }
    public var iCanAct: Bool { !legal.contains { $0.type == .wait } && !legal.isEmpty }
    public var isOver: Bool { view?.isOver ?? false }
    /// A genesis game with no move yet is not sealable (a 0-action opening is not
    /// a valid FMSG body, MSG_EBODY); continuations always are.
    public var isGenesis: Bool { if case .genesis = base { return true }; return false }
    /// A continuation (I opened someone's bubble) is ALWAYS sealable, even with
    /// zero staged moves — re-sealing the adopted chain is valid FMSG. This is
    /// what lets an undo-to-empty REPLACE its stale staged bubble with the base
    /// state (§10 / 1.0(4)): Apple gives no API to remove an inserted bubble, so
    /// the closest to "cancel the staged move" is to overwrite it with a bubble
    /// that carries nothing new. A genesis with no move is NOT sealable (see the
    /// isGenesis note), so this is false there.
    public var isContinuation: Bool { !isGenesis }

    /// The chain this board is built on (nil for a genesis, which has no chain
    /// yet). What an ARRIVING bubble is Rule-P-compared against so a stale or
    /// duplicate delivery never tears the live board down (GameSurface's
    /// maybeAdoptIncoming).
    public var basePayload: Data? { if case .continuation(let p) = base { return p }; return nil }

    // MARK: lifecycle

    /// Establish the base game (adopt the parent, or deal the genesis), replay any
    /// Rule R survivors on top, then read the board. Call once from the view's
    /// `.task`.
    public func begin() async {
        lastChangeWasUndo = false
        await rebuildBase()
        // The open animations: the kernel's viewer-aware evwire for the LAST move
        // on the adopted chain (notes 6/12/#9). Resolved NOW, after rebuildBase
        // puts the received chain resident but BEFORE re-applying my staged
        // survivors below - "the last move" to animate is the move I just
        // RECEIVED (the chain's final step), never my own Rule R re-applications,
        // which I play interactively and must not re-watch. `prevPayload` is no
        // longer consulted: the kernel decides the group from the chain alone.
        //
        // Note this does NOT depend on who sealed the chain. An earlier pass
        // suppressed the replay when `lastActorSeat == mySeat`, to kill a
        // double animation after sending — wrong cure: opening a chain always
        // shows its last move, mine included ("the replay works fine for the
        // OTHER player when they load the picked-up text, but for the self it
        // doesn't play at all"). The double was the HOST rebuilding this whole
        // surface when I sent — fixed where it happens (HarnessModel.boardEpoch;
        // StagedBubbleRouting in the extension), so there is no second load to
        // replay from in the first place.
        openReplayEvents = await kernel.lastMoveEvents(viewer: mySeat)
        pending = []
        for m in preStaged {              // §7.4 survivors, already validated by the rebase
            try? await kernel.apply(seat: mySeat, move: m)
            pending.append(m)
        }
        persistLedger()
        await refresh()
        // Round-8 #4: opening a FINISHED chain is one of the two moments a game
        // provably ends on this device (the other is committing my own final
        // move, markSent below) - drop its stored hand arrangement, the cache
        // exists only to survive mid-game reopens. Not done at apply(): an
        // unsent final move can still be undone, and the arrangement must
        // survive that undo.
        if view?.isOver == true { store.clearHandOrder(gameId: gameIdString) }
        ready = true
    }

    private func rebuildBase() async {
        switch base {
        case .continuation(let payload):
            _ = try? await kernel.decode(payload: payload, viewer: mySeat)
        case .genesis(let seed, let players):
            try? await kernel.newGame(seed: seed, players: players)
        }
    }

    public func refresh() async {
        view = await kernel.residentView(viewer: mySeat)
        legal = await kernel.residentLegal(seat: mySeat)
    }

    // MARK: turn actions

    public func apply(_ move: Move) async {
        lastChangeWasUndo = false
        do {
            try await kernel.apply(seat: mySeat, move: move)
            pending.append(move)
            persistLedger()
            await refresh()
        } catch MessageEnvelope.Failure.rejected(let reason) {
            lastRejectReason = reason
            rejectTick += 1
        } catch {
            lastRejectReason = 0
            rejectTick += 1
        }
    }

    /// The extension reports my staged chain was actually SENT (the human pressed
    /// Send -> didStartSending). The moves are now in the thread's sent chain, so
    /// they are no longer STAGED: empty the in-memory pending list so `canSend`/
    /// `canUndo` go false and the collapsed view's Undo button disappears.
    ///
    /// Round-6 bug 4: without this, sending left `pending` populated, so the Undo
    /// button lingered in the compact drawer and tapping it re-staged (and let the
    /// human re-send) a move already in the thread. The DURABLE ledger is cleared
    /// separately, at commit (didStartSending -> clearPending); this is the LIVE
    /// half of that same "it's sent now, forget it" signal. Deliberately does NOT
    /// rebuild the base: the sent move stays applied to the resident game, so the
    /// board keeps showing the state I just sent, only without a pending move to
    /// undo or re-send. No-op when nothing is staged (e.g. a genesis with no move).
    public func markSent() async {
        guard !pending.isEmpty else { return }
        pending = []
        lastChangeWasUndo = false
        await refresh()
        // Round-8 #4: the final move is committed to the thread (no undo left),
        // so this game's stored hand arrangement has nothing left to order.
        if view?.isOver == true { store.clearHandOrder(gameId: gameIdString) }
    }

    /// Undo the last staged action by rebuilding the base and replaying all but
    /// the last pending action (§10). No-op if nothing is pending.
    public func undo() async {
        guard !pending.isEmpty else { return }
        let keep = Array(pending.dropLast())
        pending = []
        await rebuildBase()
        for m in keep {
            try? await kernel.apply(seat: mySeat, move: m)
            pending.append(m)
        }
        persistLedger()
        // note 10: set BEFORE refresh() — the state may legally go
        // battles→empty here (undoing the move that opened a bout), and
        // flyBoutEndToDiscard must not mistake that for a real bout end and
        // replay the PREVIOUS bout's draws.
        lastChangeWasUndo = true
        await refresh()
    }

    /// Mirror the in-memory staged list into the durable pending ledger (§17.15)
    /// so a bubble that arrives mid-staging — or a killed extension — can rebase
    /// these moves onto whatever chain wins, rather than dropping them. Tagged
    /// with the bout they were composed against (`baseRound`), the round guard's
    /// key. Sent moves are cleared from the ledger at commit (§7.6).
    private func persistLedger() {
        store.setPending(pending.map { PendingAction(seat: mySeat, round: baseRound, move: $0) },
                         gameId: gameIdString)
    }

    /// The joins to seal: the parent's, plus MY seat if it wasn't named yet — a
    /// joiner appends their own nickname the first time they act (§5.2), so a 2p
    /// opponent stops showing as "Seat 2" once they reply. Seat order preserved.
    var sealJoins: [MessageJoin] {
        if joins.contains(where: { $0.seat == mySeat }) { return joins }
        let mine = MessageJoin(seat: mySeat, name: MessageGameStore.shared.nickname)
        return (joins + [mine]).sorted { $0.seat < $1.seat }
    }

    /// Seal the staged chain into the next bubble's payload. The kernel derives
    /// turn/round from the body it writes, so a device cannot emit a chain it
    /// would itself reject. Phase is FINISHED if my move ended the game, else LIVE.
    /// Throws if nothing is staged on a genesis game (see `isGenesis`).
    public func stagedPayload() async throws -> Data {
        try await kernel.seal(phase: isOver ? 3 : 2,
                              lastActorSeat: mySeat,
                              gameId: gameId,
                              parent8: parent8,
                              joins: sealJoins)
    }

    /// First 8 bytes of a hex digest, zero-padded - the parent-pointer tag (§7.4).
    public static func firstEight(hex: String) -> Data {
        var out = Data(); var i = hex.startIndex
        while out.count < 8, i < hex.endIndex {
            let j = hex.index(i, offsetBy: 2, limitedBy: hex.endIndex) ?? hex.endIndex
            if let b = UInt8(hex[i..<j], radix: 16) { out.append(b) }
            i = j
        }
        while out.count < 8 { out.append(0) }
        return out
    }
}
