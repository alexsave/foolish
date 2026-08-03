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

    // MARK: - §6 resolution, gated for a lobby bubble (note 14, HARNESS_NOTES_R2)

    /// The exact bug note 14 describes: a cached seat that is NOT in this
    /// bubble's own `joins` must not read as joined — an older WAITING bubble,
    /// reopened after I've since joined elsewhere, would otherwise still hand
    /// me Start/Send for a lobby that does not list me.
    func testCachedSeatAbsentFromJoinsIsNotJoined() {
        let staleJoins = [MessageJoin(seat: 0, name: "Alex")]   // seat 1 (me) not in here yet
        let r = SeatIdentity.resolveInLobby(cachedSeat: 1, senderIsLocal: false,
                                            nPlayers: 8, lastActorSeat: 0, joins: staleJoins)
        XCTAssertNil(r, "a cached seat this bubble's own joins does not list must not read as joined")
    }

    /// The flip side of the same bug: once the bubble's `joins` DOES list my
    /// cached seat (the freshest lobby bubble, post-join), I must resolve as
    /// joined again — the gate only rejects a MISMATCH, not every lobby.
    func testCachedSeatPresentInJoinsIsJoined() {
        let freshJoins = [MessageJoin(seat: 0, name: "Alex"), MessageJoin(seat: 1, name: "Sveta")]
        let r = SeatIdentity.resolveInLobby(cachedSeat: 1, senderIsLocal: false,
                                            nPlayers: 8, lastActorSeat: 0, joins: freshJoins)
        XCTAssertEqual(r, 1, "once this bubble's own joins list me, I resolve as joined")
    }

    /// Sender inference (S1) is gated the same way: even though I sent the
    /// stale bubble (so plain `resolve` would say `.known(lastActorSeat)`),
    /// that bubble's joins must still be checked.
    func testSenderInferredSeatAbsentFromJoinsIsNotJoined() {
        let staleJoins = [MessageJoin(seat: 0, name: "Alex")]
        let r = SeatIdentity.resolveInLobby(cachedSeat: nil, senderIsLocal: true,
                                            nPlayers: 8, lastActorSeat: 2, joins: staleJoins)
        XCTAssertNil(r, "sender-inferred seat 2 is not in this stale bubble's joins")
    }

    /// `.ambiguous` still maps to nil either way (no seat to check joins against).
    func testAmbiguousStaysNilInLobby() {
        let r = SeatIdentity.resolveInLobby(cachedSeat: nil, senderIsLocal: false,
                                            nPlayers: 4, lastActorSeat: 2, joins: [])
        XCTAssertNil(r)
    }

    // MARK: - the App Group store

    private let chatA = "chat-A"
    private let chatB = "chat-B"

    private func freshStore() -> MessageGameStore {
        let suite = "test.fmsg.\(UUID().uuidString)"
        let d = UserDefaults(suiteName: suite)!
        d.removePersistentDomain(forName: suite)
        return MessageGameStore(defaults: d)
    }

    /// Round 7: the seat this device holds round-trips through the store, scoped by
    /// chat (§6.1). This is the one per-game fact the store keeps after the cache
    /// strip (the preferred-chain game-record cache is gone).
    func testSetSeatRoundTrip() {
        let s = freshStore()
        s.setSeat(gameId: "g1", chatKey: chatA, seat: 1)
        XCTAssertEqual(s.seat(gameId: "g1", chatKey: chatA), 1)
        XCTAssertNil(s.seat(gameId: "absent", chatKey: chatA))
        // A device only ever holds one seat in a game; the latest write wins.
        s.setSeat(gameId: "g1", chatKey: chatA, seat: 0)
        XCTAssertEqual(s.seat(gameId: "g1", chatKey: chatA), 0)
    }

    /// Round 8 brought the game-record store BACK for `put`/`games` (the app-drawer
    /// + reopen, § one game per thread) - but `record` stays a nil no-op ON PURPOSE:
    /// that reader was Rule P ("prefer a cached chain over the tapped bubble"), the
    /// Round-7 "stuck on a stale lobby" bug. So a tapped bubble is still rendered
    /// exactly as tapped (record nil), while a drawer-open with no bubble can reopen
    /// the last stored chain (games non-empty).
    func testGameRecordServesReopenButNotRuleP() {
        let s = freshStore()
        s.put(MessageGameRecord(gameId: "g", chatKey: chatA, mySeat: 1, nPlayers: 2, round: 1,
                                turn: 4, phase: 2, finished: false, names: [:],
                                payloadBase32: "AAAA", updatedAt: 100))
        XCTAssertNil(s.record(gameId: "g", chatKey: chatA), "record stays nil - Rule P is not back")
        XCTAssertEqual(s.games(chatKey: chatA).map(\.gameId), ["g"], "but games() serves the drawer reopen")
        // Newest wins, and it stays chat-scoped.
        s.put(MessageGameRecord(gameId: "g2", chatKey: chatA, mySeat: 0, nPlayers: 2, round: 2,
                                turn: 0, phase: 2, finished: false, names: [:],
                                payloadBase32: "BBBB", updatedAt: 200))
        XCTAssertEqual(s.games(chatKey: chatA).first?.gameId, "g2", "newest chain reopens first")
        XCTAssertTrue(s.games(chatKey: "chat-B-xyz").isEmpty, "another chat sees none of it")
    }

    /// A corrupt seat blob reads as no seat and never crashes; a write recovers.
    func testCorruptSeatBlobDegradesToEmpty() {
        let suite = "test.fmsg.\(UUID().uuidString)"
        let d = UserDefaults(suiteName: suite)!
        d.set(Data([0xff, 0x00, 0x13]), forKey: "fmsg.seats.v1")   // not our JSON
        let s = MessageGameStore(defaults: d)
        XCTAssertNil(s.seat(gameId: "g", chatKey: chatA), "a corrupt suite reads as no seat, never crashes")
        s.setSeat(gameId: "g", chatKey: chatA, seat: 0)            // and recovers on write
        XCTAssertEqual(s.seat(gameId: "g", chatKey: chatA), 0)
    }

    // MARK: - chat scoping (the cross-chat leak fix)

    /// A seat cached from Chat A must be invisible to a read scoped to Chat B, even
    /// though the same device's single App Group suite holds both — the leak the
    /// game-record cache used to guard against, now enforced on the seat store.
    func testSeatsAreScopedToTheirChat() {
        let s = freshStore()
        s.setSeat(gameId: "gA", chatKey: chatA, seat: 0)
        s.setSeat(gameId: "gB", chatKey: chatB, seat: 1)
        XCTAssertEqual(s.seat(gameId: "gA", chatKey: chatA), 0)
        XCTAssertEqual(s.seat(gameId: "gB", chatKey: chatB), 1)
        XCTAssertNil(s.seat(gameId: "gA", chatKey: chatB), "chat B must not see chat A's seat")
        XCTAssertNil(s.seat(gameId: "gB", chatKey: chatA), "chat A must not see chat B's seat")
    }

    // MARK: - ChatKey (what the scoping above is only as good as)

    /// The round-3 report: "every time I try to send a message it pulls up the
    /// same game for each chat, no matter who I'm texting." The store WAS
    /// scoped (above) — the key wasn't. `localParticipantIdentifier` is the same
    /// UUID in every conversation on a device, so keying on it alone keyed on
    /// the DEVICE, and every chat shared one key. Two different threads must
    /// produce two different keys even when the local identifier is identical.
    func testTwoChatsOnTheSameDeviceGetDifferentKeys() {
        let me = "LOCAL-SAME-EVERYWHERE"
        let withVera = ChatKey.make(local: me, remotes: ["vera"])
        let withBoris = ChatKey.make(local: me, remotes: ["boris"])
        XCTAssertNotEqual(withVera, withBoris,
                          "two DMs from one device must not share a game cache")
        let group = ChatKey.make(local: me, remotes: ["vera", "boris"])
        XCTAssertNotEqual(group, withVera, "a group is not the DM inside it")
        XCTAssertNotEqual(group, withBoris)
    }

    /// …and the same thread must key the same way every time it is opened, or a
    /// device would lose its own seat between launches. Nothing documents the
    /// order Messages returns `remoteParticipantIdentifiers` in, so the key is
    /// order-independent by construction.
    func testTheSameConversationKeysStablyRegardlessOfMemberOrder() {
        let a = ChatKey.make(local: "me", remotes: ["vera", "boris", "dima"])
        let b = ChatKey.make(local: "me", remotes: ["dima", "vera", "boris"])
        XCTAssertEqual(a, b, "member order must not change a conversation's identity")
        XCTAssertFalse(ChatKey.make(local: "", remotes: []).isEmpty,
                       "a degenerate conversation still gets a non-empty key")
    }
}
