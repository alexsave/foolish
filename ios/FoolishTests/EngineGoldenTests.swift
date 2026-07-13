// EngineGoldenTests.swift — THE keystone (§16.A3, §16.A6). Replays the same
// seeds the C generator used (cnitro/ios/ios_goldens.c → Fixtures/goldens.json)
// through libfoolish.a and asserts byte-equality of the deal fingerprint, the
// legal-move menu, and a full deterministic playthrough hash. If this is green,
// the native build IS the same engine the fixtures came from.
//
// The driver policy here mirrors the generator exactly: lowest eligible seat
// plays its first legal move; hash the viewer-0 state after every move.

import XCTest
@testable import FoolishKit

private struct Goldens: Decodable {
    let nPlayers: Int
    let games: [Game]
    struct Game: Decodable {
        let seedByte0: Int
        let dealHash: String
        let seat0LegalHash: String
        let steps: Int
        let fool: Int
        let playHash: String
    }
}

final class EngineGoldenTests: XCTestCase {

    private func loadGoldens() throws -> Goldens {
        let url = try XCTUnwrap(
            Bundle(for: Self.self).url(forResource: "goldens", withExtension: "json"),
            "goldens.json missing from the test bundle — run `make ios-goldens`"
        )
        return try JSONDecoder().decode(Goldens.self, from: Data(contentsOf: url))
    }

    // FNV-1a 64-bit, identical to the C generator so hashes compare.
    private func fnv1a(_ seed: UInt64, _ bytes: Data) -> UInt64 {
        var h = seed
        for b in bytes { h ^= UInt64(b); h = h &* 1099511628211 }
        return h
    }
    private var fnvOffset: UInt64 { 1469598103934665603 }

    func testEngineMatchesGoldens() async throws {
        let goldens = try loadGoldens()
        XCTAssertEqual(goldens.nPlayers, 4)

        for (i, g) in goldens.games.enumerated() {
            let engine = EngineC()
            var seed = [UInt8](repeating: 0, count: 32)
            for j in 0..<32 { seed[j] = UInt8(((i + 1) * 131 + j * 17) & 0xFF) }
            try await engine.newGame(seed: Data(seed), players: goldens.nPlayers)

            // Deal fingerprint.
            let dealData = try await engine.stateData(viewer: 0)
            XCTAssertEqual(String(format: "%016llx", fnv1a(fnvOffset, dealData)), g.dealHash,
                           "deal fingerprint mismatch, game \(i)")

            // Seat-0 legal menu.
            let legal0 = try await engine.legalMovesData(seat: 0)
            XCTAssertEqual(String(format: "%016llx", fnv1a(fnvOffset, legal0)), g.seat0LegalHash,
                           "seat-0 legal menu mismatch, game \(i)")

            // Deterministic playthrough.
            var playHash = fnvOffset
            var steps = 0
            while try await engine.gameOver() < 0 && steps < 5000 {
                let mask = try await engine.actorMask()
                if mask <= 0 { break }
                var seat = -1
                for s in 0..<goldens.nPlayers where (mask & (1 << s)) != 0 { seat = s; break }
                if seat < 0 { break }
                let moves = try await engine.legalMoves(seat: seat)
                guard let first = moves.first else { break }
                do { try await engine.apply(seat: seat, move: first) } catch { break }
                let sd = try await engine.stateData(viewer: 0)
                playHash = fnv1a(playHash, sd)
                steps += 1
            }
            let fool = try await engine.gameOver()
            XCTAssertEqual(steps, g.steps, "step count mismatch, game \(i)")
            XCTAssertEqual(fool, g.fool, "fool seat mismatch, game \(i)")
            XCTAssertEqual(String(format: "%016llx", playHash), g.playHash,
                           "playthrough hash mismatch, game \(i)")
        }
    }
}
