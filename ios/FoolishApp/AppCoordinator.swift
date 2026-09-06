// AppCoordinator.swift — top-level navigation + the active offline game. Keeps
// SwiftUI navigation simple: a screen enum plus the live LocalGame. Online play
// (M-D) will add its session here alongside `offlineGame`.

import SwiftUI
import Foundation
import FoolishKit
import FoolishBots
import FoolishNet

/// A chosen offline setup: which bot fills the opponent seats, and how many.
public struct OfflineConfig: Equatable {
    public var opponentStrategyId: Int
    public var opponentName: String
    public var opponents: Int          // 1...3 → 2...4 players
    public init(opponentStrategyId: Int, opponentName: String, opponents: Int) {
        self.opponentStrategyId = opponentStrategyId
        self.opponentName = opponentName
        self.opponents = opponents
    }
}

@MainActor
final class AppCoordinator: ObservableObject {
    enum Screen: Equatable { case home, dashboard, table, onlineTable }

    @Published var screen: Screen = .home
    @Published private(set) var offlineGame: LocalGame?
    @Published private(set) var onlineGame: OnlineGame?
    @Published var onlineError: String?
    /// A replay opened from a universal link (foolish.cards/<code>), presented
    /// over whatever is on screen (§16.C5).
    @Published var pendingReplay: PendingReplay?

    private let linkEngine = EngineC()

    /// Deterministic-but-varied seed. Uses a monotonic counter + wall clock so
    /// each new offline game deals differently; the seed is 32 bytes so the deal
    /// uses the wide ChaCha path (reproducible if ever needed).
    private var seedCounter: UInt64 = 0

    func startOffline(_ config: OfflineConfig) {
        seedCounter &+= 1
        let players = config.opponents + 1
        var strategies: [Int: Int] = [:]
        var seatNames: [Int: String] = [:]
        // All opponents share one strategy today, so number the duplicates
        // ("Moscow 1/2/3"); a single opponent needs no index (§IOS_BOT_NAMING §3).
        let base = BotNames.display(strategy: config.opponentName)
        for seat in 1..<players {
            strategies[seat] = config.opponentStrategyId
            seatNames[seat] = config.opponents > 1 ? "\(base) \(seat)" : base
        }
        let game = LocalGame(seed: makeSeed(), players: players, humanSeat: 0,
                             strategies: strategies, seatNames: seatNames)
        offlineGame = game
        screen = .table
    }

    func rematch(_ config: OfflineConfig) { startOffline(config) }

    /// Create a new online game and land in its lobby (§16.D5). Requires a
    /// signed-in user; the caller gates on auth first.
    func createOnline(userId: UUID) {
        Task {
            do {
                let game = try await OnlineService.shared.quickMatch(userId: userId)
                onlineGame = game
                screen = .onlineTable   // waiting status → LobbyView
            } catch {
                onlineError = error.localizedDescription
            }
        }
    }

    /// Join an existing online game by code/id and land in its lobby.
    func joinOnline(code: String, userId: UUID) {
        Task {
            do {
                let game = try await OnlineService.shared.join(gameId: code, userId: userId)
                onlineGame = game
                screen = .onlineTable
            } catch {
                onlineError = error.localizedDescription
            }
        }
    }

    func goHome() {
        offlineGame = nil
        onlineGame = nil
        screen = .home
    }

    // MARK: - DEBUG online orchestration (two-simulator play, IOS_APP_DESIGN §17.10)

    /// Host: create a game, land on the table, wait `startAfter` for a guest to
    /// join, then start it. Optionally auto-play the human seat.
    func debugHost(userId: UUID, startAfter: TimeInterval, autoplay: Bool) {
        Task {
            do {
                let game = try await OnlineService.shared.quickMatch(userId: userId)
                onlineGame = game
                screen = .onlineTable
                NSLog("FOOLISH_ONLINE_GAME_ID=\(game.gameId)")
                try? await Task.sleep(nanoseconds: UInt64(startAfter * 1_000_000_000))
                try await OnlineService.shared.start(gameId: game.gameId)
                if autoplay { autoplayOnline() }
            } catch { onlineError = error.localizedDescription }
        }
    }

    /// Guest: join an existing game by id, land in its lobby, ready up, then
    /// (optionally) auto-play once the host starts.
    func debugGuest(userId: UUID, gameId: String, autoplay: Bool) {
        Task {
            do {
                let game = try await OnlineService.shared.join(gameId: gameId, userId: userId)
                onlineGame = game
                screen = .onlineTable
                try? await Task.sleep(nanoseconds: 4_000_000_000)
                game.ready()   // both seats must ready before the server deals
                if autoplay { autoplayOnline() }
            } catch { onlineError = error.localizedDescription }
        }
    }

    private func autoplayOnline() {
        Task { @MainActor in
            while !Task.isCancelled {
                try? await Task.sleep(nanoseconds: 700_000_000)
                guard let g = onlineGame, g.foolSeat == nil else { break }
                // Shed cards so the test game converges quickly: prefer
                // attack/cover, fall back to any legal move.
                let legal = g.humanLegal
                let shed = legal.filter { $0.type == .attack || $0.type == .cover }
                if let mv = (shed.randomElement() ?? legal.randomElement()) { g.play(mv) }
            }
        }
    }

    /// Route a universal link. `foolish.cards/<code>` collides with live-game
    /// URLs, so we route by DECODE SUCCESS (§16.C5): a valid replay code opens
    /// the replay viewer; `/m/*` (iMessage viewer) and bare game ids for
    /// spectating are handled once those milestones land (else the link falls
    /// back to opening in Safari, which the OS does when we don't consume it).
    func handle(url: URL) {
        let path = url.path.trimmingCharacters(in: CharacterSet(charactersIn: "/"))
        guard !path.isEmpty, !path.hasPrefix("m/") else { return }
        Task {
            if let decoded = try? await linkEngine.replayDecode(code: path) {
                pendingReplay = PendingReplay(replay: decoded)
            }
        }
    }

    private func makeSeed() -> Data {
        var bytes = [UInt8](repeating: 0, count: 32)
        let t = UInt64(Date().timeIntervalSince1970 * 1000) &+ seedCounter &* 0x9E3779B97F4A7C15
        for i in 0..<8 { bytes[i] = UInt8((t >> (8 * i)) & 0xFF) }
        for i in 8..<32 { bytes[i] = UInt8((t &* UInt64(i + 1)) & 0xFF) }
        return Data(bytes)
    }
}

/// Identifiable wrapper so a decoded replay can drive a `.sheet(item:)`.
struct PendingReplay: Identifiable {
    let id = UUID()
    let replay: DecodedReplay
}
