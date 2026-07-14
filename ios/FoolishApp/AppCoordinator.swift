// AppCoordinator.swift — top-level navigation + the active offline game. Keeps
// SwiftUI navigation simple: a screen enum plus the live LocalGame. Online play
// (M-D) will add its session here alongside `offlineGame`.

import SwiftUI
import Foundation
import FoolishKit

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
    enum Screen: Equatable { case home, table, onlineTable }

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
        for seat in 1..<players { strategies[seat] = config.opponentStrategyId }
        let game = LocalGame(seed: makeSeed(), players: players, humanSeat: 0, strategies: strategies)
        offlineGame = game
        screen = .table
    }

    func rematch(_ config: OfflineConfig) { startOffline(config) }

    /// Debug/QA hook: `-offlinePlayers N` auto-starts an N-player offline game on
    /// launch so any table configuration (e.g. an 8-player game on a small phone)
    /// can be inspected without driving the UI. No effect in normal launches.
    func maybeAutostartFromLaunchArgs() {
        let args = ProcessInfo.processInfo.arguments
        guard let i = args.firstIndex(of: "-offlinePlayers"), i + 1 < args.count,
              let n = Int(args[i + 1]), n >= 2, n <= 8 else { return }
        guard screen == .home, offlineGame == nil else { return }
        startOffline(OfflineConfig(opponentStrategyId: 0, opponentName: "Random", opponents: n - 1))
    }

    /// Quick-match online (§16.D5). Requires a signed-in user; the caller gates
    /// on auth first. Surfaces the create/seam error rather than failing silently.
    func startOnline(userId: UUID) {
        Task {
            do {
                let game = try await OnlineService.shared.quickMatch(userId: userId)
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
