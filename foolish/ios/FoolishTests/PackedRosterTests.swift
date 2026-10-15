// PackedRosterTests.swift - the envelope's roster, read as bytes.
//
// The server's packed game envelope carried its roster as a JSON island; it is
// a packed TRAILER now, appended past the view blob and announced in flags
// bit1, because merging a server change here deploys instantly while the app
// ships through the App Store. These tests drive PackedGame.decode - the real
// entry point OnlineGame uses - over an envelope assembled here byte by byte
// around a REAL masked-state blob from the kernel (PackedEnvelope.swift).
//
// THE READER IS THE KERNEL'S (c/src/client_table.h client_adopt_envelope,
// through EngineC.adoptEnvelope), the same one the web uses. This file is what
// says the app puts those fields where the board reads them; the bytes
// themselves are pinned against the server's writer in
// e2e/packed_roster_wire.test.ts.
import XCTest
@testable import FoolishKit
@testable import FoolishNet

final class PackedRosterTests: XCTestCase {

    private func seed(_ salt: Int) -> Data {
        Data((0..<32).map { UInt8((($0 &* 31) &+ salt &* 7 &+ 1) & 0xFF) })
    }

    // MARK: - tests

    /// The packed roster reaches the board: names, the game id, and the
    /// column-authoritative status.
    func testPackedRosterFeedsTheBoard() async throws {
        let engine = EngineC()
        try await engine.newGame(seed: seed(3), players: 4)
        let blob = try await PackedEnvelope.viewBlob(engine, seat: 1)

        let names = ["Sveta", "Владимир", "🤡", ""]
        let ids = (0..<4).map { "player-\($0)" }
        let t = PackedEnvelope.trailer(id: "game-xyz", name: "Sveta's Game", status: GameStatus.playing.rawValue,
                                       names: names, ids: ids, isAI: [false, false, true, true],
                                       good: ["player-2"], ts: 1723456789012)
        let buf = PackedEnvelope.envelope(seat: 1, version: 42, island: nil, viewBlob: blob, trailer: t)

        guard let dec = await PackedGame.decode(buf, engine: engine) else {
            return XCTFail("an island-free envelope did not decode")
        }
        XCTAssertEqual(dec.gameId, "game-xyz")
        XCTAssertEqual(dec.seat, 1)
        XCTAssertEqual(dec.version, 42)
        XCTAssertEqual(dec.view.status, GameStatus.playing.rawValue)
        XCTAssertEqual(dec.view.players.map(\.name), names,
                       "the packed roster's names did not reach the seats")
    }

    /// games.status is column-authoritative over the blob's copy - a WAITING
    /// lobby whose blob says otherwise must still render as a lobby.
    func testStatusFromTheRosterOverridesTheBlob() async throws {
        let engine = EngineC()
        try await engine.newGame(seed: seed(4), players: 2)   // the blob says PLAYING
        let blob = try await PackedEnvelope.viewBlob(engine, seat: 0)
        let t = PackedEnvelope.trailer(id: "g", name: "n", status: GameStatus.waiting.rawValue,
                                       names: ["A", "B"], ids: ["a", "b"], isAI: [false, true])
        let buf = PackedEnvelope.envelope(seat: 0, version: 1, island: nil, viewBlob: blob, trailer: t)

        let dec = await PackedGame.decode(buf, engine: engine)
        XCTAssertEqual(dec?.view.status, GameStatus.waiting.rawValue,
                       "the blob's status won over the roster's")
    }

