// PackedViewTests.swift — the packed masked-view decode, Swift-side (§16.D4,
// §3). Replaces the retired C harness ios_view_test.c: where that compared the
// kernel's packed→JSON bridge against the JSON state, this exercises the wire
// the app actually ships — a seat's masked state as raw state_put bytes
// (statePackedData) decoded by MaskedView, and the legal menu the kernel
// computes FROM that packed view (legalFromPacked, the same call OnlineGame uses
// for online enable-states) — and asserts both agree with the live offline game.
import XCTest
@testable import FoolishKit

final class PackedViewTests: XCTestCase {

    private func seed(_ salt: Int) -> Data {
        Data((0..<32).map { UInt8((($0 &* 31) &+ salt &* 7 &+ 1) & 0xFF) })
    }

    /// A seat's masked packed state (state_put) decodes to the SAME GameView the
    /// live offline read gives — the online feed and the offline board are one
    /// decoder (MaskedView), never a reimplemented wire.
    func testPackedStateDecodesToTheLiveView() async throws {
        let engine = EngineC()
        try await engine.newGame(seed: seed(1), players: 4)

        let live = try await engine.state(viewer: 0)
        let packed = try await engine.statePackedData(viewer: 0)
        guard let decoded = MaskedView.decode(packed, viewer: 0) else {
            return XCTFail("MaskedView.decode returned nil for a dealt game")
        }
        XCTAssertEqual(decoded.numPlayers, live.numPlayers)
        XCTAssertEqual(decoded.powerSuit, live.powerSuit)
        XCTAssertEqual(decoded.deckCount, live.deckCount)
        XCTAssertEqual(decoded.defender, live.defender)
        XCTAssertEqual(decoded.firstAttacker, live.firstAttacker)
        XCTAssertEqual(decoded.me?.hand, live.me?.hand, "the viewer's own hand survives the packed round-trip")
    }

    /// Legal moves computed FROM the packed masked view match the live game's
    /// legal moves for that seat — online enable-states are the kernel's answer
    /// (§3), decoded from the same bytes the server stores in player_views.
    func testLegalFromPackedMatchesTheLiveMenu() async throws {
        let engine = EngineC()
        try await engine.newGame(seed: seed(2), players: 4)

        // Drive a few moves so the menu is a real mid-game one (covers included),
        // not just the opening attack.
        for _ in 0..<8 {
            if try await engine.gameOver() >= 0 { break }
            let mask = try await engine.actorMask()
            guard mask > 0 else { break }
            var seat = -1
            for s in 0..<4 where (mask & (1 << s)) != 0 { seat = s; break }
            guard seat >= 0, let mv = try await engine.legalMoves(seat: seat).first else { break }
            try await engine.apply(seat: seat, move: mv)
        }

        for seat in 0..<4 {
            let live = try await engine.legalMoves(seat: seat)
            let packed = try await engine.statePackedData(viewer: seat)
            let fromPacked = try await engine.legalFromPacked(packed, seat: seat)
            XCTAssertEqual(fromPacked, live,
                           "legal moves from the packed view diverged from the live game at seat \(seat)")
        }
    }
}
