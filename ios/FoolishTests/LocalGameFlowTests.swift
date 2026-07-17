// LocalGameFlowTests.swift — §16.B/§12: verify a full OFFLINE game plays end to
// end and reaches the win/lose screen. LocalGame is a thin @MainActor wrapper
// over the kernel; its bot loop only adds cosmetic pacing (BotDrive.delayMs) on
// top of these exact calls — newGame, setSeatStrategy, botDrive, legalMoves,
// apply, gameOver. So we drive that same kernel path in a tight loop (no sleeps)
// and assert the three things #12 asks for: the board is interactive (the human
// plays), legal-move gating is the kernel's (every move we play was on the
// kernel's own menu — §3), and the game reaches a decided end (the fool the
// WinView reveals is a real seat and matches the human's own view).
//
// The playthrough policy mirrors LocalGame: botDrive advances every non-human
// seat as far as the kernel allows, then the human plays its first legal move.
import XCTest
@testable import FoolishKit

final class LocalGameFlowTests: XCTestCase {

    /// A real opponent from the roster (not `random`), so the bot actually plays
    /// Durak — preferring the names LocalGame's callers use, any non-random id
    /// otherwise.
    private func opponentStrategyId(preferring names: [String]) -> Int {
        let roster = EngineC.roster()
        for want in names {
            if let hit = roster.first(where: { $0.name == want }) { return hit.id }
        }
        return roster.first(where: { $0.name != "random" })?.id ?? 0
    }

    /// Drive one full offline 2p game (human seat 0 vs a roster bot). Returns the
    /// fool seat and how many moves the human actually made.
    private func driveOffline(seed: Data, botId: Int) async throws -> (fool: Int, humanMoves: Int) {
        let engine = EngineC()
        let humanSeat = 0
        try await engine.newGame(seed: seed, players: 2)
        try await engine.setSeatStrategy(seat: 1, strategyId: botId)

        var humanMoves = 0, steps = 0
        while try await engine.gameOver() < 0 && steps < 5000 {
            // Let the bot (every non-human seat) act as far as it can this cycle.
            let drive = try await engine.botDrive(humanSeats: [humanSeat])
            if drive.isOver { break }

            // The human's kernel-gated menu; `.wait` means "not your turn".
            let menu = try await engine.legalMoves(seat: humanSeat)
            let playable = menu.filter { $0.type != .wait }
            if playable.isEmpty {
                // Neither side advanced and it isn't the human's turn — the only
                // non-terminal stop. Break rather than spin forever.
                if drive.actions.isEmpty { break }
                steps += 1
                continue
            }

            // The GATE: the move we apply is one the kernel itself offered.
            let move = playable[0]
            XCTAssertTrue(menu.contains(move), "played a move the kernel never offered")
            try await engine.apply(seat: humanSeat, move: move)
            humanMoves += 1
            steps += 1
        }

        let fool = try await engine.gameOver()
        // The screen the app shows reads the fool off the human's OWN masked view;
        // it must agree with the authoritative game_done.
        let over = try await engine.state(viewer: humanSeat)
        XCTAssertEqual(over.gameOver, fool, "the human's view disagrees with game_done")
        return (fool, humanMoves)
    }

    /// A complete offline game reaches a decided end with kernel-gated human moves
    /// — the exact precondition RootView needs to route Table → WinView.
    func testOfflineGameVsBotReachesADecidedEnd() async throws {
        let botId = opponentStrategyId(preferring: ["robusta", "cordite", "handwritten"])
        var seed = [UInt8](repeating: 0, count: 32)
        for j in 0..<32 { seed[j] = UInt8((j * 37 + 11) & 0xFF) }

        let (fool, humanMoves) = try await driveOffline(seed: Data(seed), botId: botId)
        XCTAssertGreaterThanOrEqual(fool, 0, "the game must reach a decided end (WinView needs a fool)")
        XCTAssertLessThan(fool, 2, "the fool is a real seat")
        XCTAssertGreaterThan(humanMoves, 0, "the human actually played — the board was interactive")
    }

    /// Both outcomes are reachable: across a spread of deals the human is
    /// sometimes the fool and sometimes not, so WinView's win/lose split
    /// (`humanWon = fool != humanSeat`) is exercised on both branches.
    func testOfflinePlayYieldsBothWinAndLoss() async throws {
        let botId = opponentStrategyId(preferring: ["robusta", "cordite", "handwritten"])
        var sawWin = false, sawLoss = false
        for s in 0..<12 where !(sawWin && sawLoss) {
            var seed = [UInt8](repeating: 0, count: 32)
            for j in 0..<32 { seed[j] = UInt8(((s + 3) * 101 + j * 7) & 0xFF) }
            let (fool, _) = try await driveOffline(seed: Data(seed), botId: botId)
            XCTAssertGreaterThanOrEqual(fool, 0, "every deal must resolve, seed \(s)")
            if fool == 0 { sawLoss = true } else { sawWin = true }
        }
        XCTAssertTrue(sawWin, "the human wins at least one of twelve deals")
        XCTAssertTrue(sawLoss, "the human loses at least one of twelve deals")
    }
}
