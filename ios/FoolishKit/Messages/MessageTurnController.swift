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

    public var gameIdString: String { String(gameId) }

    /// Continue a chain I just opened. The resident game may already be this
    /// payload (the view decoded it to resolve my seat); `begin()` re-adopts it
    /// anyway so the controller owns the base unambiguously. `preStaged` are Rule
    /// R survivors (§7.4) to replay on top; `store` is the pending-ledger home.
    public init(parentPayload: Data, parent: MessageEnvelope, mySeat: Int,
                preStaged: [Move] = [], store: MessageGameStore = .shared) {
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
    }

    public var canSend: Bool { !pending.isEmpty }
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
        await rebuildBase()
        pending = []
        for m in preStaged {              // §7.4 survivors, already validated by the rebase
            try? await kernel.apply(seat: mySeat, move: m)
            pending.append(m)
        }
        persistLedger()
        await refresh()
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
