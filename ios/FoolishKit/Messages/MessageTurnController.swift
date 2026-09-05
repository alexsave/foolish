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
    /// ROUND 21: THE BOARD THIS BUBBLE FOUND - the committed state of the step
    /// immediately before `openReplayEvents` begins, or nil when there is none
    /// to be had (a genesis, the first move on a fresh deal, a chain whose step
    /// index does not line up - see `MessageKernel.lastMoveEventsWithPrior`).
    ///
    /// It exists for the ROLE MARKS, which are the one thing a replay cannot
    /// read off its own stream. Everything else a cold open needs is IN the
    /// stream: which cards move, where from, where to. But a mark's motion is
    /// the difference between two boards, and the earliest board the stream
    /// carries is already one move too late - an event's `state` is the table AS
    /// OF that step. So a bubble whose move was a `good` opened with the check
    /// already on the badge and nothing left to animate (the owner: "I don't see
    /// the sword to good transition. It started out already in GOOD").
    ///
    /// Only ever a SEED. Once the board is live its marks are advanced by what
    /// it watched happen, and this is not consulted again.
    @Published public private(set) var openReplayPriorState: GameView?
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
        // The prior board goes in too - see `preBoutTable`. A single-action
        // pickup turn carries no earlier step of its own to read the real table
        // off, and this is the only place that table still exists.
        Self.preBoutTable(openReplayEvents, prior: openReplayPriorState)
    }

    /// The rule above, as a pure function of ANY event stream.
    ///
    /// ROUND 16 lifted it out of the property because the LIVE bout-end needs
    /// the same answer: when a cover ends the bout in the same kernel apply, the
    /// board's own prior view is the table WITHOUT that cover on it (the view
    /// went straight from "uncovered attack" to "empty"), so a sweep built from
    /// it shows a table missing the card the player just played. The kernel's
    /// stream is the only place the covered table exists, live or on open, and
    /// there must be exactly one reading of it.
    public static func preBoutTable(_ evs: [GameEvent], prior: GameView? = nil) -> [BattleView] {
        guard let bi = evs.firstIndex(where: {
            $0.kind == .discard || $0.kind == .cardsToTrash || $0.kind == .pickup
        }) else { return [] }
        // Every card this step takes off the table - the only test a candidate
        // table has to pass. It must account for exactly these; a board that
        // holds more, or fewer, is describing some other moment.
        func ids(_ bs: [BattleView]) -> Set<String> {
            Set(bs.flatMap { [$0.attack.identity] + ($0.defense.map { [$0.identity] } ?? []) })
        }
        // discard / trash: walk back from the trash step to the last board that
        // still had cards on the table - that is the table about to be swept.
        //
        // A PICKUP NOW WALKS THE SAME WAY, and the flat one-battle-per-card
        // list below is its LAST resort rather than its only answer.
        //
        // The flat shape is a DIFFERENT GRID: a table that really held two
        // battles with one of them covered comes back as three single-card
        // cells. `MessageTableView.battlesArea` renders the sweep grid through
        // the SAME FBattleGrid the live table used, deliberately keeping card
        // identity so the cards sit still and then fly - which means handing it
        // a differently shaped table does not cut to the new one, it ANIMATES
        // every card into its new cell first. Owner: "when the opponent picked
        // up 6 of diamonds, k of diamonds, k of hearts, they did not animate
        // directly from their table positions, but seemed to spread out to an
        // evenly spaced row, AND THEN fly to the hand. weve been over this".
        //
        // "We have been over this" is `MessageTableView.sweepTableForReplay`,
        // which guards the wrong side of the problem: it prefers the board's
        // own `lastBattles` only when they account for every picked-up card,
        // and that is never true on a COLD OPEN (that board has rendered no
        // table at all) and cannot be true when the arriving stream placed one
        // of those cards itself. So the reconstruction wins in precisely the
        // cases that matter, and the only real cure is a reconstruction that is
        // right. That guard stays where it is - it is still the better answer
        // when the board has a live table - it simply stops being the only
        // thing standing between the player and a flattened grid.
        for i in stride(from: bi, through: 0, by: -1) {
            guard let b = evs[i].state?.battles, !b.isEmpty else { continue }
            guard evs[bi].kind != .pickup || ids(b) == ids(pickupTable(evs[bi])) else { break }
            return b
        }
        guard evs[bi].kind == .pickup else { return [] }
        // ...and the board the kernel says this whole stream OPENED on, for the
        // single-action pickup turn that carries no earlier step whatsoever.
        // Same exact-account test: a prior board from before some other move is
        // not the table being swept, and guessing would be worse than the flat
        // reading, which is at least about the right cards.
        if let p = prior, !p.battles.isEmpty, ids(p.battles) == ids(pickupTable(evs[bi])) {
            return p.battles
        }
        return pickupTable(evs[bi])
    }

    /// The flat one-uncovered-battle-per-card reading of a pickup step: the
    /// kernel never masks a pickup, so every viewer gets real identities and
    /// this can always be built. It is what the web does for the same animation
    /// (AnimationContext: one battle per card) and it is the right SET of cards
    /// every time - it just does not know the PAIRING, which is why
    /// `preBoutTable` now reaches for a real table first.
    private static func pickupTable(_ ev: GameEvent) -> [BattleView] {
        ev.cards.compactMap { $0 }.map { BattleView(attack: $0, defense: nil) }
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

    #if DEBUG
    /// THE RIG'S ORACLE (HarnessScenario `arrival`). The harness drives the
    /// thread from outside the surface, so it can say what the board OUGHT to
    /// show but could never read what the board's controller actually publishes
    /// - and "the published view is behind the kernel" is exactly the defect
    /// the arrival rig exists to catch. Weak, so the rig never extends a
    /// torn-down controller's life; DEBUG-only, so shipping code cannot grow a
    /// dependency on it.
    public private(set) static weak var debugLatest: MessageTurnController?
    #endif

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
    public init(parentPayload: Data, parent: MessageEnvelope, mySeat: Int,
                store: MessageGameStore = .shared,
                suppressOpenReplay: Bool = false) {
        self.base = .continuation(payload: parentPayload)
        self.gameId = UInt64(parent.gameId) ?? 0
        self.parent8 = Self.firstEight(hex: parent.digest)
        self.joins = parent.joins
        self.store = store
        self.mySeat = mySeat
        self.names = Dictionary(parent.joins.map { ($0.seat, $0.name) },
                                uniquingKeysWith: { a, _ in a })
        self.suppressOpenReplay = suppressOpenReplay
        #if DEBUG
        Self.debugLatest = self
        #endif
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
        self.suppressOpenReplay = false
        #if DEBUG
        Self.debugLatest = self
        #endif
    }

    // MARK: THE HELD SETTLEMENT
    //
    // A staged move is not a move. It sits in the input field until the human
    // presses Send, and until then it can be undone - or the bubble can simply
    // be deleted, which no amount of hiding the Undo button prevents. So a
    // staged move must never TELL its player anything they could act on.
    //
    // Three moves close a bout, and every one of them deals: the last good
    // owed, a cover that empties the defender's hand (four same-rank covers on
    // four same-rank attacks included - that is this case, not a rule of its
    // own), and a pickup, which refills the picker too when the table left them
    // short of six. Applied whole at staging time, each one hands its player a
    // look at the new hand with the move still retractable: say good, see the
    // deal, undo, throw in another card instead.
    //
    // So the turn is cut in two at the kernel's own boundary
    // (`[GameEvent].settlementStart`). The ACTION half plays as it is staged -
    // the cover lands, the table is taken, the good is declared. The
    // SETTLEMENT half - discard, deal, roles - is held here, with the board
    // showing the state before it, until `markSent` says the bytes went out.
    // Recipients need none of this: their bubble carries the whole turn and
    // they were never in a position to take it back, so they animate all of it
    // the moment it arrives.
    //
    // What is NOT involved: the App Group cache, or any other durable state.
    // The hold is an in-memory fact about a move this controller applied and
    // has not yet seen sent, which is exactly as long as it needs to live.
    private var heldSettlement: [GameEvent] = []
    /// The board as of the last step BEFORE the held settlement, masked for my
    /// seat - the kernel's own per-step snapshot, not a state assembled here.
    /// Published as `view` while the hold stands.
    private var heldView: GameView?
    /// The un-held truth, for the questions that are about the GAME rather than
    /// about what this device is currently showing (sealing, and whether this
    /// game is finished).
    private var residentOver = false

    /// Is a staged bout end waiting on Send? The board reads it to explain the
    /// wait; `legal` is already empty, so nothing can be played into it.
    public var settlementHeld: Bool { !heldSettlement.isEmpty }

    /// The half of a staged turn to animate NOW (everything before the
    /// settlement). nil when this move needs no special handling and the board
    /// should work the animation out the way it always has. Consumed by the
    /// board on the very next view change, like `pendingCover`.
    private var stagedAnimation: [GameEvent]?
    /// The held half, handed over the moment Send lands. Same one-shot
    /// contract; a value here means "play this, whatever the view diff looks
    /// like" - a released pickup goes from an empty table to an empty table and
    /// there is no diff to read.
    private var releasedSettlement: [GameEvent]?

    public func takeStagedAnimation() -> [GameEvent]? {
        defer { stagedAnimation = nil }
        return stagedAnimation
    }
    public func takeReleasedSettlement() -> [GameEvent]? {
        defer { releasedSettlement = nil }
        return releasedSettlement
    }

    /// Drop a hold without releasing it: the staged move it belonged to is
    /// gone (undone, or overtaken by a chain that arrived). The resident game
    /// has already been rebuilt without it, so there is nothing to animate and
    /// nothing to withhold.
    private func dropHold() {
        heldSettlement = []
        heldView = nil
        stagedAnimation = nil
        releasedSettlement = nil
    }

    /// THE SEND HAPPENED, WHATEVER WE THEN DECIDE ABOUT THE BYTES.
    ///
    /// The hold exists because a staged move can still be undone, or its bubble
    /// simply deleted out of the input field (see THE HELD SETTLEMENT above).
    /// Once the host reports Send, neither is true any more - and that fact is
    /// not in doubt even in the cases where the bytes that came with the signal
    /// are.
    ///
    /// So every exit from `markSent` releases, because `markSent` is the ONLY
    /// releaser there is: a return that keeps the hold keeps it forever, with
    /// `legal` forced empty and `view` pinned to `heldView` - a board no tap can
    /// move and no arrival can unstick. Owner, 1.0(35), on a bout-ending good in
    /// a two-player game: "it rotated my sword to a check, and the bubble
    /// preview showed the correct state after the good/discard/draw, but my
    /// board didn't move. It just stayed stuck in the good checkmark state, and
    /// didn't move when I sent it. had to close and reopen the extension, which
    /// is an antipattern." The 1.0(24) note further down this file describes the
    /// same symptom and fixed only the PREDICATE of the guard that caused it;
    /// this is the missing half, the CONSEQUENCE.
    ///
    /// It is also what makes the game-over screen reachable from a winning move:
    /// the board only learns the game is over from the released settlement.
    private func releaseHoldForSend() {
        guard !heldSettlement.isEmpty else { return }
        FlightRecorder.note("settle-release", "\(heldSettlement.count) steps")
        releasedSettlement = heldSettlement
        heldSettlement = []
        heldView = nil
        stagedAnimation = nil
    }

    /// Split the turn just staged at its settlement boundary, if it has one.
    /// Called after every `apply`, before the board is published, so the view
    /// that reaches the first paint is already the withheld one.
    private func captureSettlement() async {
        // Asked of the kernel here rather than read off `animAtomsBefore`: this
        // runs BEFORE `publish`, so the stored one still describes the board
        // before this move. One call, rebuilt from this controller's own chain,
        // for the reason `publish` spells out - a stream cut on somebody else's
        // board would withhold the wrong half of this turn.
        guard let turn = try? await kernel.stagedTurn(sealBase, replaying: pending,
                                                      seat: mySeat)
        else { dropHold(); return }
        let evs = turn.events
        guard let cut = evs.settlementStart else { dropHold(); return }
        // The board to show while the rest is withheld. For a good the cut is
        // at index 0 - a good emits no step of its own - and the transition
        // step it lands on carries the PRE-discard board (game.c's
        // ENGINE_HOOK_MAGIC_TRANSITION fires before anything moves), which is
        // exactly the state being asked for. Otherwise it is the state the
        // acting step committed: the cover on the table, the table taken.
        let held = cut > 0 ? evs[cut - 1].state : evs[cut].state
        // No snapshot to hold on means no honest way to show a half-applied
        // turn, so don't: a whole animation is a far smaller problem than a
        // board rendered from a state nobody vouched for.
        guard let held else { dropHold(); return }
        heldSettlement = Array(evs[cut...])
        heldView = held
        stagedAnimation = Array(evs[..<cut])
        releasedSettlement = nil
    }

    /// The host has told us the human TAPPED Send (didStartSending), but the
    /// rebase that follows is asynchronous - it decodes the bytes that went
    /// out. Nothing may be sent or undone in that window: the move is already
    /// on its way into the thread, so an Undo pill surviving into it offers to
    /// retract something that cannot be retracted, and re-staging from it
    /// re-sends a move the thread already has.
    ///
    /// Owner, 1.0(22): "the undo button disappeared... which should probably
    /// disappear the second you hit send btw". `markSent` used to be the only
    /// thing that cleared it, and reaching `markSent` costs a root-view rebuild,
    /// an onChange and a Task hop before its first line runs.
    @Published public private(set) var sending = false

    /// Called synchronously from the send signal, ahead of the async rebase.
    public func markSending() { if !sending { sending = true } }

    public var canSend: Bool { !pending.isEmpty && !sending }
    /// Is there a sendable bubble right now? Either I've staged a move
    /// (`canSend`), OR it's a fresh genesis where I have no legal move — i.e. I
    /// dealt the game but I'm not the first attacker, so the ONLY way the game
    /// progresses is to send the deal to whoever IS the first attacker. Without
    /// this, a creator who doesn't hold the lowest trump is stuck on a board with
    /// no move and no send (B4 bug: "Start game" left you unable to send).
    public var canStage: Bool { !superseded && (canSend || (isGenesis && !iCanAct)) }
    public var iCanAct: Bool {
        !superseded && !legal.contains { $0.type == .wait } && !legal.isEmpty
    }

    /// ROUND 20: this board is a BRANCH OFF AN OLD BUBBLE. A chain that beats it
    /// under Rule P has already been through this device, so whatever is played
    /// here is played onto a state the table has moved past (owner: "prevent
    /// offline players from staging moves. They might be trying to cheat by
    /// holding an older state and branching from it instead of live game").
    ///
    /// READ-ONLY, not hidden: the board still renders, because looking back at
    /// an old bubble is a legitimate thing to do and always has been. What it
    /// stops is ACTING - `iCanAct` and `canStage` are the two questions the
    /// whole board is built out of, so standing them down takes the action bar,
    /// the cover highlights, the drop targets and the Send path with them, in
    /// one place rather than five.
    ///
    /// Set from outside (GameSurface owns the comparison; the kernel owns Rule
    /// P) and re-asked on every adopt, so the newest bubble arriving on a stale
    /// board hands it back the right to play. Defaults to FALSE and every path
    /// that cannot answer leaves it there: a device with nothing on file, or a
    /// kernel call that threw, must never be the reason a game cannot be played.
    @Published public private(set) var superseded = false

    public func setSuperseded(_ v: Bool) {
        guard superseded != v else { return }
        superseded = v
        AnimLog.say("board \(v ? "SUPERSEDED - read only" : "is live again")")
    }
    /// Is the GAME over - not "does the board show a finished game". They differ
    /// while a settlement is held: a cover that put me out has been applied and
    /// must be SEALED as a finished game (phase 3), while the board is still
    /// showing the bout that ended it.
    public var isOver: Bool { residentOver }
    /// The finished game's shareable REPLAY code (§12), captured the moment the
    /// game ends - see `publish`. nil while a game is running, and nil for a
    /// finished one the kernel cannot encode.
    @Published public private(set) var replayCode: String?
    /// …as the web replay URL, `foolish.cards/<code>-<names>`: a long path
    /// segment is classified as a self-contained replay payload by the site
    /// itself (src/app/[game_id]/page.tsx), so this lands on the replay screen
    /// and needs no lookup and no account.
    ///
    /// THE NICKNAMES RIDE ALONG (ReplayExtras). The kernel's code is the cards
    /// and nothing else, so a game between friends used to replay on the web as
    /// "P1" beating "P2" - the site has read a names channel off the end of the
    /// code since the channel existed, and this was the one producer never
    /// writing it. The roster is `names`, the chain's own joins, padded to the
    /// table's width because the reader counts seats from the moves and expects
    /// one entry per seat; a seat nobody ever named stays "" and renders as
    /// "P<n>" exactly as it does today.
    ///
    /// `numPlayers` comes from the VIEW rather than the joins for the same
    /// reason: joins are who introduced themselves, seats are how many are
    /// playing, and they are not always the same number (a 2p opponent who has
    /// not moved yet has no join). Without a view there is no seat count to pad
    /// to, so the link degrades to the bare moves code - the old behaviour, not
    /// a broken one.
    public var replayURL: URL? {
        replayCode.map { code in
            MessageEnvelope.replayLink(
                code: code,
                names: ReplayExtras.seatNames(names, count: view?.numPlayers ?? 0))
        }
    }
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
        // Ahead of every await below: `base` may already have been moved by
        // `adopt`, and a controller that says "new chain, old staged moves" is
        // one the board rebuilds from as written. (The assignment further down
        // is where this used to live; it is kept there as the no-op it now is
        // for the paths that reach `begin` without an adopt.)
        pending = []
        // Whatever this board was withholding belonged to a staged move on the
        // chain being replaced. `pending` is dropped just below for the same
        // reason; a hold is the animation half of the same fact.
        dropHold()
        let opened = await rebuildBase()
        // The open animations: the kernel's viewer-aware evwire for the LAST move
        // on the adopted chain (notes 6/12/#9), resolved after rebuildBase puts
        // the received chain resident. There is no "where I last looked" to
        // consult - the kernel decides the group from the chain alone, which is
        // what let round 43 delete the `prevPayload` this used to name.
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
        // Round 21: the stream AND the board it starts from, in one actor call
        // (see `openReplayPriorState`). One call rather than two because a
        // second hop through the kernel is a second chance for some other decode
        // to repoint the resident game between them - which is why round 22
        // moved it one step further back still, into the adopt itself.
        let opening: (events: [GameEvent], prior: GameView?) = suppressOpenReplay ? ([], nil) : opened
        let replay = opening.events
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
        await publish(openReplay: replay, priorState: opening.prior)
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

    // MARK: the conflict model (docs/ANIMATION_CATALOGUE.md, decided 1.0(28))

    /// True while a staged move is being VISIBLY retracted ahead of an arriving
    /// chain: the base view (staged moves dropped) is what is published, the
    /// board is flying the staged cards home in red, and the arrival itself is
    /// latched in `conflictLatch` waiting for `finishConflictAdopt`. Read
    /// imperatively by the board's view-change handler (no publish needed - the
    /// retraction's own view change is what wakes the board).
    public private(set) var conflictRetracting = false
    /// What the arriving chain will animate and where it opens - the verdict
    /// inputs (`MessageTableView.conflictVerdict`) for the retraction. Peeked
    /// from the arriving payload BEFORE the retraction publishes, because the
    /// board must decide which staged cards fly home while the controller still
    /// stands on the OLD base. nil outside a retraction; `.unknown` when the
    /// arriving chain could not be read (then everything staged reverts, which
    /// is the honest default - an unreadable chain vouches for nothing).
    public private(set) var conflictFacts: ConflictFacts?
    /// The ONE latched arrival - newest wins, never a queue (the burst rule:
    /// "undo whatever is animating and play the last only"). An arrival landing
    /// while a retraction is already playing simply replaces the latch; the
    /// retraction is not restarted, because it already returns the board to the
    /// parent state and nothing new has played forward since.
    private var conflictLatch: (payload: Data, parent: MessageEnvelope, quietOpen: Bool)?
    /// The no-board safety net: if nothing calls `finishConflictAdopt` (the
    /// board died mid-retraction, or a retraction was armed with no board
    /// mounted after all), the latched arrival must still land - a dropped
    /// arrival is a board frozen one move in the past for good, which is a far
    /// worse failure than a skipped animation. Generous, because it must never
    /// fire while a real retraction flight is still airborne.
    private var conflictFailsafe: Task<Void, Never>?
    /// Internal so tests can shrink the wait; 3s is comfortably above one red
    /// flight (~0.55s) plus scheduling slack.
    static var conflictFailsafeSeconds: Double = 3.0
    /// Whether a live board is mounted on this controller (set from the board's
    /// own lifecycle). Without one there is nobody to fly the retraction, so an
    /// arrival adopts immediately - the conflict model is about a board that is
    /// SHOWING something; no board, no theatre.
    private var boardWatching = false

    public func setBoardWatching(_ on: Bool) {
        boardWatching = on
        // A board torn down mid-retraction must not strand the latched arrival
        // behind the failsafe's full wait - nobody is watching, so land it now.
        if !on, conflictLatch != nil { Task { await self.finishConflictAdopt() } }
    }

    /// AN ARRIVAL, OFFERED RATHER THAN FORCED. The routing layer
    /// (MessagesRootView.seatOnBoard) calls this instead of `adopt` so a chain
    /// arriving over a STAGED MOVE is visibly retracted first - the staged
    /// cards fly home tinted red, against the OLD base - and only then adopted.
    /// This is the commonest conflict on a real table and the one where the old
    /// silent swap was least defensible: the card I chose was on the table one
    /// frame and gone the next, with nothing to say it was mine or why it went.
    ///
    /// The hard invariant is unchanged and is why the adopt is DEFERRED rather
    /// than the retraction inserted into it: the board must never render
    /// new-base-with-old-staged-moves, not even for one frame. The retraction
    /// publishes the OLD base with the staged moves dropped (an undo-all), the
    /// red flight plays against that board, and `adopt` - which itself still
    /// clears `pending` before its first suspension, see the test pinning it -
    /// runs only when the board reports the reversal landed.
    public func offerArrival(payload: Data, parent: MessageEnvelope,
                             quietOpen: Bool = false) async {
        // A DUPLICATE OF THE CHAIN I AM ALREADY ON is not an arrival, staged
        // moves or not. `adopt`'s own guard skips duplicates only with nothing
        // staged (with pending moves it conservatively rebuilt, a shrug from
        // before the conflict model existed) - but under the conflict model
        // that shrug would RED-RETRACT my staged move because Messages
        // re-delivered a bubble, which is a retraction with nothing to retract
        // for. The staged moves were composed against exactly these bytes;
        // they stand.
        if ready, basePayload == payload {
            AnimLog.say("offerArrival skipped - already on this chain (pending=\(pending.count))")
            return
        }
        // MID-RETRACTION, only the latch moves - newest wins, one retraction
        // suffices. Checked BEFORE the pending guard below, because the
        // retraction has already emptied `pending`: without this order a burst's
        // second arrival would fall through to a plain adopt UNDERNEATH the
        // retraction, and the finish would then adopt the older latched chain
        // ON TOP of it - the board walking backwards one bubble
        // (ConflictModelTests.testANewerArrivalMidRetractionWinsTheLatch caught
        // exactly that).
        if conflictRetracting {
            conflictLatch = (payload, parent, quietOpen)
            return
        }
        // No staged moves, or nobody mounted to fly them home: adopt as ever.
        // A sequence still in flight is the BOARD's conflict (it reverses it
        // from its own recorded motions before playing the arrival - see
        // MessageTableView.replayLastMoveOnOpen); the controller only fronts
        // the case where the MODEL itself must walk back first.
        // A MOVE THE HUMAN HAS ALREADY SENT IS NOT A STAGED MOVE.
        //
        // `sending` stands from the send signal until `markSent` resolves it,
        // and it is exactly the window in which the bytes are already gone but
        // this controller has not caught up. Retracting there offers to take
        // back a bubble the thread already has - the owner, on a sent pickup
        // followed by an ordinary one-card throw-in: "Somehow this caused an
        // UNDO animation of the previous pickup! ... The state shown in the
        // bubble of the simple one card throw in by my opponent clearly
        // indicated that my card count was such that I had picked up. My pickup
        // definitely wasn't superceded."
        //
        // Whatever the arriving chain says about my move, its own replay is the
        // honest account of it; a red flight first is theatre for a retraction
        // that never happened. This is the floor under the substitution in
        // `markSent`: that stops `pending` being stranded in the first place,
        // and this stops the burst that lands DURING the send window from
        // retracting on the way past.
        guard boardWatching, !pending.isEmpty, !sending else {
            await adopt(payload: payload, parent: parent, quietOpen: quietOpen)
            return
        }
        conflictLatch = (payload, parent, quietOpen)
        conflictRetracting = true
        let staging = pending.count
        // The verdict inputs, peeked from the arriving chain before anything
        // moves: which cards its replay animates, and the board it opens on.
        // CLEAR is why this cannot be skipped - if the arriving chain is my own
        // sent move coming back (a lost send signal, a re-activation), my
        // staged cards are IN it, and flying them home first only for the
        // adopt to put them straight back is the flicker the web fixed.
        // The peek decodes into the resident slot, which is fine: every reader
        // in this file rebuilds its own chain before reading (round 22).
        if let peek = try? await kernel.openChain(payload: payload, viewer: mySeat) {
            conflictFacts = ConflictFacts(events: peek.events, prior: peek.prior)
        } else {
            conflictFacts = .unknown
        }
        // WHAT THE VERDICT WAS DECIDED ON, not just that a retraction happened.
        // A `conflict` line appearing after a successful `send-rebase` is itself
        // the spurious-undo signature; `prior=unknown` is why cards then flew
        // red rather than staying put, since empty facts make every card revert.
        FlightRecorder.note("conflict",
                            "retract \(staging) staged; prior=\(conflictFacts == .unknown ? "unknown" : "ok")")
        // An even newer arrival may have landed during that await - the latch
        // already holds it, and `finishConflictAdopt` adopts whatever is
        // newest. The facts describe the first conflicting chain, which is the
        // best knowledge the retraction had when it was decided; the burst rule
        // wants one reversal, not one per bubble.
        //
        // THE RETRACTION: drop the staged moves and publish the BASE view - the
        // undo-all. Same one-assignment discipline as `undo` and `adopt`
        // (`pending` empties before any suspension), and `lastChangeWasUndo` is
        // what routes the board's view-change handler into the reverse-flight
        // machinery it already has.
        let before = view
        pending = []
        dropHold()
        sending = false
        lastChangeWasUndo = true
        await refresh()
        // A retraction that changed nothing visible has nothing to fly - a
        // staged good is the real case: no card of its own on the table, and an
        // EQUAL view publishes no onChange for the board to act on. Waiting on
        // the failsafe would stall the arrival for nothing.
        if view == before {
            await finishConflictAdopt()
            return
        }
        armConflictFailsafe()
    }

    /// The reversal has landed (or provably never will): take the latched
    /// arrival and adopt it. Called by the board when its red flight lands, by
    /// `setBoardWatching(false)` when the board goes away, and by the failsafe.
    /// Idempotent - the latch is taken exactly once.
    public func finishConflictAdopt() async {
        guard let latch = conflictLatch else {
            conflictRetracting = false
            conflictFacts = nil
            return
        }
        conflictLatch = nil
        conflictRetracting = false
        conflictFacts = nil
        conflictFailsafe?.cancel()
        conflictFailsafe = nil
        await adopt(payload: latch.payload, parent: latch.parent,
                    quietOpen: latch.quietOpen)
    }

    private func armConflictFailsafe() {
        conflictFailsafe?.cancel()
        conflictFailsafe = Task { [weak self] in
            try? await Task.sleep(nanoseconds: UInt64(Self.conflictFailsafeSeconds * 1_000_000_000))
            guard !Task.isCancelled else { return }
            await self?.finishConflictAdopt()
        }
    }

    public func adopt(payload: Data, parent: MessageEnvelope, quietOpen: Bool = false) async {
        // A DUPLICATE DELIVERY IS NOT AN ARRIVAL. Messages can hand the same
        // bubble over twice (a re-delivered didReceive; two racing
        // maybeAdoptIncoming tasks whose "same chain" pre-check both read the
        // base this adopt had not written yet), and re-adopting the chain this
        // controller is already on re-armed the open-replay veil against a view
        // that was NOT going to change - `publish` guards that now, but there
        // is also no work here to redo: the chain is resident and the board is
        // showing it. Skipped only with nothing staged: with pending moves the
        // resident game is base+pending, and the conservative rebuild below is
        // the behaviour every duplicate got before this guard existed. (The
        // pending-nonempty duplicate no longer reaches here from the routing
        // layer at all - `offerArrival` skips it outright, keeping the staged
        // move - so this guard's narrowness only matters for direct callers.)
        if ready, pending.isEmpty, basePayload == payload {
            AnimLog.say("adopt skipped - already on this chain")
            return
        }
        base = .continuation(payload: payload)
        // TOGETHER WITH THE BASE, and before any await. `begin` clears this too,
        // but only after it has suspended on the rebuild - and in that window
        // the controller would read as "the NEW chain, with the moves I staged
        // against the OLD one still on top", which the board rebuilds from
        // literally: stale moves replayed onto a parent they were never legal
        // against. They are dropped rather than rebased for the reason `adopt`
        // gives above - the arriving chain is the thread's truth.
        pending = []
        sending = false
        parent8 = Self.firstEight(hex: parent.digest)
        joins = parent.joins
        names = Dictionary(parent.joins.map { ($0.seat, $0.name) },
                           uniquingKeysWith: { a, _ in a })
        suppressOpenReplay = quietOpen
        lastChangeWasUndo = false
        arrivalTick += 1
        await begin()
    }

    /// Re-establish the chain underneath this board AND read the animation its
    /// last bubble asks for - together, in one trip through the kernel.
    ///
    /// The stream used to be fetched on a SECOND hop, using the boundary this
    /// decode had just put down. That gap is where "pickup button won't appear
    /// if attack arrives while board is open" lives: an arrival adopts here
    /// while the surface is snapshotting bubbles, and whatever chain wins the
    /// race is the one the stream gets cut on. Callers that only want the base
    /// (an undo) simply drop the opening.
    @discardableResult
    private func rebuildBase() async -> (events: [GameEvent], prior: GameView?) {
        switch base {
        case .continuation(let payload):
            // The envelope's own clock and bubble delta come back with the
            // decode - the hold measures from the one, the open-replay groups
            // on the other, and both belong to the CHAIN, not to this device.
            // THE FLOOR IS WHAT THIS BOARD HAS ALREADY ANIMATED. `baseTurn`
            // still holds the PREVIOUS chain's atom count at this point -
            // `adoptBaseFacts` overwrites it just below - so it is exactly "how
            // far the player has already watched this game get to". A bubble
            // claiming to start earlier than that is claiming to re-show
            // something already on screen; see `openChain`. Zero on a cold open
            // (nothing adopted yet), where no clamp must apply.
            let alreadyShown = baseTurn
            guard let opened = try? await kernel.openChain(payload: payload, viewer: mySeat,
                                                           floor: alreadyShown) else {
                adoptBaseFacts(nil)
                return ([], nil)
            }
            adoptBaseFacts(opened.env)
            return (opened.events, opened.prior)
        case .genesis(let seed, let players):
            adoptBaseFacts(nil)   // nothing sent, nothing before my moves, no boundary
            try? await kernel.newGame(seed: seed, players: players)
            return ([], nil)      // nothing was sent, so there is nothing to replay
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
        passingAllowed = env?.passingAllowed ?? true
    }

    /// THE TABLE'S RULES, as the chain states them: may the defender transfer?
    ///
    /// The board never asks this to decide what a player may DO - the kernel
    /// simply stops offering a transfer, so the Pass button and the drag-to-pass
    /// disappear on their own (CardPlay reads `legal` for both). It is here for
    /// the one thing the legal menu cannot say: which RULES to teach. A
    /// podkidnoy player opening How to play must not be shown a page about
    /// passing.
    @Published public private(set) var passingAllowed = true

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
    ///
    /// The staged answer is the KERNEL's (`stagedAtomsBefore`, measured from its
    /// log mark), not `baseTurn` as it was until this round. The chain I adopted
    /// states its atom count, but the atom stream is re-derived from the whole
    /// log on every encode, so the same history can re-encode to FEWER atoms
    /// than that count once my move supersedes a pending good. Handed on as a
    /// starting point it lands past the end of the stream and the kernel
    /// correctly reports that this turn added nothing - so my own bout end
    /// animated not at all, and (round 16) was not recognised as a settlement to
    /// withhold. 22 of 848 staged turns in the sweep, all of them silent.
    public private(set) var animAtomsBefore: Int = -1

    public func refresh() async {
        await publish(openReplay: nil)
    }

    /// THIS TURN'S ANIMATION STREAM, from this controller's own chain.
    ///
    /// The board asks for it when a bout ends and it has not been handed a half
    /// (`MessageTableView.playBoutEnd`). It used to ask the kernel directly,
    /// which meant the discards and refills it flew came from whatever game was
    /// resident at that instant - and the instant in question is an arrival,
    /// when Rule P, the surface route and a bubble bake are all decoding at
    /// once. Same rebuild-then-read as `publish`; the boundary is the one
    /// `publish` published, so the stream and the board it plays over agree.
    public func turnEvents() async -> [GameEvent] {
        (try? await kernel.turnEvents(sealBase, replaying: pending, seat: mySeat,
                                      atomsBefore: animAtomsBefore)) ?? []
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
    /// `priorState` travels with it and is meaningless without it.
    private func publish(openReplay: [GameEvent]?, priorState: GameView? = nil) async {
        // ONE actor call, and it rebuilds THIS controller's chain before it
        // reads. These five values used to be five separate trips into the
        // kernel, which holds one resident game that decoding ADOPTS - so a
        // bubble snapshot, a Rule-P comparison or a surface reload landing
        // between two of them handed back a board from one chain and a menu
        // from another. 1.0(21), twice: "the 6 of diamonds somehow transformed
        // into a 6 of hearts covered by the 8 of hearts, and the pickup button
        // popped back up", and "pickup button won't appear if attack arrives
        // while board is open". See MessageEnvelope.readBoard, and
        // MessageResidentDriftTests for both halves.
        //
        // A rebuild that THROWS leaves everything below untouched: a board that
        // keeps showing the last state it could vouch for is a far smaller
        // problem than one blanked by a chain it could not read.
        guard let read = try? await kernel.readBoard(sealBase, replaying: pending,
                                                     seat: mySeat, sentAt: baseSentAt)
        else {
            FlightRecorder.note("read-failed", "\(pending.count) staged")
            return
        }
        let v = read.view
        let l = read.legal
        let staged = read.stagedAtomsBefore
        let held = read.hold
        // The replay code comes back with the view rather than being asked for
        // when the link is tapped: it is a question about a particular game, and
        // on tap it would be whatever game the engine happened to hold by then.
        let code = read.replayCode
        if let evs = openReplay {
            openReplayEvents = evs
            openReplayPriorState = priorState
            // Raised in the same breath as the view it describes: the paint that
            // first shows this chain must already know which cards are still to
            // fly, and must never see one without the other.
            //
            // …and ONLY when that view is actually about to change. The one
            // thing that takes this veil down is the board's
            // `.onChange(of: controller.view)` (flyBoutEndToDiscard's defer), and
            // SwiftUI does not fire onChange for an assignment of an EQUAL
            // value. So arming it while publishing an unchanged board - a
            // duplicate delivery of the chain already on screen was the
            // reproduced case - stranded every card in `openReplayTouchedCardIds`
            // behind `pendingOpen` forever: laid out in its slot, opacity 0, on
            // a board whose state was otherwise correct ("the attack flies,
            // lands, then disappears", 1.0(19) on device). A veil nothing will
            // consume must never go up.
            replayPending = !evs.isEmpty && (heldView ?? v) != view
        }
        residentOver = v?.isOver ?? false
        animAtomsBefore = pending.isEmpty ? baseAtomsBefore : staged
        // THE HELD SETTLEMENT: while one stands, the board a staged move
        // produced is the kernel's pre-settlement snapshot, and NOTHING is
        // legal. Both halves matter. The view is what keeps the deal out of
        // sight; the empty menu is what stops the same player from acting on
        // it anyway - a defender whose last cover swept the table becomes the
        // next first attacker, so without this they could pick that attack out
        // of a hand they are not supposed to have seen yet, all still staged.
        view = heldView ?? v
        legal = heldSettlement.isEmpty ? l : []
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

    /// Returns whether the move ACTUALLY HAPPENED - false for every refusal,
    /// silent or not.
    ///
    /// ROUND 40, and it is not bookkeeping: the board raises the veil over a
    /// played card SYNCHRONOUSLY, before this is called (MessageTableView
    /// `playAt` - `preHide` + `showHeld`, which is the only moment early enough
    /// to beat the paint), and from then on the only thing that ever takes that
    /// veil down is what happens NEXT - a view change, or `rejectTick`. Two of
    /// the three refusals below announce themselves that way; the retraction
    /// guard announced nothing at all, so a tap that landed in its window left
    /// its cards pre-hidden for the life of the board. `handSlotDeferred` then
    /// excludes them from a CENTRED fan forever - the hand lays out fewer cards
    /// than it holds with nothing animating, which is the owner's own
    /// breadcrumb, term for term:
    ///
    ///     fan-rows 1 rows laid=1 hand=4 width=398 deferred=3 veiled=3
    ///              preHidden=3 hidden=3 settled=true seq=0
    ///
    /// A refusal is now a FACT the caller is handed rather than a silence it
    /// has to infer from nothing happening.
    @discardableResult
    public func apply(_ move: Move) async -> Bool {
        // Mid-retraction the chain on screen is being replaced: a move staged
        // now would be composed against a base the latched arrival is about to
        // supersede, and would itself need retracting a breath later. The
        // window is one red flight long; the tap simply does nothing, exactly
        // as it would have a frame later when the arrival's board is up.
        guard !conflictRetracting else { return false }
        lastChangeWasUndo = false
        sending = false
        // ROUND 20: a board branching off an old bubble may not be played on.
        // Enforced here as well as displayed (`superseded` stands `iCanAct`
        // down, which is what takes the buttons and the drop targets away) for
        // the same reason the pickup hold below is: this is the one door every
        // gesture, every dev path and every future shortcut has to come through.
        if superseded {
            lastRejectReason = 0
            rejectTick += 1
            return false
        }
        // ROUND 16: the hold, enforced and not merely displayed. The button is
        // already hidden while `pickupHold` stands, so this only fires on a path
        // the UI does not draw (a drop gesture, the dev harness, a future
        // shortcut) - which is exactly why it is here and not only there.
        if move.type == .pickup, pickupHold > 0 {
            lastRejectReason = 0
            rejectTick += 1
            return false
        }
        do {
            try await kernel.apply(seat: mySeat, move: move)
            pending.append(move)
            // Before `refresh`, never after: `captureSettlement` decides what
            // this board is allowed to show, and publishing the resident view
            // first would put the deal on screen for a paint.
            await captureSettlement()
            await refresh()
            return true
        } catch MessageEnvelope.Failure.rejected(let reason) {
            lastRejectReason = reason
            rejectTick += 1
        } catch {
            lastRejectReason = 0
            rejectTick += 1
        }
        return false
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
    /// AND no bytes are handed over (e.g. a genesis with no move).
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
    /// screen moves. A payload that will not decode, or none to be had at all,
    /// leaves BOTH the base and the staged moves alone: half of that pair is a
    /// board walked backwards (see the 1.0(26) note in the body).
    ///
    /// The rebase does NOT depend on there being a staged move to clear. It
    /// used to: an empty `pending` returned before the decode, so a send that
    /// carried nothing of this controller's own (an undo-to-empty re-seal, a
    /// lobby bubble, a second signal for the same send) left the base pointing
    /// at the chain this drawer opened, one bubble behind the thread. The
    /// bubble after it then claimed BOTH moves - the very doubling this rebase
    /// exists to prevent. Being handed the bytes that went out is the whole
    /// signal; what was pending is only what to forget.
    /// WHICH BYTES WENT OUT, as a value - the rule stated above, extracted so
    /// it can be exercised rather than argued.
    ///
    /// It replaced two layers written in two different rounds, and the claim
    /// that they are equivalent is the whole risk of that change: this is the
    /// send path, and getting it wrong strands a settlement or red-retracts a
    /// move the thread already has. `MarkSentBytesTests` enumerates all eight
    /// combinations of (staged, host nil, sealed nil) against the superseded
    /// two-layer expression written out longhand, so the equivalence is checked
    /// rather than reasoned about.
    /// `nonisolated` because it is pure: three values in, one out, no actor
    /// state touched. That is what lets a test enumerate the input space
    /// without a controller and without hopping to the main actor.
    nonisolated static func sentBytes(staged: Bool, host: Data?, sealed: Data?) -> Data? {
        staged ? (sealed ?? host) : host
    }

    public func markSent(payload: Data? = nil) async {
        // THE SIGNAL MAY ARRIVE WITHOUT ITS BYTES, AND ON DEVICE IT DOES.
        //
        // Owner, 1.0(26), sending an attack: "nothing should animate, right?"
        // - and it animated the bubble BEFORE it. The trail says exactly what
        // happened, in three lines: `send` at 43.09s, `anim-live n=2 from=45
        // seats=0/1 kinds=pick,refi` at 43.13s, and NO `send-rebase` between
        // them. No `send-foreign` and no `send-unreadable` either, and those
        // three notes cover every way out of this function - so the payload was
        // NIL. That is the one path that empties `pending` while leaving `base`
        // where it stands.
        //
        // Both halves of the complaint follow from it. Emptying `pending`
        // un-plays the staged attack (base+pending is what the board is drawn
        // from since round 22), so the table drops from one card to none; the
        // board reads a table that just cleared as a bout ending and asks for
        // that turn's stream; and `animAtomsBefore` has just fallen back from
        // the staged boundary to `baseAtomsBefore`, which is the bubble before
        // mine. Hence `pick,refi from=45` - the arrival's own move, replayed a
        // second time, to land on the board from BEFORE the move just sent.
        // Whatever the previous bubble happened to be is what a send appears to
        // animate: a pickup here, a round-ending good in the 1.0(24) report.
        //
        // So the bytes stop being something this function has to be handed.
        // They travel from `didStartSending` through a root-view rebuild and a
        // SwiftUI `onChange` to reach it, which is a long way for a value to
        // survive, and none of it is necessary: `lastSealed` IS the chain this
        // controller sealed, and staging is the only thing that makes one. See
        // the resolution below for which of the two wins, and when.
        // THE TRAIL HAS TO SHOW THIS FUNCTION RAN AT ALL.
        //
        // Every note this function writes today is on a PATH through it, so a
        // `send-signal` with nothing after it is ambiguous between "the Task
        // never ran", "the controller was gone" and "it ran and fell out of a
        // branch that says nothing". 1.0(26) had to infer which by elimination
        // across three separate notes; this settles it in one line, and the
        // owner can pull it off the device with the gear hold.
        FlightRecorder.note("send-mark",
            "\(pending.count) staged, in=\(payload?.count ?? -1)b,"
          + " sealed=\(lastSealed?.count ?? -1)b, held=\(heldSettlement.count)")
        // WHICH BYTES WENT OUT - one rule, from two rounds that each found half
        // of it and never met.
        //
        // 1.0(26): the host's payload can arrive NIL. It travels from
        // `didStartSending` through a root-view rebuild and a SwiftUI
        // `onChange` into a Task, and a nil emptied `pending` while leaving
        // `base` where it stood - which un-plays the staged move, so the board
        // reads its own table clearing as a bout end and animates the bubble
        // BEFORE the one just sent. The answer was `lastSealed` as a fallback
        // for nil.
        //
        // 1.0(36): the host's payload can arrive STALE, which took the refusal
        // below instead - and a refusal is not the shrug it was written as. It
        // strands the withheld settlement that only this function releases, and
        // leaves `pending` full so the next arrival red-retracts a move the
        // thread already has. Owner, on a sent pickup then an ordinary throw-in:
        // "Somehow this caused an UNDO animation of the previous pickup! ... My
        // pickup definitely wasn't superceded."
        //
        // Those were fixed as two layers - a `??` for the nil and a
        // substitution for the stale - and the second silently reversed the
        // first's stated preference without deleting it. They are one sentence:
        //
        //   WITH MOVES STAGED, OUR OWN SEALED CHAIN IS THE BUBBLE.
        //   WITH NOTHING STAGED, ONLY THE HOST CAN SAY.
        //
        // Staged, there is exactly one bubble in the input field and this
        // controller sealed it (`stagedPayload` is the only thing that makes
        // one), so a send signal can only be that bubble going out - whatever
        // bytes came along for the ride, absent or stale. Unstaged there is no
        // such claim, which is also the shape a genuinely foreign signal arrives
        // in (a reload handed a chain this controller never made), and the
        // refusal below still stands untouched for it.
        //
        // Byte-for-byte identical to the two layers it replaces, checked over
        // all eight combinations of (staged, host nil, sealed nil). The two
        // trail notes keep their names: they are what the owner greps a device
        // dump for, and they still separate the two ways the host let us down.
        let sent = Self.sentBytes(staged: !pending.isEmpty, host: payload, sealed: lastSealed)
        if !pending.isEmpty {
            if let mine = lastSealed, mine != payload {
                if let host = payload {
                    FlightRecorder.note("send-substituted",
                                        "host \(host.count)b != sealed \(mine.count)b - using mine")
                } else {
                    FlightRecorder.note("send-bytesless",
                                        "rebased onto my own sealed chain instead")
                }
            }
        }

        // A SEND MAY NEVER MOVE THIS BOARD BACKWARDS.
        //
        // Owner, 1.0(22): a throw-in, Send, and then "the 10 of spades... the
        // card I picked up in the previous bout" flew into their hand, another
        // seat's count moved, a card dealt to that seat, and the board ended in
        // the state BEFORE the throw-in - correct again only after closing and
        // reopening the bubble. That is this rebase running on an OLDER chain:
        // `base` moves back a bubble, `baseAtomsBefore` with it, and the board
        // then animates the previous bubble's last move over the previous
        // bubble's board. The bytes that actually WENT OUT were the right ones,
        // which is why reopening put it right. The way in is
        // `didStartSending`'s `payload(of: message) ?? pendingStage?.payload`
        // with an unserialised `stage()` behind it (fixed in
        // MessagesViewController); this is the backstop.
        //
        // THE TEST IS "DID I SEAL THESE BYTES", NOT RULE P.
        //
        // Round 22 first asked Rule P whether the sent chain outranked the base
        // - and that was wrong, because Rule P cannot answer this question.
        // `msg_rule_p`'s own comment says so: a child can seal to a turn LOWER
        // than its parent's (the atom fold supersedes pending goods), and only
        // rule 4's parent-digest link can order such a pair - a link it also
        // documents as blind to descent at two removes. Measured on the rig,
        // 1.0(24) refused an ordinary pickup: base=[t6 r1] sent=[t5 r1],
        // rank=-1. The board then kept its staged move forever, never released
        // the withheld settlement, and `sending` stood - a board stuck mid-send.
        // A guard against a rare wrong rebase became a common broken one.
        //
        // The exact answer needs no ordering. The chain being sent is the chain
        // THIS controller sealed; `stagedPayload` is the only thing that makes
        // one, and it keeps the bytes. Anything else is a payload we did not
        // build, and the conservative move is to leave the board alone. Nil
        // `lastSealed` means this controller has sealed nothing yet and has no
        // opinion - a reload can legitimately hand it a chain it did not make.
        if let sent, let mine = lastSealed, sent != mine {
            // WHAT differed, not just that something did. Equal lengths with a
            // differing prefix is a STALE chain (the value-transport hazard the
            // substitution above covers); different lengths is a different game
            // or a truncation, which is a different bug entirely.
            FlightRecorder.note("send-foreign",
                "in=\(sent.count)b/\(sent.prefix(4).map { String(format: "%02x", $0) }.joined()) "
              + "mine=\(mine.count)b/\(mine.prefix(4).map { String(format: "%02x", $0) }.joined())")
            if AnimLog.on {
                let ce = try? await kernel.peek(payload: mine)
                let se = try? await kernel.peek(payload: sent)
                AnimLog.say("markSent REFUSED - not my bytes. "
                    + "mine=[t\(ce?.turn ?? -1) r\(ce?.round ?? -1) actor\(ce?.lastActorSeat ?? -1)] "
                    + "sent=[t\(se?.turn ?? -1) r\(se?.round ?? -1) actor\(se?.lastActorSeat ?? -1)]")
            }
            // THE REBASE IS REFUSED; THE RELEASE IS NOT. The human pressed Send,
            // so the move can no longer be undone or its bubble deleted, and
            // holding its settlement past that point is a board that cannot be
            // moved by any input at all. See `releaseHoldForSend`.
            sending = false
            releaseHoldForSend()
            await refresh()
            return
        }
        // AND A REFUSAL CHANGES NOTHING. Not the base, and not `pending`
        // either: the staged moves are what the board is DRAWN from, so
        // dropping them while declining to rebase would walk the board back by
        // exactly the move the player just watched - a smaller version of the
        // same complaint. `sending` stays up too, so the move cannot be undone
        // or re-sent while we do not know what went out. Playing on clears it
        // (`apply`), as does an arrival.
        //
        // The other alternative - re-deriving the sent bytes from base+pending -
        // was rejected: a re-seal stamps a fresh send clock, so it would be a
        // DIFFERENT chain with a different digest from the one the thread
        // actually received, and the next move would name a parent nobody has.

        // READ THE NEW CHAIN FIRST, MUTATE AFTERWARDS - and mutate all of it
        // without an await in between.
        //
        // This used to empty `pending`, then suspend on the decode, then move
        // `base`. Between those two lines the controller said "nothing staged,
        // still on the OLD chain" - and since round 22 `base` + `pending` is
        // exactly what the board is rebuilt from, so any publish landing in
        // that window rendered THE BOARD BEFORE THE MOVE, with
        // `animAtomsBefore` still pointing at the previous bubble so it
        // animated that bubble's move to get there. 1.0(23), twice: "i just did
        // a cover. whwn i sent it strangely animated to the state before the
        // cover", and a round-ending good that "did some weird animation
        // sequence that ended up back with the ace of clubs covering the king
        // of clubs again". Reopening the bubble put it right because a cold
        // open has no such window.
        //
        // It was invisible before round 22 only by luck: `publish` read the
        // RESIDENT game, which still had the staged moves applied to it, so a
        // half-updated controller still rendered correctly. Making base+pending
        // the single source of truth is what turned the window lethal.
        //
        // A payload that will not DECODE leaves everything alone rather than
        // emptying `pending` on the way to a base it never reaches: that is the
        // same walk backwards, with no race needed to produce it at all.
        var adopted: MessageEnvelope?
        if let sent {
            adopted = try? await kernel.decode(payload: sent, viewer: mySeat)
            guard adopted != nil else {
                FlightRecorder.note("send-unreadable", "kept the board on its staged move")
                AnimLog.say("markSent: the sent bytes will not decode - board unchanged")
                // The board keeps its staged move, but not its held settlement
                // and not the send window - see `releaseHoldForSend`.
                sending = false
                releaseHoldForSend()
                await refresh()
                return
            }
        }
        // ---- NO `await` PAST THIS LINE until the state is whole again ----
        // `sending` is cleared before the guard below: one left standing by an
        // ordinary early return is an Undo pill that never comes back.
        sending = false
        let hadPending = !pending.isEmpty
        guard hadPending || sent != nil else { return }
        // NO BYTES AT ALL, AND MOVES STILL STAGED: keep them. This is the shape
        // the report above walked in on, and the answer is the same one a
        // refusal gets - the staged moves are what the board is DRAWN from, so
        // dropping them without a base to replace them walks the board back by
        // exactly the move the player just watched. Only reachable now if a
        // seal never happened (`lastSealed` nil with `pending` full), which the
        // staging path does not allow; kept as the floor under a value that has
        // gone missing once already.
        guard let sent else {
            FlightRecorder.note("send-blind", "kept the staged move - no chain to rebase onto")
            releaseHoldForSend()
            await refresh()
            return
        }
        pending = []
        lastChangeWasUndo = false
        if let env = adopted {
            base = .continuation(payload: sent)
            parent8 = Self.firstEight(hex: env.digest)
            // My own seal appended my nickname if the parent had not named me
            // yet (`sealJoins`), so the roster comes back from the wire rather
            // than being patched here - the same way `adopt` takes it.
            joins = env.joins
            names = Dictionary(env.joins.map { ($0.seat, $0.name) },
                               uniquingKeysWith: { a, _ in a })
            adoptBaseFacts(env)
            // The base MOVING is the fact the animation boundary follows, so it
            // belongs in the trail beside the stream that then runs. A send that
            // replays the bubble before it is either this note missing (the
            // rebase never happened) or this note carrying the old boundary.
            FlightRecorder.note("send-rebase", "turn \(env.turn) atoms \(env.atomsBefore)")
        }
        // THE HELD SETTLEMENT IS RELEASED HERE, and only here. The move is in
        // the thread now: there is no undo left and no bubble left to delete,
        // so the deal it dealt is finally the player's to see. The board picks
        // these up on the view change the `refresh` below publishes.
        releaseHoldForSend()
        await refresh()
        // Round-8 #4: the final move is committed to the thread (no undo left),
        // so this game's stored hand arrangement has nothing left to order.
        if view?.isOver == true { store.clearHandOrder(gameId: gameIdString) }
    }

    /// Undo the last staged action by rebuilding the base and replaying all but
    /// the last pending action (§10). No-op if nothing is pending.
    public func undo() async {
        // Same guard as `apply`: the retraction IS an undo of everything
        // staged, already in flight - a second one racing it would fly cards
        // that are already flying.
        guard !conflictRetracting else { return }
        guard !pending.isEmpty else { return }
        // ONE assignment, no await, no replay.
        //
        // This used to empty `pending`, rebuild the base, and then re-apply the
        // survivors one at a time - so for the length of that rebuild the
        // controller said "this chain, nothing staged", and every step of the
        // replay was another partial state. Since round 22 the board is rebuilt
        // from `base` + `pending` on every read, which makes all of that both
        // redundant (nothing depends on what the kernel happens to hold) and
        // dangerous (each intermediate is a board a publish can land on). The
        // survivors were legal as a whole list, so they are legal as a prefix
        // of it - dropping the last one cannot invalidate the ones before it.
        let keep = Array(pending.dropLast())
        pending = keep
        // The undone move may be the one whose settlement is being held. The
        // board no longer contains it, so the hold has nothing left to
        // describe - dropped, not released.
        dropHold()
        // An earlier staged move can END A BOUT just as well as the undone one
        // did (cover, cover-that-swept, undo the throw-in that followed): what
        // survives the undo is held on exactly the same terms.
        if !keep.isEmpty { await captureSettlement() }
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
    /// ROUND 20 SEALS THE CHAIN THIS CONTROLLER IS PLAYING, not whatever game
    /// happens to be resident when the send lands.
    ///
    /// It used to call `seal` directly, which reads the kernel's ONE resident
    /// game - and between the move and the send this board makes several
    /// separate trips through that actor, any of which a tap, a reload or a
    /// bubble snapshot can decode a different chain into. In a rematch that
    /// other chain is a WAITING lobby dealt at capacity EIGHT, and the bubble
    /// that came out was that lobby's untouched deal wearing this board's
    /// roster: eight hands of six, deck 4, seats 4-8 reading "Seat N" - on every
    /// screen in the chat, because it named the live chain as its parent and
    /// Rule P's child rule ranks a child above it.
    ///
    /// `resealFromBase` rebuilds from THIS controller's own base and pending
    /// moves and seals in one uninterruptible actor call, so the game it
    /// describes is the game those moves were made on by construction. Phase is
    /// decided in there, after the replay, for the same reason.
    /// The bytes this controller most recently SEALED - the only chain it can
    /// vouch for as "the one I am sending". See `markSent`.
    private var lastSealed: Data?

    public func stagedPayload(sentAt: Int = MessageKernel.clockNow()) async throws -> Data {
        FlightRecorder.note("seal", "\(pending.count) staged")
        do {
            let sealed = try await kernel.resealFromBase(sealBase, replaying: pending,
                                                   seat: mySeat,
                                                   gameId: gameId,
                                                   parent8: parent8,
                                                   joins: sealJoins,
                                                   sentAt: sentAt)
            lastSealed = sealed
            return sealed
        } catch {
            // The callers stage with `try?`, so a refusal is SILENT - which is
            // the right behaviour (staging nothing beats staging a bubble that
            // describes another game) and the wrong diagnostics. Leave a trail:
            // this is the one place that knows a send was refused and why, and
            // the flight recorder is the only evidence a field report can carry.
            FlightRecorder.note("seal-failed", "\(error)")
            throw error
        }
    }

    /// This controller's base, in the form the kernel's atomic reseal takes.
    /// The same two cases `rebuildBase` switches on - an undo already rebuilds
    /// from exactly these bytes, which is what makes replaying them at seal time
    /// a re-derivation rather than a second opinion.
    private var sealBase: MessageKernel.SealBase {
        switch base {
        case .continuation(let payload): return .continuation(payload: payload)
        case .genesis(let seed, let players): return .genesis(seed: seed, players: players)
        }
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
