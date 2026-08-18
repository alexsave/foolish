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
                                     nPlayers: 4, lastActorSeat: 3, chatIsDM: false)
        XCTAssertEqual(r, .known(1))
    }

    func testSenderInferenceIsExactForAnyN() {
        // §6.2 S1: no cache, but THIS device sent the tapped bubble ⇒ I am its
        // last actor, exact regardless of player count or chat shape.
        XCTAssertEqual(SeatIdentity.resolve(cachedSeat: nil, senderIsLocal: true,
                                            nPlayers: 5, lastActorSeat: 2, chatIsDM: false), .known(2))
    }

    func testTwoPlayerInfersTheOtherSeatInADM() {
        // §6.2 S1: DM 2p, I did NOT send it ⇒ I must be the other of two seats —
        // sound ONLY because a DM has exactly two humans in it.
        XCTAssertEqual(SeatIdentity.resolve(cachedSeat: nil, senderIsLocal: false,
                                            nPlayers: 2, lastActorSeat: 0, chatIsDM: true), .known(1))
        XCTAssertEqual(SeatIdentity.resolve(cachedSeat: nil, senderIsLocal: false,
                                            nPlayers: 2, lastActorSeat: 1, chatIsDM: true), .known(0))
    }

    func testTwoPlayerInferenceIsRefusedInAGroupChat() {
        // The deadlocked-thread hardening: a 2-player game's bubble in a GROUP
        // chat can be tapped by any member — a bystander with no cache must NOT
        // be silently seated as "the other player" (that shows them that
        // player's hand and lets them move for them). Ambiguous instead, which
        // Release renders as the public spectator board.
        XCTAssertEqual(SeatIdentity.resolve(cachedSeat: nil, senderIsLocal: false,
                                            nPlayers: 2, lastActorSeat: 0, chatIsDM: false), .ambiguous)
    }

    func testThreePlusWithoutCacheOrSenderIsAmbiguous() {
        // §6.3: N≥3, no cache, not the last actor — the only honest answer is to
        // ask (the nickname picker), NOT to guess a seat.
        XCTAssertEqual(SeatIdentity.resolve(cachedSeat: nil, senderIsLocal: false,
                                            nPlayers: 4, lastActorSeat: 2, chatIsDM: false), .ambiguous)
    }

    func testStaleOutOfRangeCacheIsIgnoredNotTrusted() {
        // A cache from a different (bigger) game must never seat me out of range;
        // fall through to the live signals instead of returning .known(7).
        let r = SeatIdentity.resolve(cachedSeat: 7, senderIsLocal: false,
                                     nPlayers: 2, lastActorSeat: 0, chatIsDM: true)
        XCTAssertEqual(r, .known(1), "out-of-range cache ignored, DM 2p inference used")
    }

    // MARK: - the ghost-seat guard (a lost seat-claim race must not seat me)

    /// Two people claimed seat 2 off the same stale lobby bubble; this device's
    /// claim lost, so the canonical chain lists the OTHER person at seat 2.
    /// Trusting the cache would put their hand face-up on my screen.
    func testCacheDisownedWhenTheChainNamesSomeoneElseAtMySeat() {
        let joins = [MessageJoin(seat: 0, name: "Alex"), MessageJoin(seat: 1, name: "Sveta"),
                     MessageJoin(seat: 2, name: "Dima")]          // Dima won the race for 2
        XCTAssertTrue(SeatIdentity.cacheDisownedByJoins(cachedSeat: 2, recordedName: "Boris",
                                                        joins: joins),
                      "the chain says seat 2 is Dima; my row says I claimed it as Boris — not my seat")
    }

    func testCacheConfirmedWhenNamesAgreeOrEitherSideIsSilent() {
        let joins = [MessageJoin(seat: 0, name: "Alex"), MessageJoin(seat: 2, name: "Boris")]
        XCTAssertFalse(SeatIdentity.cacheDisownedByJoins(cachedSeat: 2, recordedName: "Boris",
                                                         joins: joins), "names agree — my seat")
        XCTAssertFalse(SeatIdentity.cacheDisownedByJoins(cachedSeat: 1, recordedName: "Boris",
                                                         joins: joins),
                       "no join at my seat — nothing disowns it (range checks still apply)")
        XCTAssertFalse(SeatIdentity.cacheDisownedByJoins(cachedSeat: 2, recordedName: nil,
                                                         joins: joins),
                       "no recorded name to compare — stay permissive")
        XCTAssertFalse(SeatIdentity.cacheDisownedByJoins(cachedSeat: nil, recordedName: "Boris",
                                                         joins: joins), "no cache, nothing to disown")
    }

    // MARK: - §6 resolution, gated for a lobby bubble (note 14, HARNESS_NOTES_R2)

    /// The exact bug note 14 describes: a cached seat that is NOT in this
    /// bubble's own `joins` must not read as joined — an older WAITING bubble,
    /// reopened after I've since joined elsewhere, would otherwise still hand
    /// me Start/Send for a lobby that does not list me.
    func testCachedSeatAbsentFromJoinsIsNotJoined() {
        let staleJoins = [MessageJoin(seat: 0, name: "Alex")]   // seat 1 (me) not in here yet
        let r = SeatIdentity.resolveInLobby(cachedSeat: 1, senderIsLocal: false,
                                            nPlayers: 8, lastActorSeat: 0, joins: staleJoins,
                                            chatIsDM: false)
        XCTAssertNil(r, "a cached seat this bubble's own joins does not list must not read as joined")
    }

    /// The flip side of the same bug: once the bubble's `joins` DOES list my
    /// cached seat (the freshest lobby bubble, post-join), I must resolve as
    /// joined again — the gate only rejects a MISMATCH, not every lobby.
    func testCachedSeatPresentInJoinsIsJoined() {
        let freshJoins = [MessageJoin(seat: 0, name: "Alex"), MessageJoin(seat: 1, name: "Sveta")]
        let r = SeatIdentity.resolveInLobby(cachedSeat: 1, senderIsLocal: false,
                                            nPlayers: 8, lastActorSeat: 0, joins: freshJoins,
                                            chatIsDM: false, recordedName: "Sveta")
        XCTAssertEqual(r, 1, "once this bubble's own joins list me, I resolve as joined")
    }

    /// Sender inference (S1) is gated the same way: even though I sent the
    /// stale bubble (so plain `resolve` would say `.known(lastActorSeat)`),
    /// that bubble's joins must still be checked.
    func testSenderInferredSeatAbsentFromJoinsIsNotJoined() {
        let staleJoins = [MessageJoin(seat: 0, name: "Alex")]
        let r = SeatIdentity.resolveInLobby(cachedSeat: nil, senderIsLocal: true,
                                            nPlayers: 8, lastActorSeat: 2, joins: staleJoins,
                                            chatIsDM: false)
        XCTAssertNil(r, "sender-inferred seat 2 is not in this stale bubble's joins")
    }

    /// `.ambiguous` still maps to nil either way (no seat to check joins against).
    func testAmbiguousStaysNilInLobby() {
        let r = SeatIdentity.resolveInLobby(cachedSeat: nil, senderIsLocal: false,
                                            nPlayers: 4, lastActorSeat: 2, joins: [],
                                            chatIsDM: false)
        XCTAssertNil(r)
    }

    /// The ghost-seat guard in the lobby: my cached seat is LISTED, but under
    /// somebody else's name — a claim race this device lost. nil is what puts
    /// the Join button back so I re-claim the next free seat (§5.2's "the
    /// loser's device re-claims on next open"), instead of reading as seated
    /// on a seat that is no longer mine.
    func testLobbySeatDisownedByNameOffersJoinAgain() {
        let joins = [MessageJoin(seat: 0, name: "Alex"), MessageJoin(seat: 1, name: "Dima")]
        let r = SeatIdentity.resolveInLobby(cachedSeat: 1, senderIsLocal: false,
                                            nPlayers: 8, lastActorSeat: 1, joins: joins,
                                            chatIsDM: false, recordedName: "Sveta")
        XCTAssertNil(r, "seat 1 is Dima's now — I must fall back to Join, not squat on it")
    }

    /// Name RECOVERY, the flip side of disownment (flow_sim_v3's find): I
    /// joined as "Sveta" at seat 1; a stale fork later showed Join again (my
    /// name wasn't on it) and I claimed seat 3 there; the FIRST fork won. My
    /// numeric cache says 3 — which the winning roster gives to Dima — but the
    /// roster still carries Sveta at 1. The seat wearing MY name is mine:
    /// resolve there, on the lobby and (via adopt's identical lookup) the
    /// board, instead of spectating my own game — which, when I am its first
    /// attacker, was a whole-table stall.
    func testALostSecondClaimRecoversTheFirstByName() {
        let joins = [MessageJoin(seat: 0, name: "Alex"), MessageJoin(seat: 1, name: "Sveta"),
                     MessageJoin(seat: 3, name: "Dima")]
        XCTAssertEqual(SeatIdentity.seatClaimedByName(recordedName: "Sveta", joins: joins), 1)
        XCTAssertEqual(SeatIdentity.resolveInLobby(cachedSeat: 3, senderIsLocal: false,
                                                   nPlayers: 8, lastActorSeat: 3, joins: joins,
                                                   chatIsDM: false, recordedName: "Sveta"), 1)
        XCTAssertNil(SeatIdentity.seatClaimedByName(recordedName: "Igor", joins: joins),
                     "a name the roster does not carry recovers nothing")
        XCTAssertNil(SeatIdentity.seatClaimedByName(recordedName: nil, joins: joins),
                     "no recorded name (no row) recovers nothing")
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

    /// Round 7 removed the preferred-chain game-record cache and the drawer list:
    /// `put`/`record`/`games`/`remove` are inert now, so the extension always
    /// renders the tapped bubble rather than a cached chain.
    func testGameRecordCacheIsInert() {
        let s = freshStore()
        s.put(MessageGameRecord(gameId: "g", chatKey: chatA, mySeat: 1, nPlayers: 2, round: 1,
                                turn: 4, phase: 2, finished: false, names: [:],
                                payloadBase32: "AAAA", updatedAt: 100))
        XCTAssertNil(s.record(gameId: "g", chatKey: chatA), "no game-record cache after round 7")
        XCTAssertTrue(s.games(chatKey: chatA).isEmpty, "no drawer list after round 7")
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

    /// Membership churn: ChatKey is the sorted participant-UUID set, so adding
    /// or removing a group member re-keys the conversation mid-game — after
    /// which every strictly-scoped read above misses and a seated player
    /// degrades to the spectator board. A TAPPED bubble carries the game's own
    /// random gameId, which is proof enough of which row it is: the
    /// bubble-anchored lookups must survive the re-key. The no-bubble listing
    /// (`games(chatKey:)`) has no such anchor and must STAY strictly scoped —
    /// that listing was the actual cross-chat leak.
    func testBubbleAnchoredLookupSurvivesAChatRekey() {
        let s = freshStore()
        s.put(rec("g", chatKey: "old-participant-set", seat: 2, at: 100))

        // The same thread, after someone was added: new key, same game bubble.
        XCTAssertNil(s.record(gameId: "g", chatKey: "new-participant-set"),
                     "the scoped read misses after the re-key (why the fallback exists)")
        XCTAssertEqual(s.recordForBubble(gameId: "g")?.mySeat, 2,
                       "a bubble in hand identifies its row by gameId, whatever key the chat had")
        XCTAssertEqual(s.seatForBubble(gameId: "g"), 2)
        XCTAssertTrue(s.games(chatKey: "new-participant-set").isEmpty,
                      "the keyless listing stays chat-scoped — it is the leak surface")

        // The next adopt re-keys the row, healing the scoped reads too.
        s.put(rec("g", chatKey: "new-participant-set", seat: 2, at: 200))
        XCTAssertEqual(s.seat(gameId: "g", chatKey: "new-participant-set"), 2)
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