    /// The decoder takes the TRAILER, not the island beside it. Only a reader
    /// that has genuinely stopped parsing JSON can pass this.
    func testTheTrailerWinsOverTheJsonIsland() async throws {
        let engine = EngineC()
        try await engine.newGame(seed: seed(5), players: 2)
        let blob = try await PackedEnvelope.viewBlob(engine, seat: 0)

        let island = Data("""
        {"id":"ISLAND","name":"ISLAND","status":"game_over","players":\
        [{"player_id":"i0","name":"ISLAND-0","is_ai":true},\
        {"player_id":"i1","name":"ISLAND-1","is_ai":true}],\
        "good_players":[],"good_timestamp":null}
        """.utf8)
        let t = PackedEnvelope.trailer(id: "TRAILER", name: "n", status: GameStatus.playing.rawValue,
                                       names: ["Sveta", "Misha"], ids: ["p0", "p1"], isAI: [false, false])
        let buf = PackedEnvelope.envelope(seat: 0, version: 1, island: island, viewBlob: blob, trailer: t)

        guard let dec = await PackedGame.decode(buf, engine: engine) else {
            return XCTFail("the mixed envelope did not decode")
        }
        XCTAssertEqual(dec.gameId, "TRAILER", "the JSON island was read instead of the trailer")
        XCTAssertEqual(dec.view.players.map(\.name), ["Sveta", "Misha"])
        XCTAssertEqual(dec.view.status, GameStatus.playing.rawValue)
    }

    /// A row stored BEFORE the trailer existed carries a JSON island and no
    /// trailer. The fallback that read it is gone, so this is REFUSED, and that
    /// is the intended trade: a table seated with no names is worse than a load
    /// error, because the caller cannot tell it went wrong. The next commit on
    /// that game rewrites the row.
    func testAnEnvelopeWithoutATrailerIsRefused() async throws {
        let engine = EngineC()
        try await engine.newGame(seed: seed(6), players: 2)
        let blob = try await PackedEnvelope.viewBlob(engine, seat: 0)
        let island = Data("""
        {"id":"old-row","name":"Old Game","status":"playing","players":\
        [{"player_id":"p0","name":"Sveta","is_ai":false},\
        {"player_id":"p1","name":"Миша","is_ai":false}],\
        "good_players":[],"good_timestamp":null}
        """.utf8)
        let buf = PackedEnvelope.envelope(seat: 0, version: 9, island: island, viewBlob: blob, trailer: nil)

        let dec = await PackedGame.decode(buf, engine: engine)
        XCTAssertNil(dec, "a JSON island was read as a roster - the fallback is back")
    }

    /// A trailer read short is nothing, never half a table: the decode fails
    /// rather than seating people under the wrong names.
    func testATruncatedTrailerIsRefused() async throws {
        let engine = EngineC()
        try await engine.newGame(seed: seed(7), players: 2)
        let blob = try await PackedEnvelope.viewBlob(engine, seat: 0)
        let t = PackedEnvelope.trailer(id: "g", name: "n", status: GameStatus.playing.rawValue,
                                       names: ["Sveta", "Misha"], ids: ["p0", "p1"], isAI: [false, false],
                                       good: ["p0"], ts: 1.5)
        for cut in [1, 4, 11, t.count - 1] {
            let buf = PackedEnvelope.envelope(seat: 0, version: 1, island: nil, viewBlob: blob,
                               trailer: Array(t.prefix(cut)))
            let dec = await PackedGame.decode(buf, engine: engine)
            XCTAssertNil(dec, "a \(cut)-byte trailer was read as a whole roster")
        }
    }

    /// A name whose UTF-8 length differs from its character count survives
    /// exactly, and a 64-byte name (the kernel's MSG_MAX_NAME) is not the
    /// decoder's problem to trim - it arrives whole.
    func testNonAsciiNamesSurviveTheByteLengthCodec() async throws {
        let engine = EngineC()
        try await engine.newGame(seed: seed(8), players: 4)
        let blob = try await PackedEnvelope.viewBlob(engine, seat: 0)
        // 16 clowns is exactly 64 bytes and 16 characters.
        let full = String(repeating: "🤡", count: 16)
        XCTAssertEqual(full.utf8.count, 64)
        let names = [full, "Ünïcodé", "さくら", "a\"b\\c"]
        let t = PackedEnvelope.trailer(id: "g", name: "n", status: GameStatus.playing.rawValue,
                                       names: names, ids: ["a", "b", "c", "d"],
                                       isAI: [false, false, false, false])
        let buf = PackedEnvelope.envelope(seat: 0, version: 1, island: nil, viewBlob: blob, trailer: t)

        let dec = await PackedGame.decode(buf, engine: engine)
        XCTAssertEqual(dec?.view.players.map(\.name), names,
                       "a non-ASCII name did not survive the byte-length codec")
    }
}
