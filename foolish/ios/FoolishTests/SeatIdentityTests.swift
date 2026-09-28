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

    // MARK: - two games at once, under two different nicknames (the 1.0 report)

    /// THE OWNER'S BUG, end to end over the real store and the real kernel
    /// gates: "I cannot play two large group games at the same time with
    /// different nicknames."
    ///
    /// Join group game A as "alex" (seat 2 of 4). Join group game B as "al"
    /// (seat 1 of 3). The device NICKNAME is one value, so the second join
    /// overwrote it - and game A's roster still, correctly, says "alex" at
    /// seat 2. Re-open A.
    ///
    /// The claim-time name lives on the ROW, so A's row still says "alex" and
    /// A's seat holds. The second half of this test is the defect itself,
    /// asserted rather than described: hand the very same gate the DEVICE
    /// NICKNAME - what both call sites passed before `SeatRow.name` existed -
    /// and seat 2 of game A is disowned, which is the §6.3 seat picker on the
    /// board and the Join button back in the lobby.
    ///
    /// MUTATION: make `MessageGameStore.setSeat` drop its `name` argument
    /// (`SeatRow(chatKey:seat:)`, the pre-fix row) and the four
    /// `theRowsName`/`claimName` assertions below fail - that is this test run
    /// against the pre-fix store, and it is how it was checked.
    func testTwoGroupGamesUnderTwoNicknamesEachKeepTheirOwnSeat() {
        let store = freshStore()
        // Game A, a 4-handed group. I am seat 2 and my join sealed "alex".
        let aJoins = [MessageJoin(seat: 0, name: "vera"), MessageJoin(seat: 1, name: "boris"),
                      MessageJoin(seat: 2, name: "alex"), MessageJoin(seat: 3, name: "dima")]
        store.nickname = "alex"
        store.setSeat(gameId: "A", chatKey: chatA, seat: 2, name: "alex")

        // Game B, a different group, joined as "al". ONE nickname per device,
        // so joining B renamed me everywhere - which is the whole bug.
        let bJoins = [MessageJoin(seat: 0, name: "kolya"), MessageJoin(seat: 1, name: "al"),
                      MessageJoin(seat: 2, name: "masha")]
        store.nickname = "al"
        store.setSeat(gameId: "B", chatKey: chatB, seat: 1, name: "al")

        XCTAssertEqual(store.claimName(gameId: "A"), "alex",
                       "two games are two rows, and each remembers the name IT was claimed under")
        XCTAssertEqual(store.claimName(gameId: "B"), "al")

        // Re-open A: still my seat, on the board and in the lobby.
        XCTAssertEqual(SeatIdentity.resolveOnBoard(cachedSeat: store.identity(gameId: "A").seat,
                                                   recordedName: store.identity(gameId: "A").name,
                                                   senderIsLocal: false, nPlayers: 4,
                                                   lastActorSeat: 0, joins: aJoins,
                                                   chatIsDM: false), .known(2),
                       "game A's board must still seat me at 2 after I joined game B as 'al'")
        let aClaim = store.claim(gameId: "A")
        XCTAssertEqual(SeatIdentity.resolveInLobby(cachedSeat: aClaim?.seat, senderIsLocal: false,
                                                   nPlayers: 4, lastActorSeat: 0, joins: aJoins,
                                                   chatIsDM: false, recordedName: aClaim?.name), 2,
                       "…and game A's lobby must not offer me Join for a seat I already hold")

        // THE DEFECT, asserted: the same gate, fed the device nickname.
        let asNickname = (seat: 2, name: store.nickname)
        XCTAssertEqual(SeatIdentity.resolveOnBoard(cachedSeat: asNickname.seat,
                                                   recordedName: asNickname.name,
                                                   senderIsLocal: false,
                                                   nPlayers: 4, lastActorSeat: 0, joins: aJoins,
                                                   chatIsDM: false), .ambiguous,
                       "passing the CURRENT nickname loses game A's seat - the reported bug")
        XCTAssertNil(SeatIdentity.resolveInLobby(cachedSeat: 2, senderIsLocal: false,
                                                 nPlayers: 4, lastActorSeat: 0, joins: aJoins,
                                                 chatIsDM: false, recordedName: store.nickname),
                     "…and offers Join for a seat that is mine")

        // The newest game was never broken - only every OTHER one was.
        XCTAssertEqual(SeatIdentity.resolveOnBoard(cachedSeat: store.identity(gameId: "B").seat,
                                                   recordedName: store.identity(gameId: "B").name,
                                                   senderIsLocal: false, nPlayers: 3,
                                                   lastActorSeat: 0, joins: bJoins,
                                                   chatIsDM: false), .known(1))
    }

    /// A RENAME MID-GAME is the same shape and must behave the same way. The
    /// human changes their nickname while one group game is in flight; that
    /// game's roster still carries the name their join sealed, and the row
    /// does too, so nothing is disowned.
    ///
    /// MUTATION: `SeatIdentity.resolveOnBoard` passing `nil` for
    /// `recordedName` would still pass this (nil is permissive) - which is why
    /// the test above, where a WRONG name is the failure mode, is the one that
    /// pins the behaviour, and this one exists to pin that a rename is not a
    /// disownment rather than to pin the plumbing.
    func testRenamingTheDeviceDoesNotDisownAnInFlightSeat() {
        let store = freshStore()
        let joins = [MessageJoin(seat: 0, name: "vera"), MessageJoin(seat: 1, name: "alex")]
        store.nickname = "alex"
        store.setSeat(gameId: "g", chatKey: chatA, seat: 1, name: "alex")
        store.nickname = "Alexander the Great"
        XCTAssertEqual(SeatIdentity.resolveOnBoard(cachedSeat: store.identity(gameId: "g").seat,
                                                   recordedName: store.identity(gameId: "g").name,
                                                   senderIsLocal: false, nPlayers: 2,
                                                   lastActorSeat: 0, joins: joins,
                                                   chatIsDM: true), .known(1))
    }

    /// THE FALLBACK THE FIX MUST NOT EAT: with NO row for a game, the device
    /// nickname is still offered as the name, because it is the only identity
    /// there is. That is §6.2 name recovery for a device that never claimed a
    /// seat in this game - and it is what the FoolishHarness seed demo leans on
    /// (`HarnessModel` seats its viewer by writing a nickname and nothing
    /// else), so narrowing `recordedName` to "the row's name, always" would
    /// have silently broken a dev tool while every test stayed green.
    ///
    /// The order matters and is asserted both ways: a row that HAS a name wins
    /// over the nickname, and a row with NO name (format 1) stays nil rather
    /// than falling back - falling back there is the reported bug.
    ///
    /// MUTATION (MessageGameStore.identity): return the nickname whenever
    /// `row.name` is nil (`row.name ?? nickname`) and the third assertion
    /// fails; return `row?.name` with no fallback at all and the first fails.
    func testTheDeviceNicknameIsTheNameOnlyWhenThereIsNoRow() {
        let store = freshStore()
        store.nickname = "alex"
        let joins = [MessageJoin(seat: 0, name: "vera"), MessageJoin(seat: 1, name: "alex"),
                     MessageJoin(seat: 2, name: "boris")]

        XCTAssertEqual(store.identity(gameId: "unseen").name, "alex",
                       "no row at all: the nickname is the only identity there is")
        XCTAssertNil(store.identity(gameId: "unseen").seat)
        XCTAssertEqual(SeatIdentity.resolveOnBoard(cachedSeat: store.identity(gameId: "unseen").seat,
                                                   recordedName: store.identity(gameId: "unseen").name,
                                                   senderIsLocal: false, nPlayers: 3,
                                                   lastActorSeat: 0, joins: joins,
                                                   chatIsDM: false), .known(1),
                       "…and the roster carrying it recovers the seat, as it did before")

        // A row WITH a name outranks the nickname.
        store.setSeat(gameId: "g", chatKey: chatA, seat: 2, name: "boris")
        XCTAssertEqual(store.identity(gameId: "g").name, "boris")

        // A row with NO name (format 1) stays nil - it must NOT fall back.
        store.setSeat(gameId: "g", chatKey: chatA, seat: 2, name: nil)
        XCTAssertNil(store.identity(gameId: "g").name,
                     "a nameless row is permissive; falling back to the nickname here is the bug")
        XCTAssertEqual(store.identity(gameId: "g").seat, 2)
    }

    /// THE SAFETY PROPERTY, which the fix must not weaken: a genuine seat-claim
    /// race still disowns. My row says seat 1, claimed as "alex"; the canonical
    /// chain gives seat 1 to Dima and carries "alex" nowhere. Answering 1 would
    /// put Dima's hand face-up on my screen and let me play it.
    ///
    /// Ambiguous, in a GROUP chat where §6.2 has no exact signal - Release
    /// renders that as the read-only spectator board. Asserted on the SAME
    /// entry points the two tests above use, so the permissive path and the
    /// refusing path cannot be told apart by which function was called.
    ///
    /// MUTATION: drop the disown step from `msg_seat_resolve_named`
    /// (c/src/msg_wire.c) - `const int cached = cached_seat;` - and the board
    /// assertion here returns .known(1). The C suite catches it too
    /// (msg_wire_test.c's board-gate block); this is the Swift-side proof that
    /// the entry the app actually calls is the one carrying that guard.
    func testALostSeatClaimRaceStillDisownsOnTheBoard() {
        let store = freshStore()
        let canonical = [MessageJoin(seat: 0, name: "vera"), MessageJoin(seat: 1, name: "dima"),
                         MessageJoin(seat: 2, name: "boris")]
        store.setSeat(gameId: "raced", chatKey: chatA, seat: 1, name: "alex")

        XCTAssertEqual(SeatIdentity.resolveOnBoard(cachedSeat: store.identity(gameId: "raced").seat,
                                                   recordedName: store.identity(gameId: "raced").name,
                                                   senderIsLocal: false, nPlayers: 3,
                                                   lastActorSeat: 0, joins: canonical,
                                                   chatIsDM: false), .ambiguous,
                       "seat 1 is Dima's now - my lost claim must NOT open his hand")
        let claim = store.claim(gameId: "raced")
        XCTAssertNil(SeatIdentity.resolveInLobby(cachedSeat: claim?.seat, senderIsLocal: false,
                                                 nPlayers: 3, lastActorSeat: 0, joins: canonical,
                                                 chatIsDM: false, recordedName: claim?.name),
                     "…and the lobby puts the Join button back so I claim a free seat")

        // The row is what refuses it: strip the name (a format-1 row) and the
        // number stands, which is the pre-name behaviour this fix preserves.
        XCTAssertEqual(SeatIdentity.resolveOnBoard(cachedSeat: 1, recordedName: nil,
                                                   senderIsLocal: false, nPlayers: 3,
                                                   lastActorSeat: 0, joins: canonical,
                                                   chatIsDM: false), .known(1),
                       "a nameless row is permissive - exactly as it was before rows had names")
    }

    /// The BOARD entry is the LOBBY entry minus its roster-membership check,
    /// and that difference is a rule: a live chain carries every seated player
    /// forward, so a seat this bubble does not list is a device not sealed in
    /// YET (a 2p DM receiver before their first move), not a seat that is not
    /// mine. Requiring it on the board would spectate a seated human.
    ///
    /// MUTATION: `msg_seat_resolve_on_board` passing `require_listed = 1` -
    /// the first assertion becomes .ambiguous. (3 failures in the C suite.)
    func testTheBoardDoesNotRequireTheRosterToListMeButTheLobbyDoes() {
        let creatorOnly = [MessageJoin(seat: 0, name: "vera")]
        XCTAssertEqual(SeatIdentity.resolveOnBoard(cachedSeat: 1, recordedName: nil,
                                                   senderIsLocal: false,
                                                   nPlayers: 2, lastActorSeat: 0,
                                                   joins: creatorOnly, chatIsDM: true), .known(1),
                       "a DM receiver whose join is not sealed yet is still seat 1 on the board")
        XCTAssertNil(SeatIdentity.resolveInLobby(cachedSeat: 1, senderIsLocal: false,
                                                 nPlayers: 2, lastActorSeat: 0,
                                                 joins: creatorOnly, chatIsDM: true),
                     "…and is NOT joined in a lobby whose own roster predates the join")
    }

    /// A re-affirming write must not erase the name. `commitPendingStage`
    /// (MessagesViewController) rewrites the row on every send and knows only
    /// a seat number, so it carries `claimName` forward; if it passed the
    /// device nickname instead, one send in game B would stamp B's name onto
    /// A's row and the two-game bug would be back through the back door.
    ///
    /// MUTATION: change that call site to `name: MessageGameStore.shared
    /// .nickname` - modelled here by passing the wrong name - and the seat is
    /// disowned again.
    func testResendingCarriesTheClaimNameForwardRatherThanTheNickname() {
        let store = freshStore()
        // A GROUP game, so that a disowned cache has no §6.2 signal to fall
        // back on and the damage is visible. (In a 2-player DM the "other
        // seat" inference would quietly re-derive the same seat, which is why
        // the owner's report is about LARGE group games.)
        let joins = [MessageJoin(seat: 0, name: "vera"), MessageJoin(seat: 1, name: "alex"),
                     MessageJoin(seat: 2, name: "boris")]
        store.setSeat(gameId: "A", chatKey: chatA, seat: 1, name: "alex")

        // What the send path does: same seat, the name carried forward.
        store.setSeat(gameId: "A", chatKey: chatA, seat: 1,
                      name: store.claimName(gameId: "A"))
        XCTAssertEqual(store.claimName(gameId: "A"), "alex", "a re-send must not erase the name")
        XCTAssertEqual(SeatIdentity.resolveOnBoard(cachedSeat: store.identity(gameId: "A").seat,
                                                   recordedName: store.identity(gameId: "A").name,
                                                   senderIsLocal: false, nPlayers: 3,
                                                   lastActorSeat: 0, joins: joins,
                                                   chatIsDM: false), .known(1))

        // What it must NOT do: stamp today's nickname over it.
        store.setSeat(gameId: "A", chatKey: chatA, seat: 1, name: "al")
        XCTAssertEqual(SeatIdentity.resolveOnBoard(cachedSeat: store.identity(gameId: "A").seat,
                                                   recordedName: store.identity(gameId: "A").name,
                                                   senderIsLocal: false, nPlayers: 3,
                                                   lastActorSeat: 0, joins: joins,
                                                   chatIsDM: false), .ambiguous,
                       "…which is the bug, arriving through the send path instead of the open path")
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
        s.setSeat(gameId: "g1", chatKey: chatA, seat: 1, name: nil)
        XCTAssertEqual(s.seat(gameId: "g1", chatKey: chatA), 1)
        XCTAssertNil(s.seat(gameId: "absent", chatKey: chatA))
        // A device only ever holds one seat in a game; the latest write wins.
        s.setSeat(gameId: "g1", chatKey: chatA, seat: 0, name: nil)
        XCTAssertEqual(s.seat(gameId: "g1", chatKey: chatA), 0)
    }

    /// A corrupt seat blob reads as no seat and never crashes; a write recovers.
    func testCorruptSeatBlobDegradesToEmpty() {
        let suite = "test.fmsg.\(UUID().uuidString)"
        let d = UserDefaults(suiteName: suite)!
        d.set(Data([0xff, 0x00, 0x13]), forKey: "fmsg.seats.v1")   // not our JSON
        let s = MessageGameStore(defaults: d)
        XCTAssertNil(s.seat(gameId: "g", chatKey: chatA), "a corrupt suite reads as no seat, never crashes")
        s.setSeat(gameId: "g", chatKey: chatA, seat: 0, name: nil)            // and recovers on write
        XCTAssertEqual(s.seat(gameId: "g", chatKey: chatA), 0)
    }

    // MARK: - chat scoping (the cross-chat leak fix)

    /// A seat cached from Chat A must be invisible to a read scoped to Chat B, even
    /// though the same device's single App Group suite holds both — the leak the
    /// game-record cache used to guard against, now enforced on the seat store.
    func testSeatsAreScopedToTheirChat() {
        let s = freshStore()
        s.setSeat(gameId: "gA", chatKey: chatA, seat: 0, name: nil)
        s.setSeat(gameId: "gB", chatKey: chatB, seat: 1, name: nil)
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
        s.setSeat(gameId: "g", chatKey: "old-participant-set", seat: 2, name: nil)

        // The same thread, after someone was added: new key, same game bubble.
        XCTAssertNil(s.seat(gameId: "g", chatKey: "new-participant-set"),
                     "the scoped read misses after the re-key (why the fallback exists)")
        XCTAssertEqual(s.seatForBubble(gameId: "g"), 2,
                       "a bubble in hand identifies its seat by gameId, whatever key the chat had")

        // The next adopt re-keys the row, healing the scoped read too.
        s.setSeat(gameId: "g", chatKey: "new-participant-set", seat: 2, name: nil)
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
