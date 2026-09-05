// FMSG on the phone — the third leg of the cross-engine gate (design §8.2).
//
// The SAME fixtures live in three places and must mean the same game in all of
// them, or an iMessage game forks between the players in it:
//
//   c/tests/msg_wire_test.c   the native kernel SEALS them (--fixture)
//   e2e/msg_wire.test.ts           the wasm kernel decodes them  (the web)
//   this file                      libfoolish.a decodes them     (the phone)
//
// A diff here is a release blocker, not a test failure: it means a phone and a
// browser would replay one payload into two different games. Regenerate with
// `c/build/msg_wire_test --fixture` only when the WIRE deliberately
// changes — and then every shipped device has to agree with the new bytes.
import XCTest
@testable import FoolishKit

final class MessageEnvelopeTests: XCTestCase {

    /// Mid-game turn bubbles — what actually ships, not finished games.
    /// Sealed by the native kernel; the header claims are the assertions.
    private struct Fixture {
        let players: Int, turn: Int, round: Int, hex: String
    }
    private let fixtures: [Fixture] = [
        .init(players: 2, turn: 8, round: 2, hex:
            "f7020002efcdab89674523010800000200020000000000000000ae15293755bd748b2919627cd0591ffb42d7f9b2e9b57da5c2839ed47bd7ced7020004416e6e300104416e6e310800f72719e90cb7ee031bd6af74a3a23a"),
        .init(players: 3, turn: 10, round: 1, hex:
            "f7020002efcdab89674523010a0000030001000000000000000079d87206410d37d302c19dfb6cacbc8bebf879d242622082315709cc0f183788030004416e6e300104416e6e310204416e6e320a00012951da5bef3096f9f7bf2cfb58d013f6d7fa"),
        .init(players: 4, turn: 7, round: 1, hex:
            "f7020002efcdab89674523010700000400010000000000000000449bbad52d5dfb1bdb68d87a09fe591b9419f9f39b0ec35e9f2b75c5a359a138040004416e6e300104416e6e310204416e6e320304416e6e33070001c32dd6c13bd1e53963f945fef906649a"),
    ]

    private func bytes(_ hex: String) -> Data {
        var d = Data(); var i = hex.startIndex
        while i < hex.endIndex {
            let j = hex.index(i, offsetBy: 2)
            d.append(UInt8(hex[i..<j], radix: 16)!); i = j
        }
        return d
    }

    // MARK: - The gate

    func testPhoneDecodesWhatTheNativeKernelSealed() async throws {
        for f in fixtures {
            let env = try await MessageEnvelope.decode(payload: bytes(f.hex), viewer: 0)
            XCTAssertEqual(env.nPlayers, f.players)
            XCTAssertEqual(env.phase, 2, "LIVE")
            // turn and round are the chain's own claims, and the kernel only
            // returns if the BODY backs them — so agreeing here means this
            // device replayed the identical chain the other two did.
            XCTAssertEqual(env.turn, f.turn, "\(f.players)p atom count")
            XCTAssertEqual(env.round, f.round, "\(f.players)p completed bouts")
            XCTAssertEqual(env.joins.count, f.players)
            XCTAssertEqual(env.joins.map(\.seat), Array(0..<f.players))
            XCTAssertEqual(env.gameId, "81985529216486895", "u64 survived JSON as a string")
            XCTAssertEqual(env.digest.count, 64, "SHA-256 as hex — Rule P's tiebreak")
            XCTAssertFalse(env.digest.allSatisfy { $0 == "0" })
        }
    }

    /// Decoding ADOPTS: afterwards the resident game IS the payload's game, and
    /// the ordinary engine calls read it. That is what makes a turn continue
    /// from what it decoded, with no second copy of the state anywhere.
    func testDecodeLeavesTheGameResident() async throws {
        let env = try await MessageEnvelope.decode(payload: bytes(fixtures[2].hex), viewer: 0)
        XCTAssertEqual(env.nPlayers, 4)
        let engine = EngineC()
        let state = try await engine.statePackedData(viewer: 0)
        XCTAssertGreaterThan(state.count, 0, "the adopted game is readable through the normal bridge")
    }

