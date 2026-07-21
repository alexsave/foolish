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
    /// False until `begin()` has established the base game once.
    @Published public private(set) var ready = false
    /// True immediately after `undo()` rebuilds the base minus the last
    /// pending action — set BEFORE `refresh()` so a `view` change caused by
    /// undo is distinguishable from a real bout end (note 10) the instant it
    /// happens. Reset at the start of every OTHER state-changing entry point
    /// (`begin`, `apply`), so it only ever describes the MOST RECENT change.
    public private(set) var lastChangeWasUndo = false

    /// note 4/9/38: the log index in the just-adopted chain's replay stream
    /// where the DELTA since the previously-cached chain begins — nil on a
    /// genesis game, a fresh/empty cache, or when the raw hint didn't check
    /// out (> the base's own log count — a genuine anomaly, the cached chain
    /// somehow has MORE logs than the one we just adopted). Note 13: `==` the
    /// base's own log count is NOT collapsed to nil here (it used to be) —
    /// it means "the cached chain and the adopted chain match exactly, I've
    /// already replayed everything", a real, empty delta, which is different
    /// from "no cached chain at all" (the nil case, which falls back to
    /// `openReplayDelta`'s structural heuristic in ReplayDelta.swift).
    /// Computed once in `begin()`.
    public private(set) var openReplayFromLog: Int?
    /// note 36: the real cards that ended up in MY hand purely because of
    /// this delta (a draw, or a pickup that landed in my hand) — found by
    /// diffing my hand in the PREVIOUSLY cached chain against my hand now.
    /// Real identities are not recoverable from the replay stream itself for
    /// an unfinished game (LOG_DRAW/LOG_PICKUP pairs redact anything not yet
    /// publicly played, even from the seat that holds them — see
    /// `residentReplay()`'s doc), so this view-diff is the only clean source.
    public private(set) var openReplayNewHandCards: [Card] = []
    /// notes 6/12: the RESOLVED open-delta replay window — the exact log
    /// slice `MessageTableView.replayLastMoveOnOpen` steps through —
    /// computed HERE, synchronously as part of `begin()`, instead of lazily
    /// inside that replay's own Task after a 120ms sleep. The timing is the
    /// whole point: the view needs every card identity this window touches
    /// (attacks/covers/passes landing on the table, not just cards headed
    /// into my own hand) pre-hidden BEFORE `controller.view`'s first SwiftUI
    /// paint, or a cover renders already-landed-and-rotated for a beat before
    /// its flight "un-rotates" it and lands it again. Empty on a genesis game
    /// (no replay stream to diff against — that path pre-hides straight off
    /// `view.me.hand` instead) or when nothing changed (note 13).
    public private(set) var openReplayEvents: [ReplayLog] = []
    /// notes 6/12: every REAL card identity `openReplayEvents` will land on
    /// the table this open (attacks, covers, cards transferred by a pass) —
    /// the battle-side counterpart to `openReplayNewHandCards` (my hand).
    /// `MessageTableView.replayLastMoveOnOpen` pre-hides the union of both,
    /// synchronously, before the board's first paint.
    public var openReplayTouchedCardIds: Set<String> {
        var ids = Set(openReplayNewHandCards.map(\.identity))
        let placing: Set<Int> = [ReplayLogType.attack, ReplayLogType.cover, ReplayLogType.pass]
        for ev in openReplayEvents where placing.contains(ev.type) {
            for pair in ev.pairs where !pair.primary.isHidden { ids.insert(pair.primary.identity) }
        }
        return ids
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
    /// note 4/9/38: the previously-cached chain for this game, if GameSurface
    /// found one different from the chain we're adopting (nil on a fresh
    /// cache, or a genesis). `begin()` decodes it — with THIS controller's
    /// own resolved `mySeat`, so the hand-diff below is never seat-mismatched
    /// — to derive `openReplayFromLog` / `openReplayNewHandCards`.
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

    /// Start a brand-new game as seat 0 (§5.2 creation). `seed` MUST be 32 bytes
    /// (the wide ChaCha deal both devices reproduce). `gameId` is this game's
    /// random identity; `myNickname` seats me in the joins list.
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

    // MARK: lifecycle

    /// Establish the base game (adopt the parent, or deal the genesis), replay any
    /// Rule R survivors on top, then read the board. Call once from the view's
    /// `.task`.
    public func begin() async {
        lastChangeWasUndo = false
        // note 4/9/38: read the previous chain's log count + my hand BEFORE
        // re-adopting the real base below — decode() IS adopt, so this only
        // works in this order, and rebuildBase() puts the resident game back
        // on the real base afterward, exactly like every other call site
        // expects. Silently skipped if the cached bytes don't even decode.
        var fromLog: Int?
        var prevHandIds: Set<String> = []
        if let prevPayload,
           (try? await kernel.decode(payload: prevPayload, viewer: mySeat)) != nil {
            fromLog = await kernel.residentReplay()?.logs.count
            prevHandIds = Set((await kernel.residentView(viewer: mySeat)?.me?.hand ?? []).map(\.identity))
        }

        await rebuildBase()
        if let f = fromLog {
            let total = await kernel.residentReplay()?.logs.count ?? 0
            // note 13: `<=`, not `<` — `f == total` ("nothing new since I
            // last cached this game") is a real, meaningful delta of zero,
            // not the same as "no info" (see `openReplayFromLog`'s doc).
            openReplayFromLog = (f >= 0 && f <= total) ? f : nil
        }
        pending = []
        for m in preStaged {              // §7.4 survivors, already validated by the rebase
            try? await kernel.apply(seat: mySeat, move: m)
            pending.append(m)
        }
        persistLedger()
        await refresh()
        // Computed AFTER preStaged/refresh so it reflects MY actual final
        // hand, not an intermediate one — preStaged moves are MY OWN unsent
        // re-applications, never something to animate as "arrived."
        if openReplayFromLog != nil {
            openReplayNewHandCards = (view?.me?.hand ?? []).filter { !prevHandIds.contains($0.identity) }
        }
        // notes 6/12: resolve the delta window itself here too, not lazily —
        // see `openReplayEvents`'s doc for why the timing matters.
        if let replay = await kernel.residentReplay() {
            openReplayEvents = openReplayDelta(replay, from: openReplayFromLog,
                                               battlesEmpty: view?.battles.isEmpty ?? true)
        }
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
        } catch {
            rejectTick += 1
        }
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
