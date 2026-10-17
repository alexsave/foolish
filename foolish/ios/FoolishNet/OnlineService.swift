// OnlineService.swift - create, join by code, spectate, and the lobby asks
// (§6, §16.D5).
//
// `create` answers with the creator's envelope, which the kernel reads for the
// game id, the seat and the first view. Every lobby request is an ask
// (c/src/ask.h) written by the kernel (AskWire); what it changed arrives on the
// stream.

import Foundation
import FoolishKit

@MainActor
public final class OnlineService {
    public static let shared = OnlineService()
    private init() {}

    private let engine = EngineC()

    /// Quick-match: create a game and return a session seeded from the response.
    public func quickMatch(userId: UUID) async throws -> OnlineGame {
        let answer = try await Backend.shared.ask(NC_DO_CREATE)
        guard let envelope = answer.envelope, let decoded = await engine.adoptEnvelope(envelope) else { throw NetError.unreadable }
        return OnlineGame(userId: userId, gameId: decoded.gameId, initial: decoded)
    }

    /// Join a game by id or link. The seat and state arrive on the stream.
    public func join(gameId rawId: String, userId: UUID) async throws -> OnlineGame {
        let gameId = Self.normalizeGameId(rawId)
        try await ask(.join, gameId: gameId)
        return OnlineGame(userId: userId, gameId: gameId)
    }

    /// Spectate a game by id (no seat, no actions).
    public func spectate(gameId rawId: String, userId: UUID) -> OnlineGame {
        OnlineGame(userId: userId, gameId: Self.normalizeGameId(rawId), spectator: true)
    }

    public func start(gameId: String) async throws { try await ask(.start, gameId: gameId) }
    public func addBot(gameId: String, botId: String? = nil) async throws { try await ask(.addBot, gameId: gameId, bot: botId) }
    public func continueGame(gameId: String) async throws { try await ask(.continueGame, gameId: gameId) }
    public func leave(gameId: String, userId: UUID) async throws {
        try await ask(.exit, gameId: gameId, player: userId.uuidString.lowercased())
    }

    static func normalizeGameId(_ raw: String) -> String {
        raw.trimmingCharacters(in: .whitespacesAndNewlines)
            .replacingOccurrences(of: "https://foolish.cards/", with: "")
            .replacingOccurrences(of: "http://foolish.cards/", with: "")
    }

    func ask(_ kind: AskWire.Kind, gameId: String, bot: String? = nil, player: String? = nil) async throws {
        guard let ask = AskWire.body(kind, gameId: gameId, bot: bot, player: player) else { throw NetError.unreadable }
        _ = try await Backend.shared.ask(NC_DO_ASK, bytes: [UInt8](ask))
    }
}
