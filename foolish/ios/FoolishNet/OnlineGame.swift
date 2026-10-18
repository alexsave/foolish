// OnlineGame.swift — the online game session (§8, §16.D5). Conforms to the same
// GameSession the board renders against, so ONE TableView drives online and
// offline. Authoritative masked state arrives as the player's views on the push
// stream (Stream), read by the kernel (EngineC.adoptEnvelope). A move is the
// kernel's action wire, posted through the kernel's client (NetClient). Stage
// C1: no optimistic mutation — the played cards go in-flight (dim + lock) and
// the authoritative view that follows supersedes them; a reject clears them
// with a rigid haptic + toast (§8.2).
//
// The local seat and game id are read from the decoded envelope (the web does
// the same) — no seat/id is threaded through the UI.

import Foundation
import Combine   // ObservableObject / @Published
import FoolishKit

@MainActor
public final class OnlineGame: ObservableObject, GameSession {
    @Published public private(set) var view: GameView?
    @Published public private(set) var humanLegal: [Move] = []
    /// The same menu as the kernel wrote it - what the board's play rules read
    /// (PlayWire / fio_play_probe). See GameSession.
    @Published public private(set) var humanLegalPacked: Data = MoveWire.emptyMenu
    @Published public private(set) var actorMask: Int = 0
    @Published public private(set) var thinking: Bool = false
    @Published public private(set) var lastReject: EngineError?
    @Published public private(set) var foolSeat: Int?
    @Published public private(set) var inFlight: Set<String> = []

    public let gameId: String
    private let userId: UUID
    private let spectator: Bool
    /// Learned from the decoded envelope; -1 until the first row arrives.
    @Published private var seat: Int
    public var humanSeat: Int { seat }

    private let engine = EngineC()
    private var intentVersion: UInt32 = 0

    /// Quick-match seeds `initial` from the `create` response; join/spectate pass
    /// nil and learn everything from the first view the stream delivers.
    public init(userId: UUID, gameId: String, spectator: Bool = false, initial: AdoptedEnvelope? = nil) {
        self.userId = userId
        self.gameId = gameId
        self.spectator = spectator
        self.seat = initial?.seat ?? -1
        if let initial { apply(initial) }
        start()
    }

    private func start() {
        Stream.shared.watch(gameId, spectator: spectator) { [weak self] envelope in self?.ingest(envelope) }
        if spectator { Task { await resync() } }
    }

    // MARK: - incoming state

    private func ingest(_ envelope: Data) {
        Task {
            guard let decoded = await engine.adoptEnvelope(envelope) else { return }
            // The user's feed carries every game they're in — apply only ours.
            guard decoded.gameId == gameId else { return }
            apply(decoded)
            if !spectator {
                // The menu came off the SAME adopt as the board: the kernel has
                // one client slot, so asking for it afterwards would be asking
                // about whatever had been adopted since.
                humanLegalPacked = decoded.legalPacked
                humanLegal = MoveWire.decode(decoded.legalPacked)
            }
        }
    }

    /// Adopt one authoritative envelope. Internal, not private, so a test can
    /// hand the session the row the feed would (OnlineSessionViewTests).
    func apply(_ decoded: AdoptedEnvelope) {
        view = decoded.view
        seat = decoded.seat
        intentVersion = UInt32(max(0, decoded.version))
        // The fool is the kernel's call (game_done, emitted as gameOver) — no
        // rule derivation in Swift (§3).
        foolSeat = decoded.view.gameOver >= 0 ? decoded.view.gameOver : nil
        actorMask = Self.actorMask(from: decoded.view)
        inFlight.removeAll()   // authoritative state supersedes any in-flight
        thinking = !spectator && !decoded.view.isOver && (actorMask & (1 << max(seat, 0))) == 0
    }

    /// Display hint only — enable-states come from `humanLegal` (kernel-computed).
    private static func actorMask(from v: GameView) -> Int {
        var mask = 0
        if v.battles.contains(where: { $0.defense == nil }) { mask |= (1 << v.defender) }
        else { mask |= (1 << v.firstAttacker) }
        return mask
    }

    // MARK: - outgoing moves (Stage C1)

    public func play(_ move: Move) {
        guard Backend.shared.isConfigured, !spectator else { return }
        let wire = MoveWire.encodeAction(move)
        guard !wire.isEmpty else { lastReject = .badArg; return }
        for c in move.cards { inFlight.insert(c.identity) }
        Task {
            guard let resp = try? await Backend.shared.ask(NC_DO_PLAY, gameId: gameId, bytes: [UInt8](wire),
                                                           intent: Int64(intentVersion)).done else {
                clearInFlight(move)
                lastReject = .unknown(-1)
                return
            }
            switch resp.actionStatus {
            case TABLE_STATUS_APPLIED:
                intentVersion = UInt32(resp.version)   // the view that follows animates + clears in-flight
            case TABLE_STATUS_REJECTED:
                clearInFlight(move)
                lastReject = .reject(code: resp.rejectCode)
            default:
                clearInFlight(move)
            }
        }
    }

    private func clearInFlight(_ move: Move) { for c in move.cards { inFlight.remove(c.identity) } }

    // MARK: - lifecycle

    // MARK: - Lobby actions (§16.D5)

    /// The game is in its lobby (pre-deal) — the LobbyView renders here.
    public var isWaiting: Bool { view?.gameStatus == .waiting }
    /// This user occupies a seat (vs. spectating / not yet joined).
    public var isSeated: Bool { !spectator && view?.me != nil }

    /// Mark ready + start (web ties Ready to `meta start`; the server deals once
    /// every seat is ready).
    public func ready() { Task { try? await OnlineService.shared.start(gameId: gameId) } }
    /// Add a bot to a lobby seat.
    public func addBot() { Task { try? await OnlineService.shared.addBot(gameId: gameId) } }
    /// Take a seat in a game being spectated (join by code from the lobby).
    public func joinSelf() { Task { _ = try? await OnlineService.shared.join(gameId: gameId, userId: userId) } }
    /// After a finished game, reset to the lobby for a rematch (Continue).
    public func continueGame() { Task { try? await OnlineService.shared.continueGame(gameId: gameId) } }
    /// Leave the game / lobby.
    public func leave() { Task { try? await OnlineService.shared.leave(gameId: gameId, userId: userId) } }

    /// Nudge the bot loop where the web does (ServerContext loadGameInternal):
    /// an ask to `meta` (c/src/ask.h ASK_BUMP), not a move.
    public func bumpBots() {
        Task { try? await OnlineService.shared.ask(.bump, gameId: gameId) }
    }

    /// Refetch authoritative state on foreground/reconnect (§16.D6): this game's
    /// view (the spectators' for a spectator). No envelope when it is stale: a
    /// newer one was applied meanwhile (the kernel's client keeps the order).
    public func resync() async {
        guard let envelope = try? await Backend.shared.ask(NC_DO_OPEN_GAME, gameId: gameId).envelope else { return }
        ingest(envelope)
    }

    public func makeShareURL() async -> URL? {
        // Online replay codes are minted server-side at game end (finalizeEndedGame,
        // stored in game_snapshots). Surfacing that code is a follow-up (§17.7).
        nil
    }

    deinit {
        let (gameId, spectator) = (self.gameId, self.spectator)
        Task { @MainActor in Stream.shared.unwatch(gameId, spectator: spectator) }
    }
}
