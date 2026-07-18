// OnlineGame.swift — the online game session (§8, §16.D5). Conforms to the same
// GameSession the board renders against, so ONE TableView drives online and
// offline. Authoritative masked state arrives as `player_views` rows (GameFeed),
// decoded through PackedGame (envelope → kernel). Moves POST the packed action
// (PackedAction, byte-verified). Stage C1: no optimistic mutation — the played
// cards go in-flight (dim + lock) and the authoritative row that follows
// supersedes them; a reject clears them with a rigid haptic + toast (§8.2).
//
// The local seat and game id are read from the decoded envelope (the web does
// the same via decodePackedGame) — no seat/id is threaded through the UI.

import Foundation
import Combine   // ObservableObject / @Published
import Supabase
import FoolishKit

@MainActor
public final class OnlineGame: ObservableObject, GameSession {
    @Published public private(set) var view: GameView?
    @Published public private(set) var humanLegal: [Move] = []
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
    private var feed: GameFeed?
    private var intentVersion: UInt32 = 0

    /// Quick-match seeds `initial` from the `create` response; join/spectate pass
    /// nil and learn everything from the first `player_views` row.
    public init(userId: UUID, gameId: String, spectator: Bool = false, initial: DecodedGame? = nil) {
        self.userId = userId
        self.gameId = gameId
        self.spectator = spectator
        self.seat = initial?.seat ?? -1
        if let initial { apply(initial) }
        start()
    }

    private func start() {
        let feed = GameFeed(onRow: { [weak self] hex, _ in self?.ingest(hex: hex) })
        self.feed = feed
        Task {
            if spectator { await feed.subscribePublic(gameId: gameId) }
            else { await feed.subscribe(userId: userId) }
        }
    }

    // MARK: - incoming state

    private func ingest(hex: String) {
        Task {
            guard let decoded = await PackedGame.decodeHex(hex, engine: engine) else { return }
            // The user's feed carries every game they're in — apply only ours.
            guard decoded.gameId == gameId else { return }
            apply(decoded)
            if !spectator {
                humanLegal = (try? await engine.legalFromPacked(decoded.stateBytes, seat: decoded.seat)) ?? []
            }
        }
    }

    private func apply(_ decoded: DecodedGame) {
        view = decoded.view
        seat = decoded.seat
        intentVersion = UInt32(max(0, decoded.version))
        // The fool is the kernel's call (game_done, emitted as gameOver) — no
        // rule derivation in Swift (§3).
        foolSeat = decoded.view.gameOver >= 0 ? decoded.view.gameOver : nil
        actorMask = Self.actorMask(from: decoded.view)
        inFlight.removeAll()   // authoritative state supersedes any in-flight
        thinking = !spectator && !decoded.view.isOver && (actorMask & (1 << max(seat, 0))) == 0
        // #24: player_views rows reach us over a fire-and-forget realtime push,
        // and the terminal (game-over) row is the worst to lose — the game ends
        // but this client is stranded on the table, WinView never shown. Every
        // authoritative update re-arms a watchdog; if the feed then goes silent
        // while the game is still live, we refetch once so a dropped push
        // self-heals (§16.D6). Game resolved → stop watching.
        if foolSeat == nil { armResyncWatchdog() } else { resyncWatchdog?.cancel() }
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
        guard let client = Backend.shared.client, !spectator else { return }
        for c in move.cards { inFlight.insert(c.identity) }
        Task {
            do {
                let body = try PackedAction.requestBody(gameId: gameId, intentVersion: intentVersion, move: move)
                // Raw-bytes overload — the action response is a BINARY envelope,
                // not JSON, so we must NOT let invoke run a JSONDecoder over it.
                let data: Data = try await client.functions.invoke(
                    "action", options: FunctionInvokeOptions(method: .post, body: body)
                ) { data, _ in data }
                let resp = try PackedAction.decodeResponse(data)
                switch resp.status {
                case .applied:
                    intentVersion = resp.version   // the pv- row will animate + clear in-flight
                case .rejected:
                    clearInFlight(move)
                    lastReject = .reject(code: Int(resp.rejectCode))
                case .moot:
                    clearInFlight(move)
                }
            } catch {
                clearInFlight(move)
                lastReject = (error as? EngineError) ?? .unknown(-1)
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

    /// Nudge the bot loop where the web does (ServerContext.tsx:633) — a JSON
    /// `action` body, not a packed move.
    public func bumpBots() {
        guard let client = Backend.shared.client else { return }
        struct Bump: Encodable { let game_id: String; let type = "bump" }
        Task { _ = try? await client.functions.invoke("action",
            options: FunctionInvokeOptions(method: .post, body: Bump(game_id: gameId))) }
    }

    /// Refetch authoritative state on foreground/reconnect (§16.D6): reset the
    /// gate and re-read the player_views row.
    public func resync() async {
        feed?.resetVersionGate()
        guard let client = Backend.shared.client, !spectator else { return }
        struct Row: Decodable { let view: String }
        let rows: [Row] = (try? await client.from("player_views")
            .select("view")
            .eq("game_id", value: gameId)
            .eq("player_id", value: userId.uuidString.lowercased())
            .execute().value) ?? []
        if let hex = rows.first?.view { ingest(hex: hex) }
    }

    /// How long the feed may go silent before we refetch (#24). Long enough that
    /// it never competes with a live realtime push (which re-arms it), short
    /// enough that a dropped terminal row surfaces WinView within a breath.
    private static let resyncTimeout: UInt64 = 9_000_000_000   // 9s

    private var resyncWatchdog: Task<Void, Never>?

    /// Arm/re-arm the silence watchdog. Every authoritative update calls this, so
    /// while rows keep arriving the timer keeps resetting and never fires; only a
    /// genuine silence (a lost push) trips it. On trip we refetch once and keep
    /// watching until the game resolves. No-op for spectators (resync refetches
    /// only a player's own row).
    private func armResyncWatchdog() {
        resyncWatchdog?.cancel()
        guard !spectator else { return }
        resyncWatchdog = Task { @MainActor [weak self] in
            try? await Task.sleep(nanoseconds: OnlineGame.resyncTimeout)
            guard let self, !Task.isCancelled, self.foolSeat == nil else { return }
            await self.resync()          // a dropped row (esp. the terminal one) self-heals
            self.armResyncWatchdog()     // keep watching until the game is over
        }
    }

    public func makeShareURL() async -> URL? {
        // Online replay codes are minted server-side at game end (finalizeEndedGame,
        // stored in game_snapshots). Surfacing that code is a follow-up (§17.7).
        nil
    }

    deinit {
        resyncWatchdog?.cancel()
        let feed = self.feed
        Task { await feed?.unsubscribe() }
    }
}
