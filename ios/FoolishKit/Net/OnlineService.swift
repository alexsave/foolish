// OnlineService.swift — quick-match / join-by-code entry points (§6, §16.D5).
// Creates or joins a game via the edge functions and returns an OnlineGame bound
// to the resulting game id + local seat.
//
// PROTOCOL SEAM — TODO(D0): `create` returns the creator's packed masked view
// (create/index.ts), and the game id is generated server-side. How the client
// learns the game id from that response (a header? a field decoded from the
// view? a follow-up meta read?) must be pinned from the web client
// (src/contexts/ServerContext.tsx:423). `gameId(fromCreateResponse:)` is that one
// seam; everything else — auth gating, the OnlineGame wiring, the packed-action
// POST, the realtime decode — is complete. Until the seam is filled, quickMatch
// throws a clear, non-silent error.

import Foundation
import Supabase

@MainActor
public final class OnlineService {
    public static let shared = OnlineService()
    private init() {}

    private var client: SupabaseClient? { Backend.shared.client }

    /// Quick-match: create a game and return a session on it. The creator is
    /// always seat 0.
    public func quickMatch(userId: UUID) async throws -> OnlineGame {
        guard let client else { throw OnlineError.notConfigured }
        let data: Data = try await client.functions.invoke("create", options: FunctionInvokeOptions(method: .post))
        let gameId = try Self.gameId(fromCreateResponse: data)
        return OnlineGame(gameId: gameId, userId: userId, humanSeat: 0)
    }

    /// Join a game by its id/code (the same parser as the universal-link route).
    /// The local seat comes from the join response's player roster.
    public func join(gameId rawId: String, userId: UUID) async throws -> OnlineGame {
        guard client != nil else { throw OnlineError.notConfigured }
        let gameId = Self.normalizeGameId(rawId)
        // TODO(D0): call `meta` { type: 'join', game_id } and read the local
        // player's seat from the response roster. Defaulting to a spectator seat
        // until pinned keeps the flow honest.
        let seat = try await joinSeat(gameId: gameId, userId: userId)
        return OnlineGame(gameId: gameId, userId: userId, humanSeat: seat)
    }

    /// Spectate a game by id.
    public func spectate(gameId rawId: String, userId: UUID) -> OnlineGame {
        OnlineGame(gameId: Self.normalizeGameId(rawId), userId: userId, humanSeat: -1, spectator: true)
    }

    // MARK: - seams (TODO(D0))

    static func normalizeGameId(_ raw: String) -> String {
        raw.trimmingCharacters(in: .whitespacesAndNewlines)
            .replacingOccurrences(of: "https://foolish.cards/", with: "")
            .replacingOccurrences(of: "http://foolish.cards/", with: "")
    }

    static func gameId(fromCreateResponse data: Data) throws -> String {
        // TODO(D0): decode the game id from the create response per the web wire.
        throw OnlineError.protocolSeam("create → game id extraction (docs/PROTOCOL.md §2.1)")
    }

    private func joinSeat(gameId: String, userId: UUID) async throws -> Int {
        // TODO(D0): meta 'join' → the local player's seat index.
        throw OnlineError.protocolSeam("meta join → local seat (docs/PROTOCOL.md §7)")
    }

    public enum OnlineError: Error, LocalizedError {
        case notConfigured
        case protocolSeam(String)
        public var errorDescription: String? {
            switch self {
            case .notConfigured: return "Online play isn’t configured in this build."
            case .protocolSeam(let what): return "Online play needs the backend wire finalized: \(what)."
            }
        }
    }
}
