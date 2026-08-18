// The lobby (design §5.2, rewritten for lobby v2 — docs/IMESSAGE_LOBBY_V2.md,
// notes 19/20/25), end to end through the kernel — the send path a creator and
// each joiner take, with no Messages harness. Proves that a phase-0 bubble
// seals and decodes (seed + joins, zero actions), that a joiner appends the
// lowest free seat and the lobby NEVER auto-starts (no matter how many have
// joined, or that the wire's 8-seat cap is reached), and that Start — a
// separate, explicit action any joined player may take once 2+ have joined —
// re-derives the LOCKED seed at the real player count and seals a LIVE game
// the first attacker can then play. If this holds, the lobby is a UI over
// primitives that already agree with the C and wasm suites.
import XCTest
@testable import FoolishKit

@MainActor
final class MessageLobbyTests: XCTestCase {

    private func freshSeed(_ salt: UInt8) -> Data {
        Data((0..<32).map { UInt8(truncatingIfNeeded: $0 &* 7 &+ Int(salt)) | 1 })
    }

    /// A creator locks the seed in and opens an OPEN lobby: the kernel is dealt
    /// at the wire's MAX capacity (8) — not a chosen count, because lobby v2
    /// asks for none up front — and the sealed bubble is WAITING, carrying only
    /// the creator's own join. n_players==8 here is the open-lobby convention
    /// (see docs/IMESSAGE_LOBBY_V2.md), never a real 8-player game.
    func testCreatorSealsAnOpenWaitingBubble() async throws {
        let k = MessageKernel.shared
        try await k.newGame(seed: freshSeed(3), players: 8)   // fixes seed + open capacity
        let payload = try await k.seal(phase: 0, lastActorSeat: 0, gameId: 900,
                                              parent8: Data(repeating: 0, count: 8),
                                              joins: [MessageJoin(seat: 0, name: "Alex")])
        XCTAssertFalse(payload.isEmpty)

        let env = try await MessageEnvelope.decode(payload: payload, viewer: -1)
        XCTAssertEqual(env.phase, 0, "WAITING")
        XCTAssertEqual(env.nPlayers, 8, "the open-lobby convention: max capacity, not a chosen count")
        XCTAssertEqual(env.turn, 0, "a lobby carries no actions")
        XCTAssertEqual(env.round, 0)
        XCTAssertEqual(env.joins, [MessageJoin(seat: 0, name: "Alex")])
    }

    /// A joiner adopts the WAITING bubble, claims the lowest free seat, and
    /// reseals WAITING — the joins list grows, nothing else does. Unchanged
    /// mechanics from the old N>=3 lobby.
    func testJoinerAppendsLowestFreeSeatAndStaysWaiting() async throws {
        let k = MessageKernel.shared
        try await k.newGame(seed: freshSeed(5), players: 8)
        let created = try await k.seal(phase: 0, lastActorSeat: 0, gameId: 901,
                                              parent8: Data(repeating: 0, count: 8),
                                              joins: [MessageJoin(seat: 0, name: "Alex")])

        // Joiner adopts, then claims the lowest free seat (1).
        let env = try await MessageEnvelope.decode(payload: created, viewer: -1)
        let free = (0..<env.nPlayers).first { s in !env.joins.contains { $0.seat == s } }
        XCTAssertEqual(free, 1, "seat 0 is taken; the next free seat is 1")
        let joins2 = (env.joins + [MessageJoin(seat: free!, name: "Sveta")]).sorted { $0.seat < $1.seat }
        let payload2 = try await k.seal(phase: 0, lastActorSeat: free!, gameId: 901,
                                               parent8: MessageTurnController.firstEight(hex: env.digest),
                                               joins: joins2)

        let env2 = try await MessageEnvelope.decode(payload: payload2, viewer: -1)
        XCTAssertEqual(env2.phase, 0, "still WAITING with room to spare")
        XCTAssertEqual(env2.nPlayers, 8, "the open capacity never shrinks on a join")
        XCTAssertEqual(env2.joins.count, 2)
        XCTAssertEqual(env2.joins.map(\.name), ["Alex", "Sveta"])
        XCTAssertEqual(env2.lastActorSeat, 1, "the bubble records who just joined")
    }