    /// The extension reads the adopted board through MessageKernel itself (same
    /// actor as the decode, so no race on the shared static Game) — this is what
    /// MessageBoardView renders. A seat view unmasks that seat; the spectator
    /// (-1) view the bubble snapshot uses leaks no hand.
    func testResidentViewReadsTheAdoptedBoard() async throws {
        let env = try await MessageEnvelope.decode(payload: bytes(fixtures[2].hex), viewer: 0)
        XCTAssertEqual(env.nPlayers, 4)
        let k = MessageKernel.shared
        guard let mine = await k.residentView(viewer: 0) else {
            return XCTFail("residentView(0) returned nil for an adopted game")
        }
        XCTAssertEqual(mine.numPlayers, 4, "the adopted 4p game reads back as 4 seats")
        XCTAssertEqual(mine.players.count, 4)

        guard let pub = await k.residentView(viewer: -1) else {
            return XCTFail("residentView(-1) returned nil")
        }
        XCTAssertTrue(pub.players.allSatisfy { $0.hand == nil }, "the spectator/snapshot view leaks no hand")
        _ = await k.residentLegal(seat: 0)   // kernel-computed; must not trap on a live game
    }

    /// The OUTGOING path (M3 oracle, no Messages harness). Two halves:
    /// (a) newGame makes a fresh dealt board resident; (b) seal of the resident
    /// game round-trips through the link format. An FMSG bubble is sent AFTER a
    /// move, so the seal oracle re-seals a real mid-game turn rather than a
    /// 0-action opening (which is not a valid body — MSG_EBODY).
    func testNewGameAndSealRoundTrip() async throws {
        let k = MessageKernel.shared

        // (a) newGame → a fresh 2p deal is 6 cards a seat.
        let seed = Data((0..<32).map { UInt8(($0 &* 7 &+ 3) | 1) })   // 32 bytes, non-zero
        try await k.newGame(seed: seed, players: 2)
        guard let fresh = await k.residentView(viewer: 0) else {
            return XCTFail("newGame left no resident board")
        }
        XCTAssertEqual(fresh.numPlayers, 2)
        XCTAssertEqual(fresh.players.first?.handCount, 6, "a fresh 2p deal is 6 cards")

        // (b) adopt a real mid-game turn, re-seal it, decode the result — the
        // header the sender claims survives the seal→decode both sides do.
        let orig = try await MessageEnvelope.decode(payload: bytes(fixtures[0].hex), viewer: 0)
        let payload = try await k.seal(phase: orig.phase, lastActorSeat: orig.lastActorSeat,
                                       gameId: UInt64(orig.gameId)!,
                                       parent8: Data(repeating: 0, count: 8), joins: orig.joins)
        XCTAssertFalse(payload.isEmpty, "seal produced a payload")
        let url = URL(string: "https://foolish.cards/m/1" + Base32.encode(payload))!
        let env = try await MessageEnvelope.decode(url: url, viewer: -1)
        XCTAssertEqual(env.nPlayers, orig.nPlayers)
        XCTAssertEqual(env.turn, orig.turn, "the atom count survived reseal")
        XCTAssertEqual(env.round, orig.round)
        XCTAssertEqual(env.joins, orig.joins, "identities survived")
    }

