// ReplayDecodeParityTests.swift — the Swift packed replay decoder
// (DecodedReplay.decode via fio_replay_decode_packed) parses a finished game's
// share code into a coherent step list. This replaced the fio_replay_decode_json
// bridge (#17); byte-parity against that bridge was verified across 24 games at
// the time of the cutover, and this pins the decode's structural invariants so a
// future wire change can't silently corrupt a replay.
import XCTest
@testable import FoolishKit

final class ReplayDecodeParityTests: XCTestCase {

    private func codeFromFinishedGame(seedSalt: Int, players: Int) async throws -> String {
        let engine = EngineC()
        let seed = Data((0..<32).map { UInt8((($0 &* 29) &+ seedSalt &* 13 &+ 7) & 0xFF) })
        try await engine.newGame(seed: seed, players: players)
        for _ in 0..<5000 {
            if try await engine.gameOver() >= 0 { break }
            let mask = try await engine.actorMask()
            guard mask > 0 else { break }
            var seat = -1
            for s in 0..<players where (mask & (1 << s)) != 0 { seat = s; break }
            guard seat >= 0, let mv = try await engine.legalMoves(seat: seat).first else { break }
            try await engine.apply(seat: seat, move: mv)
        }
        let finalFool = try await engine.gameOver()
        XCTAssertGreaterThanOrEqual(finalFool, 0, "the driven game finished")
        return try await engine.replayEncodeCode()
    }

    func testPackedDecodeIsCoherentAcrossManyGames() async throws {
        let engine = EngineC()
        var checked = 0
        for players in [2, 3, 4, 6] {
            for salt in 0..<6 {
                let code = try await codeFromFinishedGame(seedSalt: salt, players: players)
                XCTAssertFalse(code.isEmpty, "p=\(players) salt=\(salt) produced no code")

                let r = try await engine.replayDecode(code: code)
                XCTAssertEqual(r.nPlayers, players, "decoded player count, p=\(players) salt=\(salt)")
                XCTAssertTrue(r.isComplete, "a finished game's replay knows its fool")
                XCTAssertTrue((0..<players).contains(r.fool), "fool is a real seat")
                XCTAssertNotNil(r.trump, "a real deal has a trump")
                XCTAssertFalse(r.logs.isEmpty, "the replay carries its step list")
                XCTAssertEqual(r.logs.first?.type, 0, "the stream opens with GAME_START (LOG type 0)")
                checked += 1
            }
        }
        XCTAssertEqual(checked, 24, "every game decoded")
    }
}