    /// The old lobby auto-started the moment joins filled n_players. Lobby v2
    /// removes that entirely (notes 19/20/25: "unspecified player count until
    /// someone hits start") — joining NEVER flips the phase, no matter how many
    /// have joined, including reaching the wire's 8-seat cap.
    func testJoiningNeverAutoStartsEvenAtCapacity() async throws {
        let k = MessageKernel.shared
        try await k.newGame(seed: freshSeed(11), players: 8)
        var joins = [MessageJoin(seat: 0, name: "P0")]
        var payload = try await k.seal(phase: 0, lastActorSeat: 0, gameId: 950,
                                              parent8: Data(repeating: 0, count: 8), joins: joins)

        for seat in 1..<8 {
            let env = try await MessageEnvelope.decode(payload: payload, viewer: -1)
            XCTAssertEqual(env.phase, 0, "still WAITING before seat \(seat) joins")
            joins = (env.joins + [MessageJoin(seat: seat, name: "P\(seat)")]).sorted { $0.seat < $1.seat }
            payload = try await k.seal(phase: 0, lastActorSeat: seat, gameId: 950,
                                              parent8: MessageTurnController.firstEight(hex: env.digest),
                                              joins: joins)
        }

        // All 8 seats claimed — the cap is reached, and the lobby is STILL
        // WAITING: filling every seat is no longer special.
        let full = try await MessageEnvelope.decode(payload: payload, viewer: -1)
        XCTAssertEqual(full.phase, 0, "reaching the 8-seat cap does not start the game")
        XCTAssertEqual(full.joins.count, 8)
    }

    /// Start (lobby v2's new, explicit action): any joined player, once 2+ have
    /// joined, re-derives the game from the seed LOCKED at create at the ACTUAL
    /// joined count — here 3, though the lobby's own capacity was 8 — and seals
    /// a LIVE game the first attacker can play. Proves the reseat mechanism
    /// (MessageKernel.reseatResidentGame) end to end.
    func testStartRedealsAtTheActualJoinCountAndSealsLive() async throws {
        let k = MessageKernel.shared
        let gid: UInt64 = 902
        try await k.newGame(seed: freshSeed(9), players: 8)   // open lobby, cap 8
        var joins = [MessageJoin(seat: 0, name: "Alex")]
        var payload = try await k.seal(phase: 0, lastActorSeat: 0, gameId: gid,
                                              parent8: Data(repeating: 0, count: 8), joins: joins)

        // Two more join (seats 1, 2) — still WAITING/8 throughout (see the test
        // above), so only the THIRD join's payload is threaded into Start below.
        for (seat, name) in [(1, "Sveta"), (2, "Boris")] {
            let env = try await MessageEnvelope.decode(payload: payload, viewer: -1)
            joins = (env.joins + [MessageJoin(seat: seat, name: name)]).sorted { $0.seat < $1.seat }
            payload = try await k.seal(phase: 0, lastActorSeat: seat, gameId: gid,
                                              parent8: MessageTurnController.firstEight(hex: env.digest),
                                              joins: joins)
        }

        let lobbyEnv = try await MessageEnvelope.decode(payload: payload, viewer: -1)
        XCTAssertEqual(lobbyEnv.phase, 0, "3 joined, still an open lobby — nobody hit Start yet")
        XCTAssertEqual(lobbyEnv.joins.count, 3)

        // Start, as seat 1 (Sveta) — ANY joined player, not just the creator or
        // the last joiner: re-adopt the lobby, reseat at joins.count (3), seal LIVE.
        _ = try await k.decode(payload: payload, viewer: -1)
        try await k.reseatResidentGame(players: lobbyEnv.joins.count)
        let live = try await k.seal(phase: 2, lastActorSeat: 1, gameId: gid,
                                          parent8: MessageTurnController.firstEight(hex: lobbyEnv.digest),
                                          joins: lobbyEnv.joins)

        let liveEnv = try await MessageEnvelope.decode(payload: live, viewer: -1)
        XCTAssertEqual(liveEnv.phase, 2, "Start seals a LIVE handoff")
        XCTAssertEqual(liveEnv.nPlayers, 3, "the REAL count (3), not the lobby's open capacity (8)")
        XCTAssertEqual(liveEnv.turn, 0, "no move yet — just the deal")
        XCTAssertEqual(liveEnv.joins.count, 3, "everyone who joined is named")

        // Somebody can actually act on the freshly-dealt 3p game (first attacker).
        var someoneCanPlay = false
        for s in 0..<liveEnv.nPlayers where await k.residentLegal(seat: s).contains(where: { $0.type != .wait }) {
            someoneCanPlay = true; break
        }
        XCTAssertTrue(someoneCanPlay, "a live game has a first attacker with a legal move")
    }