    /// THE ROSTER CROSSES AS BYTES, in the layout the kernel hands back - it
    /// was the last JSON on any path that mattered. What is pinned is that the
    /// codec is a round trip through the REAL kernel and not a Swift-only
    /// equality, and that it survives the two things a JSON parser used to have
    /// opinions about: multi-byte UTF-8, and the characters JSON has to escape.
    ///
    /// MUTATIONS (sdk/swift/RosterWire.swift), each on its own: write name_len
    /// as the CHARACTER count rather than the UTF-8 byte count -> this test
    /// fails at the seal, because the blob no longer measures up and the kernel
    /// refuses it whole (c/ios/ios_api.c fio_read_joins); drop the n_joins byte
    /// -> every test in the target that seals fails, for the same reason.
    func testTheRosterCrossesAsBytesAndComesBackWhole() async throws {
        let k = MessageKernel.shared
        let seed = Data((0..<32).map { UInt8(truncatingIfNeeded: $0 &* 11 &+ 5) | 1 })
        try await k.newGame(seed: seed, players: 4)
        let joins = [MessageJoin(seat: 0, name: "Света"),
                     MessageJoin(seat: 1, name: "A \"quoted\" name, with \\ and \u{1F0A1}"),
                     MessageJoin(seat: 2, name: ""),
                     MessageJoin(seat: 3, name: String(repeating: "x", count: 64))]
        let payload = try await k.seal(phase: 2, lastActorSeat: 0, gameId: 77,
                                       parent8: Data(repeating: 0, count: 8),
                                       joins: joins, sentAt: 1)
        let env = try await MessageEnvelope.decode(payload: payload, viewer: -1)
        XCTAssertEqual(env.joins, joins,
                       "every seat, every byte - the kernel writes the roster back in the "
                       + "same layout it was handed")

        // The 64-byte cap is the kernel's (MSG_MAX_NAME) and the encoder trims
        // to it on a scalar boundary, so an over-long name shortens rather than
        // failing the seal or arriving as broken UTF-8.
        let longName = String(repeating: "Ж", count: 40)          // 80 UTF-8 bytes
        let over = try await k.seal(phase: 2, lastActorSeat: 0, gameId: 78,
                                    parent8: Data(repeating: 0, count: 8),
                                    joins: [MessageJoin(seat: 0, name: longName),
                                            MessageJoin(seat: 1, name: "B"),
                                            MessageJoin(seat: 2, name: "C"),
                                            MessageJoin(seat: 3, name: "D")],
                                    sentAt: 1)
        let trimmed = try await MessageEnvelope.decode(payload: over, viewer: -1).joins[0].name
        XCTAssertEqual(trimmed, String(repeating: "Ж", count: 32),
                       "trimmed by whole scalars to 64 bytes, never mid-sequence")
        XCTAssertTrue(longName.hasPrefix(trimmed), "…and it is a prefix of what was asked for")
    }

    // MARK: - Rule P, decided in C

    func testRulePIsReflexiveAndSymmetric() async throws {
        let a = bytes(fixtures[1].hex), b = bytes(fixtures[2].hex)
        let k = MessageKernel.shared
        let aa = try await k.preferred(a, a)
        XCTAssertEqual(aa, 0, "a chain never beats itself")
        let ab = try await k.preferred(a, b)
        let ba = try await k.preferred(b, a)
        XCTAssertEqual(ab, -ba, "a<b iff b>a — and identical on every device")
    }

    // MARK: - Hostile input

    func testDamagedPayloadsAreRefusedNotHalfLoaded() async throws {
        let good = bytes(fixtures[1].hex)
        // Corrupt the body: a code that is not this game's.
        var bad = good
        bad[bad.count - 2] ^= 0xff
        do {
            _ = try await MessageEnvelope.decode(payload: bad, viewer: 0)
            XCTFail("a corrupted body must not decode")
        } catch { /* expected: validation IS replay */ }

        // A header that lies about its own chain.
        var lying = good
        lying[17] = 99   // round
        do {
            _ = try await MessageEnvelope.decode(payload: lying, viewer: 0)
            XCTFail("a round the chain cannot back must not decode")
        } catch { /* expected */ }

        // Truncation must never crash, whatever it decides.
        for cut in 0..<good.count {
            _ = try? await MessageEnvelope.decode(payload: good.prefix(cut), viewer: 0)
        }
    }

    // MARK: - The URL layer

    func testPayloadRoundTripsThroughTheLinkFormat() throws {
        let raw = bytes(fixtures[0].hex)
        let link = URL(string: "https://foolish.cards/m/1\(Base32.encode(raw))")!
        XCTAssertEqual(try MessageEnvelope.payloadBytes(url: link), raw)
        // The '1' is the TEXT-level version: a link is rejected before a single
        // binary byte is read (§4.3).
        let wrongVersion = URL(string: "https://foolish.cards/m/9\(Base32.encode(raw))")!
        XCTAssertThrowsError(try MessageEnvelope.payloadBytes(url: wrongVersion))
        // And it fits MSMessage.url's documented 5,000-char cap with room.
        XCTAssertLessThan(link.absoluteString.count, 1000)
    }

    /// `link` is the composer's URL builder and the exact inverse of
    /// `payloadBytes` — what the extension stages must decode back to the bytes.
    func testLinkIsTheInverseOfPayloadBytes() throws {
        let raw = bytes(fixtures[1].hex)
        let url = MessageEnvelope.link(payload: raw)
        XCTAssertTrue(url.absoluteString.hasPrefix("https://foolish.cards/m/1"))
        XCTAssertEqual(try MessageEnvelope.payloadBytes(url: url), raw)
    }
}
