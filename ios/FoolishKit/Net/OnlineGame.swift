// OnlineGame.swift — the online game session (§8, §16.D5). Conforms to the same
// GameSession the board renders against, so ONE TableView drives online and
// offline. State comes from the server's masked-view feed (decoded through the
// kernel, never a Swift wire), and moves POST the packed action (PackedAction,
// byte-verified). Stage C1: no optimistic mutation — the played cards go
// in-flight (dim + lock, no movement) and animate from the authoritative
// broadcast; a reject clears them with a rigid haptic + toast (§8.2).
//
// PROTOCOL SEAMS — TODO(D0): the `create`/`meta` response shapes (how the game id
// and the local seat come back) and the broadcast payload keys are the parts
// that need the live wire / web client to finalize (docs/PROTOCOL.md). The
// verified primitives — packed-action encode, packed-view decode, the version
// gate, the auth email — are wired in and correct.

import Foundation
import Supabase

@MainActor
public final class OnlineGame: ObservableObject, GameSession {
    @Published public private(set) var view: GameView?
    @Published public private(set) var humanLegal: [Move] = []
    @Published public private(set) var actorMask: Int = 0
    @Published public private(set) var thinking: Bool = false
    @Published public private(set) var lastReject: EngineError?
    @Published public private(set) var foolSeat: Int?
    @Published public private(set) var inFlight: Set<String> = []

    public let humanSeat: Int
    public let gameId: String
    private let userId: UUID
    private let spectator: Bool

    private let engine = EngineC()
    private var feed: GameFeed?
    /// The client's current authoritative version — sent as `intent_version` so
    /// the server's stale-round guard can reject cross-round moves (§8.1).
    private var intentVersion: UInt32 = 0

    /// - Parameters:
    ///   - gameId: the game to join / spectate.
    ///   - userId: the local user (their seat is `humanSeat`).
    ///   - humanSeat: the local player's seat (from the create/join response).
    ///   - spectator: true to watch the public feed (no fan, no actions).
    public init(gameId: String, userId: UUID, humanSeat: Int, spectator: Bool = false) {
        self.gameId = gameId
        self.userId = userId
        self.humanSeat = humanSeat
        self.spectator = spectator
        start()
    }

    private func start() {
        let feed = GameFeed(onPacked: { [weak self] bytes, viewer, version in
            self?.ingest(bytes: bytes, viewer: viewer, version: version)
        })
        self.feed = feed
        Task {
            if spectator { await feed.subscribePublic(gameId: gameId) }
            else { await feed.subscribe(gameId: gameId, userId: userId, mySeat: humanSeat) }
        }
    }

    // MARK: - incoming state

    /// Decode a gated packed view through the kernel, publish view + legal moves,
    /// clear any in-flight cards the broadcast now confirms.
    private func ingest(bytes: Data, viewer: Int, version: Int?) {
        Task {
            guard let v = try? await engine.viewFromPacked(bytes, viewer: viewer) else { return }
            self.view = v
            if let version { self.intentVersion = UInt32(max(0, version)) }
            self.foolSeat = v.isOver ? v.gameOver : nil
            // Kernel-computed legal moves from the masked view (own hand is real).
            if !spectator {
                self.humanLegal = (try? await engine.legalFromPacked(bytes, seat: humanSeat)) ?? []
            }
            self.actorMask = Self.actorMask(from: v)
            self.inFlight.removeAll()      // authoritative state supersedes in-flight
            self.thinking = !spectator && !v.isOver && (self.actorMask & (1 << humanSeat)) == 0
        }
    }

    /// Derive the actor mask from a view: the defender when battles are
    /// uncovered, else the attackers. The kernel owns the real rule offline; for
    /// the online masked view this is a display hint only (enable-states come
    /// from `humanLegal`), so a coarse derivation is acceptable.
    private static func actorMask(from v: GameView) -> Int {
        var mask = 0
        let hasUncovered = v.battles.contains { $0.defense == nil }
        if hasUncovered { mask |= (1 << v.defender) }
        else { mask |= (1 << v.firstAttacker) }
        return mask
    }

    // MARK: - outgoing moves (Stage C1)

    public func play(_ move: Move) {
        guard let client = Backend.shared.client else { return }
        // In-flight affordance: dim + lock the touched cards; do NOT move them.
        for c in move.cards { inFlight.insert(c.identity) }

        Task {
            do {
                let body = try PackedAction.requestBody(gameId: gameId, intentVersion: intentVersion, move: move)
                let data: Data = try await client.functions.invoke(
                    "action",
                    options: FunctionInvokeOptions(method: .post, body: body)
                )
                let resp = try PackedAction.decodeResponse(data)
                switch resp.status {
                case .applied:
                    intentVersion = resp.version
                    // The authoritative broadcast will animate + clear in-flight.
                case .rejected:
                    clearInFlight(move)
                    lastReject = resp.isStaleRound ? .reject(code: Int(PackedAction.rejectStaleRound))
                                                   : .reject(code: Int(resp.rejectCode))
                case .moot:
                    clearInFlight(move)   // already applied by another path
                }
            } catch {
                clearInFlight(move)
                lastReject = (error as? EngineError) ?? .unknown(-1)
            }
        }
    }

    private func clearInFlight(_ move: Move) {
        for c in move.cards { inFlight.remove(c.identity) }
    }

    // MARK: - lifecycle

    /// Refetch authoritative state on foreground/reconnect (§16.D6). The exact
    /// `meta`/fetch call is a TODO(D0) seam; the resync flow (reset gate →
    /// refetch → resubscribe) is here.
    public func resync() async {
        feed?.resetVersionGate()
        // TODO(D0): call `meta` to refetch the current masked view, then feed it
        // through `feed?.feed(version:packedViewHex:viewer:)`.
    }

    public func makeShareURL() async -> URL? {
        // Online replay codes are minted server-side at game end (finalizeEndedGame);
        // the share flow surfaces that code. TODO(D0): read it from the meta feed.
        nil
    }

    deinit {
        let feed = self.feed
        Task { await feed?.unsubscribe() }
    }
}