    // MARK: - note 15: Rule P extended to lobby (WAITING) bubbles

    /// The pure comparison `MessagesRootView.load()` gates a stale-lobby
    /// adoption on: a cached row strictly LATER than the incoming (always
    /// phase-0) bubble wins; no cache, or nothing past WAITING, does not.
    func testLobbyCachePreferredOnlyWhenCacheIsStrictlyLater() {
        XCTAssertTrue(MessageGameStore.lobbyCachePreferred(cachedPhase: 2, incomingPhase: 0),
                      "a cached LIVE game must win over a stale WAITING bubble")
        XCTAssertTrue(MessageGameStore.lobbyCachePreferred(cachedPhase: 3, incomingPhase: 0),
                      "a cached FINISHED game must win too")
        XCTAssertFalse(MessageGameStore.lobbyCachePreferred(cachedPhase: 0, incomingPhase: 0),
                       "two WAITING lobbies at the same phase: the incoming one is not stale")
        XCTAssertFalse(MessageGameStore.lobbyCachePreferred(cachedPhase: nil, incomingPhase: 0),
                       "nothing cached yet — the incoming lobby is all we know")
    }

    /// End-to-end: a viewer's device cached the game after it went LIVE
    /// (mirrors GameSurface.cache(), called on every adopt), then taps a STALE
    /// WAITING invite bubble for the SAME game. The phantom-8-player bug (note
    /// 15) was: `env.phase == 0` returned the lobby unconditionally, with no
    /// cache lookup at all. Proves the primitive the fix now consults would
    /// correctly steer `load()` to adopt the cached LIVE chain instead.
    func testStaleWaitingBubbleWouldBeSupersededByALaterCachedPhase() async throws {
        let k = MessageKernel.shared
        let gid: UInt64 = 903
        try await k.newGame(seed: freshSeed(21), players: 8)
        let waiting = try await k.seal(phase: 0, lastActorSeat: 0, gameId: gid,
                                             parent8: Data(repeating: 0, count: 8),
                                             joins: [MessageJoin(seat: 0, name: "Alex")])
        let waitingEnv = try await MessageEnvelope.decode(payload: waiting, viewer: -1)
        XCTAssertEqual(waitingEnv.phase, 0)

        // The game went LIVE elsewhere (e.g. this device itself started it) and
        // got cached at that later phase.
        try await k.reseatResidentGame(players: 3)
        let live = try await k.seal(phase: 2, lastActorSeat: 0, gameId: gid,
                                          parent8: MessageTurnController.firstEight(hex: waitingEnv.digest),
                                          joins: [MessageJoin(seat: 0, name: "Alex")])
        let liveEnv = try await MessageEnvelope.decode(payload: live, viewer: -1)
        XCTAssertEqual(liveEnv.phase, 2)

        // The exact gate `load()` runs before showing the tapped (stale)
        // WAITING bubble: is the cached phase strictly later?
        XCTAssertTrue(MessageGameStore.lobbyCachePreferred(cachedPhase: liveEnv.phase,
                                                            incomingPhase: waitingEnv.phase),
                      "the cached LIVE game must supersede the stale WAITING invite")
    }

    // MARK: - note 2: "join then start" and "join and start" are the same deal

