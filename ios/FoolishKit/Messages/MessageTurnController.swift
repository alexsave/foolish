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
    /// ROUND 16 — seconds this seat must still wait before it may pick up; 0
    /// means now. The board hides Take while it is non-zero and `apply` refuses
    /// a pickup, so the owner's "guarded by the kernel, as well as by the UI"
    /// is one number read in two places. Counted down by `holdTicker`, which
    /// exists so the button can APPEAR on its own — a defender who is waiting is
    /// looking straight at it, and a UI that only updated on the next kernel
    /// call would leave them looking at nothing.
    @Published public private(set) var pickupHold: Int = 0
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
    /// the move THIS BUBBLE carries (its own atoms, by the round-16 bubble
    /// delta: `baseAtomsBefore`), as the KERNEL's own viewer-aware evwire stream
    /// (MessageKernel.lastMoveEvents -> fio_replay_last_events_packed).
    /// Resolved HERE in `begin()`, synchronously before the board's first
    /// paint, so the view can pre-hide every card this open will move.
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
    public var openReplayPreBattles: [BattleView] { Self.preBoutTable(openReplayEvents) }

    /// The rule above, as a pure function of ANY event stream.
    ///
    /// ROUND 16 lifted it out of the property because the LIVE bout-end needs
    /// the same answer: when a cover ends the bout in the same kernel apply, the
    /// board's own prior view is the table WITHOUT that cover on it (the view
    /// went straight from "uncovered attack" to "empty"), so a sweep built from
    /// it shows a table missing the card the player just played. The kernel's
    /// stream is the only place the covered table exists, live or on open, and
    /// there must be exactly one reading of it.
    public static func preBoutTable(_ evs: [GameEvent]) -> [BattleView] {
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
    /// Seat -> nickname, from the chain's joins. A `var` since round-12: a chain
    /// that ARRIVES into this same controller can carry a join the old one did
    /// not (a 2p opponent who names themselves on their first move), and the
    /// board would otherwise keep calling them "Seat 2" for the rest of the game.
    public private(set) var names: [Int: String]

    /// Bumped by `adopt` - a chain that ARRIVED while this board was open,
    /// folded into this controller instead of a fresh one.
    ///
    /// The board watches this to know that the next `view` change is not its own
    /// move but somebody else's turn landing, so it plays the arrival the way a
    /// cold open plays it (`replayLastMoveOnOpen`) rather than as an ordinary
    /// placement. Monotonic, so a board that missed one still notices.
    @Published public private(set) var arrivalTick = 0

    /// THE VEIL's owner. True from the moment a chain (opened or arrived) is
    /// resident with animations still to play, false once the board has taken
    /// those animations over.
    ///
    /// It lives on the CONTROLLER, not the board, and that is the whole point.
    /// It used to be `@State private var settled` initialized at construction,
    /// on the reasoning that construction is the one moment guaranteed to
    /// precede every paint - which is true, and is exactly why an arriving
    /// bubble had to REBUILD the whole board to be veiled in time. Rebuilding
    /// the board is what the owner sees as "still flashes if move comes in
    /// during expanded screen". Published state gets the veil up before the
    /// first paint of the new chain WITHOUT a teardown: `adopt` sets this and
    /// `openReplayEvents` before it publishes the new `view`, so the very first
    /// body evaluation that can see the arrival already knows to hide it.
    @Published public private(set) var replayPending = false

    private let kernel = MessageKernel.shared
    private let store: MessageGameStore

    /// The re-establishable base — the bytes the whole game derives from.
    private enum Base {
        case continuation(payload: Data)          // re-adopt this chain
        case genesis(seed: Data, players: Int)    // re-deal this game
    }
    /// All four are replaced wholesale by `adopt` when a newer chain arrives,
    /// which is why they are `var`: the controller's identity is the GAME and
    /// the SEAT, not any one chain along it.
    private var base: Base
    private let gameId: UInt64
    private var parent8: Data
    private var joins: [MessageJoin]
    /// DEPRECATED (retained only so GameSurface's call sites compile unchanged):
    /// the previously-cached chain, once used to diff my hand for the open-replay.
    /// The open-replay is now the kernel's evwire for the last move
    /// (`openReplayEvents`, resolved from the adopted chain alone), which needs no
    /// "where I last looked", so this is no longer read. Safe to remove along with
    /// its GameSurface threading in a follow-up cleanup.
    private let prevPayload: Data?
    /// Round-9 #5: this base is the chain THIS DEVICE just pressed Send on
    /// (MessageGameStore's one-shot just-sent marker matched at adopt). Opening
    /// it must be QUIET - the "last move" on it is my own, watched live seconds
    /// ago; replaying it (plus the Rule-R "superseded" the stale ledger used to
    /// add) is what made every send end in a confusing self-replay.
    private var suppressOpenReplay: Bool

    public var gameIdString: String { String(gameId) }

    /// Continue a chain I just opened. The resident game may already be this
    /// payload (the view decoded it to resolve my seat); `begin()` re-adopts it
    /// anyway so the controller owns the base unambiguously. `store` is where
    /// the hand-arrangement rows live (round-8 #4).
    /// `prevPayload` is the previously-cached chain for this game (§ open-delta
    /// replay, notes 4/9/38) — nil skips the delta computation entirely.
    public init(parentPayload: Data, parent: MessageEnvelope, mySeat: Int,
                store: MessageGameStore = .shared,
                prevPayload: Data? = nil, suppressOpenReplay: Bool = false) {
        self.base = .continuation(payload: parentPayload)
        self.gameId = UInt64(parent.gameId) ?? 0
        self.parent8 = Self.firstEight(hex: parent.digest)
        self.joins = parent.joins
        self.store = store
        self.mySeat = mySeat
        self.names = Dictionary(parent.joins.map { ($0.seat, $0.name) },
                                uniquingKeysWith: { a, _ in a })
        self.prevPayload = prevPayload
        self.suppressOpenReplay = suppressOpenReplay
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
        self.store = store
        self.mySeat = 0
        self.names = [0: myNickname]
        self.prevPayload = nil
        self.suppressOpenReplay = false
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
    /// The finished game's shareable REPLAY code (§12), captured the moment the
    /// game ends - see `publish`. nil while a game is running, and nil for a
    /// finished one the kernel cannot encode.
    @Published public private(set) var replayCode: String?
    /// …as the web replay URL, `foolish.cards/<code>`: a long path segment is
    /// classified as a self-contained replay payload by the site itself
    /// (src/app/[game_id]/page.tsx), so this lands on the replay screen and
    /// needs no lookup and no account.
    public var replayURL: URL? { replayCode.map { MessageEnvelope.replayLink(code: $0) } }
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
        // on the adopted chain (notes 6/12/#9), resolved after rebuildBase puts
        // the received chain resident. `prevPayload` is no longer consulted:
        // the kernel decides the group from the chain alone.
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
        let replay = suppressOpenReplay ? []
            : await kernel.lastMoveEvents(viewer: mySeat, atomsBefore: baseAtomsBefore)
        // ROUND 9 (owner): the durable pending ledger - and the Rule R rebase
        // that replayed its survivors here as `preStaged` - is REMOVED. Staged
        // moves live only in memory; the staged input-field bubble itself still
        // carries them as a sealed chain.
        pending = []
        // ROUND 16: the two breadcrumbs the flight recorder wants from this
        // file. Adopting a chain is the biggest single piece of work the
        // extension does (a full replay of every atom, then a view and a legal
        // menu) and it grows with the game, so if the drawer dies late in a long
        // match this is the line the trail will end on. The atom count rides
        // along because "how far into the game" is the question being asked.
        FlightRecorder.note("adopt", "turn \(baseTurn), \(replay.count) to animate")
        // The veil and the board it describes go up TOGETHER - see `publish`.
        await publish(openReplay: replay)
        // Round-8 #4: opening a FINISHED chain is one of the two moments a game
        // provably ends on this device (the other is committing my own final
        // move, markSent below) - drop its stored hand arrangement, the cache
        // exists only to survive mid-game reopens. Not done at apply(): an
        // unsent final move can still be undone, and the arrangement must
        // survive that undo.
        if view?.isOver == true { store.clearHandOrder(gameId: gameIdString) }
        ready = true
    }

    /// Fold a chain that ARRIVED into this same controller (design §7.2's adopt,
    /// without the teardown). Same game, same seat - only the chain moves on.
    ///
    /// Why not just build a fresh controller, which is what every adopt used to
    /// do? Because the board is keyed on the controller's identity
    /// (`GameSurface.expandedContent`'s `.id(ObjectIdentifier(controller))`), so
    /// a new controller means SwiftUI throws the whole board away and builds
    /// another: new `@State`, unmeasured geometry, a first paint at defaults.
    /// That teardown IS the flash the owner reports when a move arrives on an
    /// expanded board. Keeping the controller keeps the board, its measured
    /// frames and its animator - and the arrival plays as an animation on a
    /// board that never blinked.
    ///
    /// Staged moves do NOT survive: the arriving chain is the thread's truth and
    /// `pending` was composed against a parent that is now history. That is the
    /// same thing a fresh controller did (round 9 dropped the durable ledger),
    /// so nothing regresses here - see §7.4 for what a real rebase would add.
    /// May an arriving chain be folded into THIS controller rather than
    /// replacing it? Only when it is the same game, the same seat, and this
    /// controller is a started continuation that has finished `begin()`.
    ///
    /// A rule about IDENTITY - "the board on screen is still this board" - and
    /// getting it subtly wrong is worse than the flash it exists to prevent:
    /// re-adopting across a different game would put one game's chain onto
    /// another game's measured board.
    public func canAdopt(seat: Int, gameId: String) -> Bool {
        ready && mySeat == seat && gameIdString == gameId && isContinuation
    }

    public func adopt(payload: Data, parent: MessageEnvelope, quietOpen: Bool = false) async {
        base = .continuation(payload: payload)
        parent8 = Self.firstEight(hex: parent.digest)
        joins = parent.joins
        names = Dictionary(parent.joins.map { ($0.seat, $0.name) },
                           uniquingKeysWith: { a, _ in a })
        suppressOpenReplay = quietOpen
        lastChangeWasUndo = false
        arrivalTick += 1
        await begin()
    }

    private func rebuildBase() async {
        switch base {
        case .continuation(let payload):
            // The envelope's own clock and bubble delta come back with the
            // decode - the hold measures from the one, the open-replay groups
            // on the other, and both belong to the CHAIN, not to this device.
            adoptBaseFacts(try? await kernel.decode(payload: payload, viewer: mySeat))
        case .genesis(let seed, let players):
            adoptBaseFacts(nil)   // nothing sent, nothing before my moves, no boundary
            try? await kernel.newGame(seed: seed, players: players)
        }
    }

    /// The three things this controller knows about the chain UNDERNEATH its
    /// staged moves. They are set together, from one decode, by everything that
    /// changes WHICH chain that is: an undo (`rebuildBase`) and - since round 16
    /// stopped closing the drawer on Send - my own bubble becoming it
    /// (`markSent`). Kept in one place because a half-updated base is not a
    /// visible bug on this device, it is a wrong boundary on somebody else's.
    private func adoptBaseFacts(_ env: MessageEnvelope?) {
        baseSentAt = env?.sentAt ?? 0
        baseTurn = env?.turn ?? 0
        baseAtomsBefore = env?.atomsBefore ?? -1
    }

    /// The board has taken the pending animations over (or there were none):
    /// drop the veil. Idempotent - every view change calls it.
    public func consumeReplayPending() {
        if replayPending { replayPending = false }
    }

    /// The SEND CLOCK of the chain this controller is playing on (0 for a
    /// genesis, or for a format-2 chain from a build older than round 16). The
    /// pickup hold measures from it; see MessageEnvelope.sentAt.
    private var baseSentAt = 0
    private var holdTicker: Task<Void, Never>?

    /// The atom count of the chain this controller adopted, and the boundary
    /// the bubble it came in carried (`MessageEnvelope.atomsBefore`; -1 on a
    /// format-2 chain or a genesis, which means "the kernel guesses").
    private var baseTurn = 0
    private var baseAtomsBefore = -1

    /// Where the animation now on screen STARTS, for
    /// `MessageKernel.lastMoveEvents`: the number of atoms that were on the
    /// chain before it.
    ///
    /// Two things get animated on this board and they have different answers.
    /// With nothing staged, what is on screen is the bubble I opened, and that
    /// bubble states its own boundary. Once I have staged moves the resident
    /// game is that bubble PLUS my actions, so the boundary is the whole chain
    /// I adopted - everything past it is mine, however many atoms the codec
    /// made of it (which is exactly why this is a base and not a count: a
    /// staged action is not reliably one atom).
    ///
    /// Getting it wrong is not a crash, it is a re-run: too high a base drops
    /// the front of my own turn, too low a one replays the move before it -
    /// which is the very bug the delta exists to kill.
    public var animAtomsBefore: Int { pending.isEmpty ? baseAtomsBefore : baseTurn }

    public func refresh() async {
        await publish(openReplay: nil)
    }

    /// Read the board and PUBLISH IT IN ONE GO - view, legal moves, the pickup
    /// hold and (on `begin`) the open-replay stream, assigned back to back with
    /// no `await` between them.
    ///
    /// ROUND 16, and the reason this is not just tidiness. These are separate
    /// `@Published` properties and every `await` here is a chance for SwiftUI to
    /// paint what has been assigned so far. Assigned one at a time, the board
    /// gets painted with the NEW open-replay events against the OLD view - and
    /// `MessageTableView.pendingOpen` reads exactly that pair, freezing every
    /// badge to "the old board, minus a move that has not landed in it yet".
    /// That is a count too high for one paint, corrected the moment the view
    /// catches up: the owner's "briefly have their card count bumped". The same
    /// window let `legal` describe a board that was already gone, which is a
    /// button offering a move the kernel would refuse.
    ///
    /// `openReplay` nil means "leave the replay stream alone" (an ordinary
    /// refresh after a move); a value replaces it, which only `begin` does.
    private func publish(openReplay: [GameEvent]?) async {
        let v = await kernel.residentView(viewer: mySeat)
        let l = await kernel.residentLegal(seat: mySeat)
        let held = baseSentAt == 0 ? 0
                 : await kernel.pickupHold(seat: mySeat, sentAt: baseSentAt)
        // The replay code is READ HERE, not at the moment the link is tapped,
        // because it is a question about the RESIDENT game and the resident game
        // does not stay put: every bubble snapshot and every Rule-P comparison
        // decodes into the same kernel and re-points it. Asked here it is asked
        // in the same breath as the view it belongs to; asked on tap it would be
        // whatever game the engine happened to be holding by then.
        let code = v?.isOver == true ? await kernel.residentReplayCode() : nil
        if let evs = openReplay {
            openReplayEvents = evs
            // Raised in the same breath as the view it describes: the paint that
            // first shows this chain must already know which cards are still to
            // fly, and must never see one without the other.
            replayPending = !evs.isEmpty
        }
        view = v
        legal = l
        if code != replayCode { replayCode = code }
        if held != pickupHold { pickupHold = held }
        armHoldTicker(held)
    }

    /// Re-ask the kernel for the hold and, if one stands, keep asking once a
    /// second until it lapses. Every path that changes the game runs through
    /// `refresh()`, so the hold cannot outlive the state it was computed from:
    /// staging a cover makes the last action a cover, which ends the hold on the
    /// next read (owner: "if the player chooses a card in the mean time, this
    /// shouldn't cause any issues - the timer should just not do anything").
    private func armHoldTicker(_ held: Int) {
        holdTicker?.cancel()
        guard held > 0 else { holdTicker = nil; return }
        holdTicker = Task { [weak self] in
            while !Task.isCancelled {
                try? await Task.sleep(nanoseconds: 1_000_000_000)
                guard let self, !Task.isCancelled else { return }
                let now = self.baseSentAt == 0 ? 0
                        : await self.kernel.pickupHold(seat: self.mySeat, sentAt: self.baseSentAt)
                if now != self.pickupHold { self.pickupHold = now }
                if now == 0 { return }
            }
        }
    }

    // MARK: turn actions

    public func apply(_ move: Move) async {
        lastChangeWasUndo = false
        // ROUND 16: the hold, enforced and not merely displayed. The button is
        // already hidden while `pickupHold` stands, so this only fires on a path
        // the UI does not draw (a drop gesture, the dev harness, a future
        // shortcut) - which is exactly why it is here and not only there.
        if move.type == .pickup, pickupHold > 0 {
            lastRejectReason = 0
            rejectTick += 1
            return
        }
        do {
            try await kernel.apply(seat: mySeat, move: move)
            pending.append(move)
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
    /// human re-send) a move already in the thread. The sent move stays applied to
    /// the resident game, so the board keeps showing the state I just sent, only
    /// without a pending move to undo or re-send. No-op when nothing is staged
    /// (e.g. a genesis with no move).
    ///
    /// ROUND 16 - `payload` is the chain that actually went out, and passing it
    /// REBASES this controller onto it. Until this round the send closed the
    /// drawer (didStartSending's `dismiss()`), so the next move was always played
    /// by a controller rebuilt from the sent bubble; now that the drawer stays
    /// open, the same controller plays it, and everything that rebuild used to
    /// refresh has to be refreshed here instead. Without it the next bubble is
    /// measured against the chain this drawer ADOPTED - one bubble stale - so it
    /// claims BOTH moves as its own and replays the one its recipient just
    /// watched (pinned in MessageSendStaysOpenTests; it is the same doubled cover
    /// n_new exists to prevent, arriving by a different road), and it points its
    /// parent tag past the bubble it was actually sent after.
    ///
    /// The decode replaces the resident game with a game identical to the one
    /// already there - the sent chain IS the base plus my moves - so nothing on
    /// screen moves. `nil` (or a payload that will not decode) keeps the old
    /// behaviour: forget the staged move and leave the base alone.
    public func markSent(payload: Data? = nil) async {
        guard !pending.isEmpty else { return }
        pending = []
        lastChangeWasUndo = false
        if let sent = payload,
           let env = try? await kernel.decode(payload: sent, viewer: mySeat) {
            base = .continuation(payload: sent)
            parent8 = Self.firstEight(hex: env.digest)
            // My own seal appended my nickname if the parent had not named me
            // yet (`sealJoins`), so the roster comes back from the wire rather
            // than being patched here - the same way `adopt` takes it.
            joins = env.joins
            names = Dictionary(env.joins.map { ($0.seat, $0.name) },
                               uniquingKeysWith: { a, _ in a })
            adoptBaseFacts(env)
        }
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
        // note 10: set BEFORE refresh() — the state may legally go
        // battles→empty here (undoing the move that opened a bout), and
        // flyBoutEndToDiscard must not mistake that for a real bout end and
        // replay the PREVIOUS bout's draws.
        lastChangeWasUndo = true
        await refresh()
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
    ///
    /// `sentAt` is the round-16 send clock and defaults to this device's own -
    /// the receiving defender's 15-second pickup hold measures from it. The
    /// only caller that passes anything else is the test that pins what a
    /// CLOCKLESS (pre-round-16, format 2) bubble does, which is nothing.
    public func stagedPayload(sentAt: Int = MessageKernel.clockNow()) async throws -> Data {
        FlightRecorder.note("seal", "\(pending.count) staged")
        return try await kernel.seal(phase: isOver ? 3 : 2,
                              lastActorSeat: mySeat,
                              gameId: gameId,
                              parent8: parent8,
                              joins: sealJoins,
                              sentAt: sentAt)
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
