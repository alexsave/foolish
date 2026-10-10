// OnlineService.swift — quick-match / join-by-code / spectate (§6, §16.D5).
//
// The wire is now fully resolved from the web client (docs/PROTOCOL.md):
// - `create` returns the creator's enveloped packed game (octet-stream). We
//   decode it (PackedGame) to learn the game id + our seat + the initial view —
//   the same thing decodePackedGame does on the web (ServerContext.tsx:433).
// - `join`/`spectate` only need the game id up front; the seat and state arrive
//   on the `player_views` / `spectator_views` feed (OnlineGame learns them).
// - Every lobby request is an ask (c/src/ask.h) POSTed to `meta` as bytes the
//   kernel wrote (AskWire). Nothing here is JSON: the server reads no JSON, and
//   a request with nothing to say (`create`) has no body at all.

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
        // No body: a new game takes the caller's identity, which the token carries.
        let data: Data = try await client.functions.invoke(
            "create", options: FunctionInvokeOptions(method: .post)
        ) { data, _ in data }
        guard let decoded = await PackedGame.decode(data, engine: engine) else {
            throw OnlineError.badResponse
        }
        return OnlineGame(userId: userId, gameId: decoded.gameId, initial: decoded)
    }

    /// Join a game by id/code (the same parser as the universal-link route). The
    /// seat + state arrive on the feed; we only need the id to subscribe.
    public func join(gameId rawId: String, userId: UUID) async throws -> OnlineGame {
        let gameId = Self.normalizeGameId(rawId)
        try await ask(.join, gameId: gameId)
        return OnlineGame(userId: userId, gameId: gameId)
    }

    /// Spectate a game by id (public feed; no fan, no actions).
    public func spectate(gameId rawId: String, userId: UUID) -> OnlineGame {
        OnlineGame(userId: userId, gameId: Self.normalizeGameId(rawId), spectator: true)
    }

    /// Start a game in the lobby (deal cards). The web's `meta type:'start'`
    /// (ServerContext.startGame). Any seated player may start.
    public func start(gameId: String) async throws {
        try await ask(.start, gameId: gameId)
    }

    /// Add a bot to fill a seat (web `meta type:'add-bot'`). `botId` nil lets the
    /// server pick from the seeded roster.
    public func addBot(gameId: String, botId: String? = nil) async throws {
        try await ask(.addBot, gameId: gameId, bot: botId)
    }

    /// Continue after a finished game — resets the same game back to its lobby for
    /// a rematch (web `meta type:'continue'`, ServerContext.continueGame).
    public func continueGame(gameId: String) async throws {
        try await ask(.continueGame, gameId: gameId)
    }

    /// Leave a game / lobby (web `meta type:'exit'`).
    public func leave(gameId: String, userId: UUID) async throws {
        try await ask(.exit, gameId: gameId, player: userId.uuidString.lowercased())
    }

    static func normalizeGameId(_ raw: String) -> String {
        raw.trimmingCharacters(in: .whitespacesAndNewlines)
            .replacingOccurrences(of: "https://foolish.cards/", with: "")
            .replacingOccurrences(of: "http://foolish.cards/", with: "")
    }

    /// POST one ask to `meta`. The body is the kernel's (AskWire); a `Data` body
    /// goes out as application/octet-stream, which is the only body the server
    /// reads. The answer is the caller's envelope, which the feed delivers
    /// anyway, so it is not read here.
    func ask(_ kind: AskWire.Kind, gameId: String, bot: String? = nil, player: String? = nil) async throws {
        guard let client else { throw OnlineError.notConfigured }
        guard let body = AskWire.body(kind, gameId: gameId, bot: bot, player: player) else { throw OnlineError.badRequest }
        try await client.functions.invoke("meta", options: FunctionInvokeOptions(method: .post, body: body))
    }

    public enum OnlineError: Error, LocalizedError {
        case notConfigured, badResponse, badRequest
        public var errorDescription: String? {
            switch self {
            case .notConfigured: return "Online play isn’t configured in this build."
            case .badResponse: return "The server sent an unreadable response."
            case .badRequest: return "That game code isn’t one the server can take."
            }
        }
    }
}
