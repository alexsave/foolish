// OnlineService.swift — quick-match / join-by-code / spectate (§6, §16.D5).
//
// The wire is now fully resolved from the web client (docs/PROTOCOL.md):
// - `create` returns the creator's enveloped packed game (octet-stream). We
//   decode it (PackedGame) to learn the game id + our seat + the initial view —
//   the same thing decodePackedGame does on the web (ServerContext.tsx:433).
// - `join`/`spectate` only need the game id up front; the seat and state arrive
//   on the `player_views` / `spectator_views` feed (OnlineGame learns them).

import Foundation
import Supabase
import FoolishKit

@MainActor
public final class OnlineService {
    public static let shared = OnlineService()
    private init() {}

    private let engine = EngineC()
    private var client: SupabaseClient? { Backend.shared.client }

    /// Quick-match: create a game and return a session seeded from the response.
    public func quickMatch(userId: UUID) async throws -> OnlineGame {
        guard let client else { throw OnlineError.notConfigured }
        // Raw-bytes overload: `create` returns the packed envelope, not JSON.
        let data: Data = try await client.functions.invoke(
            "create", options: FunctionInvokeOptions(method: .post, body: EmptyBody())
        ) { data, _ in data }
        guard let decoded = await PackedGame.decode(data, engine: engine) else {
            throw OnlineError.badResponse
        }
        return OnlineGame(userId: userId, gameId: decoded.gameId, initial: decoded)
    }

    /// Join a game by id/code (the same parser as the universal-link route). The
    /// seat + state arrive on the feed; we only need the id to subscribe.
    public func join(gameId rawId: String, userId: UUID) async throws -> OnlineGame {
        guard let client else { throw OnlineError.notConfigured }
        let gameId = Self.normalizeGameId(rawId)
        struct JoinBody: Encodable { let type = "join"; let game_id: String }
        try await client.functions.invoke(
            "meta", options: FunctionInvokeOptions(method: .post, body: JoinBody(game_id: gameId))
        )
        return OnlineGame(userId: userId, gameId: gameId)
    }

    /// Spectate a game by id (public feed; no fan, no actions).
    public func spectate(gameId rawId: String, userId: UUID) -> OnlineGame {
        OnlineGame(userId: userId, gameId: Self.normalizeGameId(rawId), spectator: true)
    }

    /// Start a game in the lobby (deal cards). The web's `meta type:'start'`
    /// (ServerContext.startGame). Any seated player may start.
    public func start(gameId: String) async throws {
        guard let client else { throw OnlineError.notConfigured }
        struct StartBody: Encodable { let type = "start"; let game_id: String }
        try await client.functions.invoke(
            "meta", options: FunctionInvokeOptions(method: .post, body: StartBody(game_id: gameId)))
    }

    /// Add a bot to fill a seat (web `meta type:'add-bot'`). `botId` nil lets the
    /// server pick from the seeded roster.
    public func addBot(gameId: String, botId: String? = nil) async throws {
        guard let client else { throw OnlineError.notConfigured }
        struct BotBody: Encodable { let type = "add-bot"; let game_id: String; let bot_id: String? }
        try await client.functions.invoke(
            "meta", options: FunctionInvokeOptions(method: .post, body: BotBody(game_id: gameId, bot_id: botId)))
    }

    /// Continue after a finished game — resets the same game back to its lobby for
    /// a rematch (web `meta type:'continue'`, ServerContext.continueGame).
    public func continueGame(gameId: String) async throws {
        guard let client else { throw OnlineError.notConfigured }
        struct ContinueBody: Encodable { let type = "continue"; let game_id: String }
        try await client.functions.invoke(
            "meta", options: FunctionInvokeOptions(method: .post, body: ContinueBody(game_id: gameId)))
    }

    /// Leave a game / lobby (web `meta type:'exit'`).
    public func leave(gameId: String, userId: UUID) async throws {
        guard let client else { throw OnlineError.notConfigured }
        struct ExitBody: Encodable { let type = "exit"; let game_id: String; let player_id: String }
        try await client.functions.invoke(
            "meta", options: FunctionInvokeOptions(method: .post,
                                                   body: ExitBody(game_id: gameId, player_id: userId.uuidString.lowercased())))
    }

    static func normalizeGameId(_ raw: String) -> String {
        raw.trimmingCharacters(in: .whitespacesAndNewlines)
            .replacingOccurrences(of: "https://foolish.cards/", with: "")
            .replacingOccurrences(of: "http://foolish.cards/", with: "")
    }

    private struct EmptyBody: Encodable {}

    public enum OnlineError: Error, LocalizedError {
        case notConfigured, badResponse
        public var errorDescription: String? {
            switch self {
            case .notConfigured: return "Online play isn’t configured in this build."
            case .badResponse: return "The server sent an unreadable response."
            }
        }
    }
}