    /// The two lobby-v3 Start routes — an already-joined player tapping Start
    /// after a plain Join, vs a fresh joiner tapping "Join and start" — must
    /// deal the IDENTICAL game: same seed, same final player count, so the
    /// hand is fixed the instant the creator sends the first chat and cannot
    /// be rerolled by which route got taken. Both call the same
    /// `MessageKernel.startFromLobby` primitive (`MessagesRootView.startGame`
    /// / `.joinAndStart`); this proves it end to end for both seats' hands.
    func testJoinThenStartAndJoinAndStartDealIdenticalHands() async throws {
        let k = MessageKernel.shared
        let seed = freshSeed(33)

        // The creator's WAITING lobby, seat 0 only — shared starting point for
        // both routes below (mirrors GameSurface.createWaiting for a DM: cap 2).
        try await k.newGame(seed: seed, players: 2)
        let created = try await k.seal(phase: 0, lastActorSeat: 0, gameId: 904,
                                             parent8: Data(repeating: 0, count: 8),
                                             joins: [MessageJoin(seat: 0, name: "Alex")])
        let createdEnv = try await MessageEnvelope.decode(payload: created, viewer: -1)

        // Route 1: "join then start" — Sveta joins (reseals WAITING, mirrors
        // joinLobby), THEN a separate Start reseats+seals LIVE (mirrors
        // startGame), reading joins off the ALREADY-RESEALED WAITING chain.
        let joins2 = [MessageJoin(seat: 0, name: "Alex"), MessageJoin(seat: 1, name: "Sveta")]
        let waiting2 = try await k.seal(phase: 0, lastActorSeat: 1, gameId: 904,
                                              parent8: MessageTurnController.firstEight(hex: createdEnv.digest),
                                              joins: joins2)
        let waiting2Env = try await MessageEnvelope.decode(payload: waiting2, viewer: -1)
        let route1Live = try await k.startFromLobby(
            lobbyPayload: waiting2, gameId: 904, actingSeat: 0,
            parent8: MessageTurnController.firstEight(hex: waiting2Env.digest), joins: waiting2Env.joins)

        // Route 2: "join and start" — Sveta reseats+seals LIVE directly off the
        // ORIGINAL 1-join lobby chain, with a `joins` list she assembled
        // locally (mirrors joinAndStart) — her own join never exists as its
        // own sealed WAITING bubble.
        let route2Live = try await k.startFromLobby(
            lobbyPayload: created, gameId: 904, actingSeat: 1,
            parent8: MessageTurnController.firstEight(hex: createdEnv.digest), joins: joins2)

        let env1 = try await MessageEnvelope.decode(payload: route1Live, viewer: -1)
        let env2 = try await MessageEnvelope.decode(payload: route2Live, viewer: -1)
        XCTAssertEqual(env1.phase, 2); XCTAssertEqual(env2.phase, 2)
        XCTAssertEqual(env1.nPlayers, 2); XCTAssertEqual(env2.nPlayers, 2)

        // The actual proof: the SAME cards land in each seat's hand under both
        // routes — re-decode each chain per seat and compare.
        _ = try await k.decode(payload: route1Live, viewer: 0)
        let route1Seat0 = (await k.residentView(viewer: 0))?.me?.hand?.map(\.identity).sorted()
        _ = try await k.decode(payload: route1Live, viewer: 1)
        let route1Seat1 = (await k.residentView(viewer: 1))?.me?.hand?.map(\.identity).sorted()

        _ = try await k.decode(payload: route2Live, viewer: 0)
        let route2Seat0 = (await k.residentView(viewer: 0))?.me?.hand?.map(\.identity).sorted()
        _ = try await k.decode(payload: route2Live, viewer: 1)
        let route2Seat1 = (await k.residentView(viewer: 1))?.me?.hand?.map(\.identity).sorted()

        XCTAssertNotNil(route1Seat0); XCTAssertNotNil(route1Seat1)
        XCTAssertEqual(route1Seat0, route2Seat0, "seat 0's hand must be identical between the two routes")
        XCTAssertEqual(route1Seat1, route2Seat1, "seat 1's hand must be identical between the two routes")
    }

