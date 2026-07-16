// Seat identity + the App Group cache (design §6). No Messages framework, no
// kernel — pure decisions over the three §6 signals and a round-trip through the
// store — so this whole file runs on the plain simulator with no fixtures.
import XCTest
@testable import FoolishKit

final class SeatIdentityTests: XCTestCase {

    // MARK: - §6 resolution priority

    func testCacheWinsOverEverything() {
        // Even when I sent the bubble (S1 would say seat 3), a cached seat is the
        // authoritative §6.1 answer — the one thing a fresh bubble can't recover.
        let r = SeatIdentity.resolve(cachedSeat: 1, senderIsLocal: true,
                                     nPlayers: 4, lastActorSeat: 3)
        XCTAssertEqual(r, .known(1))
    }

    func testSenderInferenceIsExactForAnyN() {
        // §6.2 S1: no cache, but THIS device sent the tapped bubble ⇒ I am its
        // last actor, exact regardless of player count.
        XCTAssertEqual(SeatIdentity.resolve(cachedSeat: nil, senderIsLocal: true,
                                            nPlayers: 5, lastActorSeat: 2), .known(2))
    }

    func testTwoPlayerInfersTheOtherSeat() {
        // §6.2 S1: 2p, I did NOT send it ⇒ I must be the other of two seats.
        XCTAssertEqual(SeatIdentity.resolve(cachedSeat: nil, senderIsLocal: false,
                                            nPlayers: 2, lastActorSeat: 0), .known(1))
        XCTAssertEqual(SeatIdentity.resolve(cachedSeat: nil, senderIsLocal: false,
                                            nPlayers: 2, lastActorSeat: 1), .known(0))
    }

    func testThreePlusWithoutCacheOrSenderIsAmbiguous() {
        // §6.3: N≥3, no cache, not the last actor — the only honest answer is to
        // ask (the nickname picker), NOT to guess a seat.
        XCTAssertEqual(SeatIdentity.resolve(cachedSeat: nil, senderIsLocal: false,
                                            nPlayers: 4, lastActorSeat: 2), .ambiguous)
    }

    func testStaleOutOfRangeCacheIsIgnoredNotTrusted() {
        // A cache from a different (bigger) game must never seat me out of range;
        // fall through to the live signals instead of returning .known(7).
        let r = SeatIdentity.resolve(cachedSeat: 7, senderIsLocal: false,
                                     nPlayers: 2, lastActorSeat: 0)
        XCTAssertEqual(r, .known(1), "out-of-range cache ignored, 2p inference used")
    }

    // MARK: - the App Group store

    private func freshStore() -> MessageGameStore {
        let suite = "test.fmsg.\(UUID().uuidString)"
        let d = UserDefaults(suiteName: suite)!
        d.removePersistentDomain(forName: suite)
        return MessageGameStore(defaults: d)
    }

    private func rec(_ id: String, seat: Int, at t: Double, turn: Int = 4) -> MessageGameRecord {
        MessageGameRecord(gameId: id, mySeat: seat, nPlayers: 2, round: 1, turn: turn,
                          phase: 2, finished: false, names: [0: "Alex", 1: "Sveta"],
                          payloadBase32: "AAAA", updatedAt: t)
    }

    func testPutThenSeatAndRecordRoundTrip() {
        let s = freshStore()
        s.put(rec("g1", seat: 1, at: 100))
        XCTAssertEqual(s.seat(gameId: "g1"), 1)
        XCTAssertEqual(s.record(gameId: "g1")?.name(0), "Alex")
        XCTAssertNil(s.seat(gameId: "absent"))
    }

    func testGamesSortNewestFirst() {
        let s = freshStore()
        s.put(rec("old", seat: 0, at: 100))
        s.put(rec("new", seat: 0, at: 200))
        XCTAssertEqual(s.games().map(\.gameId), ["new", "old"])
    }

    func testOlderUpdateDoesNotRollBackTheCache() {
        // A late-delivered stale bubble (lower updatedAt) must not overwrite a
        // newer row — the cache is delivery-order-independent, like Rule P (§7.2).
        let s = freshStore()
        s.put(rec("g", seat: 0, at: 200, turn: 9))
        s.put(rec("g", seat: 0, at: 100, turn: 3))     // arrives later, older state
        XCTAssertEqual(s.record(gameId: "g")?.turn, 9, "newer state kept")
    }

    func testRemove() {
        let s = freshStore()
        s.put(rec("g", seat: 0, at: 100))
        s.remove(gameId: "g")
        XCTAssertNil(s.record(gameId: "g"))
        XCTAssertTrue(s.games().isEmpty)
    }

    func testCorruptBlobDegradesToEmpty() {
        let suite = "test.fmsg.\(UUID().uuidString)"
        let d = UserDefaults(suiteName: suite)!
        d.set(Data([0xff, 0x00, 0x13]), forKey: "fmsg.games.v1")   // not our JSON
        let s = MessageGameStore(defaults: d)
        XCTAssertTrue(s.games().isEmpty, "a corrupt suite reads as no games, never crashes")
        s.put(rec("g", seat: 0, at: 1))                            // and recovers on write
        XCTAssertEqual(s.seat(gameId: "g"), 0)
    }
}