    /// The anti-reroll guarantee itself (note 2): the deal follows the seed
    /// LOCKED into the lobby chain, not whatever game happens to be resident
    /// when Start is pressed.
    ///
    /// This exists because the route-equivalence test above does NOT actually
    /// pin `startFromLobby`'s re-adopt: both of its routes run back to back off
    /// the same already-resident seed, so deleting the `decode(payload:
    /// lobbyPayload:)` line leaves it passing (verified by mutation). That is a
    /// test green against a broken artifact. The real hazard the re-adopt
    /// guards is a DIFFERENT game being resident at Start — the extension is a
    /// single kernel that every chat, lobby and board decodes through, so by
    /// the time a human taps Start the resident game routinely belongs to
    /// something else entirely (§7.3: decoding adopts). Without the re-adopt
    /// that deals a hand from the wrong seed, silently.
    ///
    /// So: seal a lobby, POLLUTE the resident kernel with an unrelated game at
    /// a different seed and seat count, then Start the original lobby and
    /// require the hands to match the unpolluted control exactly.
    func testStartFromLobbyReDerivesTheLockedSeedNotTheResidentGame() async throws {
        let k = MessageKernel.shared
        let joins = [MessageJoin(seat: 0, name: "Alex"), MessageJoin(seat: 1, name: "Sveta")]

        // The lobby whose seed is locked, plus the control deal taken straight
        // off it with nothing else touched in between.
        try await k.newGame(seed: freshSeed(41), players: 2)
        let lobby = try await k.seal(phase: 0, lastActorSeat: 0, gameId: 905,
                                     parent8: Data(repeating: 0, count: 8),
                                     joins: [MessageJoin(seat: 0, name: "Alex")])
        let lobbyEnv = try await MessageEnvelope.decode(payload: lobby, viewer: -1)
        let parent = MessageTurnController.firstEight(hex: lobbyEnv.digest)
        let control = try await k.startFromLobby(lobbyPayload: lobby, gameId: 905,
                                                 actingSeat: 1, parent8: parent, joins: joins)
        _ = try await k.decode(payload: control, viewer: 0)
        let controlSeat0 = (await k.residentView(viewer: 0))?.me?.hand?.map(\.identity).sorted()
        XCTAssertNotNil(controlSeat0)

        // A wholly unrelated game becomes resident — a different seed AND a
        // different seat count, so a Start that reseats whatever is resident
        // instead of re-adopting the lobby cannot coincidentally agree.
        try await k.newGame(seed: freshSeed(77), players: 5)
        _ = try await k.seal(phase: 0, lastActorSeat: 0, gameId: 906,
                             parent8: Data(repeating: 0, count: 8),
                             joins: [MessageJoin(seat: 0, name: "Someone else")])

        let started = try await k.startFromLobby(lobbyPayload: lobby, gameId: 905,
                                                 actingSeat: 1, parent8: parent, joins: joins)
        _ = try await k.decode(payload: started, viewer: 0)
        let startedSeat0 = (await k.residentView(viewer: 0))?.me?.hand?.map(\.identity).sorted()
        XCTAssertEqual(startedSeat0, controlSeat0,
                       "Start must deal from the lobby's LOCKED seed, not the resident game")
    }
    // MARK: - a lobby always offers SOMETHING

    /// The owner hit a lobby listing one player with not a single control on
    /// it: joined (so no Join), fewer than two players (so no Start), and the
    /// invite button had been removed as redundant with the auto-stage — which
    /// it is only while that staged bubble is still in the compose field. Once
    /// it is sent or deleted, that screen is a dead end.
    ///
    /// This enumerates every (mySeat, joined, capacity) a lobby can be in and
    /// asserts each maps to an action. It is a total function now, so "no
    /// control" is not expressible — the point of pulling the branch out of the
    /// view. The specific regression is the joined-alone case.
    ///
    /// Round-5 M9 added a 4th parameter, `iSentTheInvite`, that further narrows
    /// `.start` to `.waiting` for a joined player who is BOTH the newest
    /// bubble's sender AND still short of a full lobby (see
    /// `LobbyControls.offered`'s doc). Every call below omits it, so it
    /// defaults to `false` and every `.start` assertion here is completely
    /// untouched by that change — this file stays about the pre-M9 shape of
    /// the gate. `Round5LobbyTests.swift` is where the `iSentTheInvite: true`
    /// side of the gate (and the full-lobby exemption) is pinned.
    func testEveryLobbyStateOffersAnAction() {
        XCTAssertEqual(LobbyControls.offered(mySeat: 0, joined: 1, capacity: 8), .invite,
                       "joined but alone: the only useful action is to invite someone")
        XCTAssertEqual(LobbyControls.offered(mySeat: 0, joined: 2, capacity: 8), .start)
        XCTAssertEqual(LobbyControls.offered(mySeat: 0, joined: 8, capacity: 8), .start,
                       "a full lobby I am in still starts")
        XCTAssertEqual(LobbyControls.offered(mySeat: nil, joined: 1, capacity: 2), .join)
        XCTAssertEqual(LobbyControls.offered(mySeat: nil, joined: 2, capacity: 2), .full,
                       "no room and not mine: nothing to do but wait")

        // Exhaustive: no combination may be unreachable or ambiguous.
        for capacity in 2...8 {
            for joined in 0...capacity {
                for seat in [nil, 0] as [Int?] {
                    let a = LobbyControls.offered(mySeat: seat, joined: joined, capacity: capacity)
                    if seat != nil {
                        XCTAssertTrue(a == .start || a == .invite,
                                      "a joined player is offered start or invite, never join/full")
                    } else {
                        XCTAssertTrue(a == .join || a == .full,
                                      "a non-joined player is offered join or full")
                    }
                }
            }
        }
    }
    /// Rule P, through the SWIFT binding, on the exact pair the group-chat fork
    /// produced: my own cached lobby reseal vs the LIVE game someone started
    /// from it. `MessageSurfaceRouter` compares these two and opens the winner,
    /// so if the lobby wins here the extension shows a lobby while the thread is
    /// mid-game — "there is a game currently in play, with the current player in
    /// it, yet the extension is stuck on the lobby".
    ///
    /// The C test (msg_wire_test) pins the RULE. This pins that the app's linked
    /// kernel carries it — not the same thing, and exactly how this bug survived
    /// a "fix": FoolishKit links a PREBUILT vendor/Foolish.xcframework, so a C
    /// change reaches the app only after `make ios-lib`. An earlier single-pair
    /// version of this test passed against the stale library on a digest
    /// coin-flip.
    ///
    /// So it runs many pairs and REQUIRES that some have the lobby's digest
    /// sorting first — the ones the tiebreak alone would get wrong. Same guard
    /// as the C test, for the same reason.
    func testAStartedGameBeatsMyOwnCachedLobbyThroughTheKernelBinding() async throws {
        let k = MessageKernel.shared
        var lobbyDigestFirst = 0

        for i in 0..<12 {
            let gid = UInt64(5150 + i)
            try await k.newGame(seed: freshSeed(UInt8(40 + i)), players: 8)
            let joins3 = [MessageJoin(seat: 0, name: "Alex"), MessageJoin(seat: 1, name: "Vera"),
                          MessageJoin(seat: 2, name: "Boris")]
            let lobby = try await k.seal(phase: 0, lastActorSeat: 2, gameId: gid,
                                         parent8: Data(repeating: 0, count: 8), joins: joins3)
            let lobbyEnv = try await MessageEnvelope.decode(payload: lobby, viewer: -1)

            let joins5 = joins3 + [MessageJoin(seat: 3, name: "Dima"),
                                   MessageJoin(seat: 4, name: "Katya")]
            let live = try await k.startFromLobby(
                lobbyPayload: lobby, gameId: gid, actingSeat: 3,
                parent8: MessageTurnController.firstEight(hex: lobbyEnv.digest), joins: joins5)
            let liveEnv = try await MessageEnvelope.decode(payload: live, viewer: -1)
            XCTAssertEqual(liveEnv.phase, 2, "fixture: the started game is LIVE")
            XCTAssertEqual(liveEnv.round, lobbyEnv.round, "fixture: same round — the tie the rule must break")
            XCTAssertEqual(liveEnv.turn, lobbyEnv.turn, "fixture: same turn")
            if lobbyEnv.digest < liveEnv.digest { lobbyDigestFirst += 1 }

            let a = try await k.preferred(lobby, live)
            let b = try await k.preferred(live, lobby)
            XCTAssertGreaterThan(a, 0, "game \(gid): the started game must beat the lobby it grew out of")
            XCTAssertLessThan(b, 0, "game \(gid): and the comparison must be symmetric")
        }

        XCTAssertGreaterThan(lobbyDigestFirst, 0,
                             "no pair had the lobby's digest sorting first — this run could not "
                             + "have caught a kernel without the phase rule (e.g. a stale "
                             + "vendor/Foolish.xcframework)")
    }

    // MARK: - Rule P rule 3: the double-Start fork (the 4-player incident)

    /// Any joined player may Start, and Start deals at the tapped bubble's join
    /// count — so two players starting near-simultaneously (or one starting off
    /// a stale bubble that predates the last join) seal TWO LIVE handoffs, both
    /// round 0 / turn 0, dealt from the SAME locked seed at DIFFERENT player
    /// counts. Those are different games: different trump, different first
    /// attacker. Under the digest tiebreak the smaller fork won half the time;
    /// when the full game's first attacker was the player stranded on the small
    /// fork's board, every screen in the chat waited on a player whose own
    /// screen said someone else must open — the shipped 4-player deadlock.
    ///
    /// Kernel rule 3 (msg_wire.h): at an equal (round, turn), the fuller roster
    /// wins, before the digest. This seed (salt 1) is chosen so the fixture
    /// genuinely poses the old bug: its two forks disagree about the first
    /// attacker AND the 3-player fork's digest sorts first — against a pre-rule-3
    /// kernel both `preferred` assertions below fail.
    func testFullerStartBeatsAStaleSmallerStart() async throws {
        let k = MessageKernel.shared
        let gid: UInt64 = 904
        try await k.newGame(seed: freshSeed(1), players: 8)   // open lobby, cap 8
        var payload = try await k.seal(phase: 0, lastActorSeat: 0, gameId: gid,
                                       parent8: Data(repeating: 0, count: 8),
                                       joins: [MessageJoin(seat: 0, name: "Alex")])
        var stale3: Data?          // the 3-join lobby a stale Start races from
        for (seat, name) in [(1, "Sveta"), (2, "Boris"), (3, "Dima")] {
            let env = try await MessageEnvelope.decode(payload: payload, viewer: -1)
            let joins = (env.joins + [MessageJoin(seat: seat, name: name)]).sorted { $0.seat < $1.seat }
            payload = try await k.seal(phase: 0, lastActorSeat: seat, gameId: gid,
                                       parent8: MessageTurnController.firstEight(hex: env.digest),
                                       joins: joins)
            if joins.count == 3 { stale3 = payload }
        }

        // Alex starts from the full 4-join lobby…
        let fullLobby = try await MessageEnvelope.decode(payload: payload, viewer: -1)
        let live4 = try await k.startFromLobby(
            lobbyPayload: payload, gameId: gid, actingSeat: 0,
            parent8: MessageTurnController.firstEight(hex: fullLobby.digest), joins: fullLobby.joins)
        let env4 = try await MessageEnvelope.decode(payload: live4, viewer: -1)
        let fa4 = await k.residentView(viewer: -1)?.firstAttacker ?? -1

        // …while Sveta, whose device has not seen Dima's join yet, starts from
        // her stale 3-join view of the same lobby chain.
        let staleEnv = try await MessageEnvelope.decode(payload: stale3!, viewer: -1)
        let live3 = try await k.startFromLobby(
            lobbyPayload: stale3!, gameId: gid, actingSeat: 1,
            parent8: MessageTurnController.firstEight(hex: staleEnv.digest), joins: staleEnv.joins)
        let env3 = try await MessageEnvelope.decode(payload: live3, viewer: -1)
        let fa3 = await k.residentView(viewer: -1)?.firstAttacker ?? -1

        // The fixture is the real thing: two LIVE turn-0 chains of one game id,
        // different sizes, disagreeing about the opener, with the smaller one's
        // digest sorting first (what the old rule wrongly rewarded).
        XCTAssertEqual(env4.phase, 2); XCTAssertEqual(env3.phase, 2)
        XCTAssertEqual(env4.nPlayers, 4); XCTAssertEqual(env3.nPlayers, 3)
        XCTAssertEqual(env4.turn, 0); XCTAssertEqual(env3.turn, 0)
        XCTAssertNotEqual(fa4, fa3, "this seed's forks disagree about the opener — the deadlock ingredient")
        XCTAssertLessThan(env3.digest, env4.digest,
                          "fixture must pose the digest coin-flip the old rule lost")

        // Rule 3: every device prefers the fuller start, in both directions.
        XCTAssertLessThan(try await k.preferred(live4, live3), 0,
                          "the full 4-player game must beat the stale 3-player start")
        XCTAssertGreaterThan(try await k.preferred(live3, live4), 0,
                             "and the comparison must be symmetric")

        // But a chain someone actually PLAYED on still out-ranks a wider turn-0
        // start: rule 3 sits below turn, so real progress is never clobbered.
        _ = try await k.decode(payload: live3, viewer: -1)
        var played3: Data?
        for s in 0..<env3.nPlayers {
            if let m = await k.residentLegal(seat: s).first(where: { $0.type != .wait }) {
                try await k.apply(seat: s, move: m)
                played3 = try await k.seal(phase: 2, lastActorSeat: s, gameId: gid,
                                           parent8: MessageTurnController.firstEight(hex: env3.digest),
                                           joins: env3.joins)
                break
            }
        }
        let playedEnv = try await MessageEnvelope.decode(payload: played3!, viewer: -1)
        XCTAssertEqual(playedEnv.turn, 1)
        XCTAssertLessThan(try await k.preferred(played3!, live4), 0,
                          "a played-on chain must not be clobbered by a stale wider Start")
    }
}
