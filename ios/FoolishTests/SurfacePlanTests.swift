// SurfacePlanTests - 1.1(56): what an open lobby does with a text that lands on
// it, proved through the real kernel crossing rather than against a mock.
//
// THE REPORT, owner: "LOBBY DID NOT UPDATE LIVE! I was in lobby, got a start
// game text, and it was stuck on lobby! I think it should fade from lobby to the
// game in this case." A single text can carry several actions -
// `conversation.insert` REPLACES an unsent draft, so Join, then the rules
// checkbox, then Start go out as ONE envelope - and the surface has to play them
// in order instead of jumping to the end.
//
// FIVE STREAMS, and they are the whole set: a message comes from one
// participant, so they seat themselves or get up once, they must hold a seat to
// move the rules, and Start is always last.
//
//   join / leave     -> snap  (no beats: the ordinary adopt IS the snap)
//   passing moved    -> rotate
//   join + passing   -> snap, rest, rotate   - and it ENDS IN THE LOBBY
//   join + start     -> snap, rest, fade
//   start            -> fade
//
// The RULES are pinned natively (c/tests/msg_wire_test.c's test_surface_delta,
// c/tests/anim_plan_test.c's test_surface_plan) and the packed wire in
// c/ios/ios_api_smoke.c. What is proved HERE is the thing none of those can
// reach: that the Swift the extension actually calls - real sealed chains, the
// real kernel actor, `SurfacePlan`'s own decode - reports those beats. Every
// earlier round of this file's neighbours learned the same lesson the hard way:
// a green run against a hand-built fixture is evidence about the fixture.
//
// MUTATION MATRIX (each run against the change it names, each must FAIL):
//   * SurfacePlan's decode reading `passing` where `transition` sits ->
//     testJoinAndStartSnapsThenFades, testRulesAloneRotates
//   * FIO_SURFACE_STRIDE as 6 -> every beat-shaped assertion
//   * anim_surface_plan not collapsing a lone snap -> testJoinAloneStagesNothing
//   * the game-id guard in msg_surface_delta -> testAnotherGamesBubbleIsASwitch
import XCTest
@testable import FoolishKit

@MainActor
final class SurfacePlanTests: XCTestCase {

    private let k = MessageKernel.shared
    private func seed(_ salt: UInt8) -> Data {
        Data((0..<32).map { UInt8(truncatingIfNeeded: $0 &* 11 &+ Int(salt)) | 1 })
    }
    private let alex = MessageJoin(seat: 0, name: "Alex")
    private let vera = MessageJoin(seat: 1, name: "Vera")

    /// A lobby with `joins` seated, off a freshly locked seed.
    private func lobby(_ joins: [MessageJoin], game: UInt64, salt: UInt8,
                       passing: Bool = true, actor: Int? = nil) async throws -> Data {
        try await k.newGame(seed: seed(salt), players: 8)
        await k.setPassing(passing)
        return try await k.seal(phase: 0, lastActorSeat: actor ?? joins.map(\.seat).max() ?? 0,
                                gameId: game, parent8: Data(repeating: 0, count: 8),
                                joins: joins)
    }

    /// A LIVE handoff sealed off `lobbyPayload`, the way Start does it.
    private func started(from lobbyPayload: Data, joins: [MessageJoin],
                         game: UInt64) async throws -> Data {
        let env = try await MessageEnvelope.decode(payload: lobbyPayload, viewer: -1)
        return try await k.startFromLobby(lobbyPayload: lobbyPayload, gameId: game,
                                          actingSeat: joins.count - 1,
                                          parent8: MessageTurnController.firstEight(hex: env.digest),
                                          joins: joins)
    }

    // MARK: the five streams

    /// 1. A JOIN, on its own. Owner: "just snap to the state where they are in
    /// the lobby and do nothing else." No beats at all - the adopt the surface
    /// was going to do IS the snap, and staging one would cost a second render
    /// and buy nothing.
    func testJoinAloneStagesNothing() async throws {
        let one = try await lobby([alex], game: 5001, salt: 1)
        let two = try await lobby([alex, vera], game: 5001, salt: 1)
        let plan = await k.surfacePlan(showing: one, arriving: two)
        XCTAssertTrue(plan.beats.isEmpty, "a lone join is a snap, and a snap needs no sequence")
    }

    /// 1b. A LEAVE is a roster change like any other, and snaps the same way.
    /// A leave COMPACTS the seats below it, so the arriving roster is not a
    /// prefix of the one on screen - which must not turn it into something else.
    func testLeaveAloneStagesNothing() async throws {
        let both = try await lobby([alex, vera], game: 5002, salt: 2)
        let gone = try await lobby([MessageJoin(seat: 0, name: "Vera")], game: 5002, salt: 2)
        let plan = await k.surfacePlan(showing: both, arriving: gone)
        XCTAssertTrue(plan.beats.isEmpty, "a leave snaps too")
    }

    /// 2. THE RULES MOVED, with nobody joining: one beat, and the checkbox
    /// TURNS (owner: "lets do the 'rotate in' or out thing for the checkbox").
    /// No snap in front of it - there is no roster change to show.
    func testRulesAloneRotates() async throws {
        let on = try await lobby([alex, vera], game: 5003, salt: 3, passing: true)
        let off = try await lobby([alex, vera], game: 5003, salt: 3, passing: false)
        let plan = await k.surfacePlan(showing: on, arriving: off)
        XCTAssertEqual(plan.beats.count, 1)
        XCTAssertEqual(plan.beats.first?.kind, .rules)
        XCTAssertEqual(plan.beats.first?.transition, .turn)
        XCTAssertEqual(plan.beats.first?.passing, false, "it shows the NEW rule")
        XCTAssertEqual(plan.beats.first?.start, 0, "with nothing to wait for")
        XCTAssertGreaterThan(plan.beats.first?.duration ?? 0, 0)
    }

    /// 3. A JOIN AND A RULE CHANGE in one text: snap, rest, rotate - and it ENDS
    /// IN THE LOBBY. A stream that begins with a snap is not on its way to the
    /// table, and an implementation that assumes it is fails exactly here.
    func testJoinThenRulesEndsInTheLobby() async throws {
        let one = try await lobby([alex], game: 5004, salt: 4, passing: true)
        let two = try await lobby([alex, vera], game: 5004, salt: 4, passing: false)
        let plan = await k.surfacePlan(showing: one, arriving: two)
        XCTAssertEqual(plan.beats.map(\.kind), [.roster, .rules])
        XCTAssertEqual(plan.beats.map(\.transition), [.snap, .turn])
        XCTAssertEqual(plan.beats.first?.passing, true,
                       "the roster snaps in under the OLD rule; the rule moves after it")
        XCTAssertNotEqual(plan.beats.last?.kind, .board, "this stream ends in the lobby")
        XCTAssertGreaterThan(plan.beats[1].start, 0, "and it rests in between")
    }

    /// 4. A JOIN AND A START in one text - the report itself. Owner: "it should
    /// snap to the state where there are two or whatever people in the lobby,
    /// wait a bit, then fade."
    func testJoinAndStartSnapsThenFades() async throws {
        let one = try await lobby([alex], game: 5005, salt: 5)
        let two = try await lobby([alex, vera], game: 5005, salt: 5)
        let live = try await started(from: two, joins: [alex, vera], game: 5005)
        let plan = await k.surfacePlan(showing: one, arriving: live)
        XCTAssertEqual(plan.beats.map(\.kind), [.roster, .board])
        XCTAssertEqual(plan.beats.map(\.transition), [.snap, .fade])
        XCTAssertEqual(plan.beats[0].start, 0)
        XCTAssertGreaterThan(plan.beats[1].start, 0, "the table waits a REST after the roster")
        XCTAssertEqual(plan.total, plan.beats[1].start + plan.beats[1].duration, accuracy: 0.001)
    }

    /// 5. A START with nobody joining on the way: the fade, and NO snap in
    /// front of it.
    func testStartAloneFades() async throws {
        let two = try await lobby([alex, vera], game: 5006, salt: 6)
        let live = try await started(from: two, joins: [alex, vera], game: 5006)
        let plan = await k.surfacePlan(showing: two, arriving: live)
        XCTAssertEqual(plan.beats.map(\.kind), [.board])
        XCTAssertEqual(plan.beats.first?.transition, .fade)
        XCTAssertEqual(plan.beats.first?.start, 0, "there is nothing to show first")
    }

    /// THE CONTROLS, and the owner's four cases (anim_plan.h quotes him in
    /// full). Vera joining a one-player table really does hand Alex a Start
    /// button — but only when the stream stops there. A stream that goes on to
    /// the board holds every control exactly as it was for its whole length,
    /// because flashing Start into a lobby that dissolves half a second later
    /// is a flicker, and it is what he first read as "why does 'start playing'
    /// become disabled for Alex temporarily".
    func testAStreamThatEndsInTheLobbyShowsItsOwnControls() async throws {
        let one = try await lobby([alex], game: 5013, salt: 14, passing: true)
        let two = try await lobby([alex, vera], game: 5013, salt: 14, passing: false)
        let plan = await k.surfacePlan(showing: one, arriving: two)
        XCTAssertEqual(plan.beats.map(\.controls), [.live, .live],
                       "join+rules ends in the lobby, so Start and Leave appear and stay")
    }

    func testAStreamThatEndsAtTheBoardHoldsEveryControl() async throws {
        let one = try await lobby([alex], game: 5014, salt: 15)
        let two = try await lobby([alex, vera], game: 5014, salt: 15)
        let live = try await started(from: two, joins: [alex, vera], game: 5014)
        let plan = await k.surfacePlan(showing: one, arriving: live)
        XCTAssertEqual(plan.beats.map(\.controls), [.held, .held],
                       "join+start must not touch Alex's buttons on the way past")
        XCTAssertEqual(plan.beats.first?.kind, .roster,
                       "…and she is still snapped in, which is the thing he asked to see")
    }

    // MARK: what must NOT animate

    /// A BOARD takes an arrival the way it always has: the live controller folds
    /// the chain in without a teardown (ArrivalReadoptTests), and a plan here
    /// would be a second opinion about a transition that is already the board's.
    func testABoardIsHandedNoBeats() async throws {
        let two = try await lobby([alex, vera], game: 5007, salt: 7)
        let live = try await started(from: two, joins: [alex, vera], game: 5007)
        let plan = await k.surfacePlan(showing: live, arriving: live)
        XCTAssertTrue(plan.beats.isEmpty)
    }

    /// A COLD OPEN PAINTS. Owner: "If you open a lobby bubble, it should just
    /// open the state of that message with no animations. It's only if you
    /// already have the lobby open that it should snap/pause/rotate/fade."
    ///
    /// Another game's bubble is a SWITCH, not a continuation - and it is caught
    /// on the game id, because "something was showing" is true here and is not
    /// the question. Same reasoning as the 1.0(37) game-switch fix: tapping
    /// another game's bubble switches, never rebases.
    func testAnotherGamesBubbleIsASwitch() async throws {
        let mine = try await lobby([alex], game: 5008, salt: 8)
        let theirs = try await lobby([alex, vera], game: 9008, salt: 9)
        let live = try await started(from: theirs, joins: [alex, vera], game: 9008)
        let switched = await k.surfacePlan(showing: mine, arriving: theirs)
        let switchedLive = await k.surfacePlan(showing: mine, arriving: live)
        XCTAssertTrue(switched.beats.isEmpty, "another game's lobby is a switch")
        XCTAssertTrue(switchedLive.beats.isEmpty,
                      "…and so is another game's started chain, start or no start")
    }

    /// Re-opening the chain already on screen differs by nothing, so nothing
    /// animates. A no-op that played a fade would be very visible.
    func testTheSameChainTwiceAnimatesNothing() async throws {
        let two = try await lobby([alex, vera], game: 5009, salt: 10)
        let again = await k.surfacePlan(showing: two, arriving: two)
        XCTAssertTrue(again.beats.isEmpty)
    }

    /// A NET-ZERO change across two texts - the rule toggled off and back on
    /// while nobody was looking. Two bubbles arrived and the surface does
    /// nothing, because nothing differs. Intended, and worth pinning: it will
    /// read as a missed message to whoever watched both bubbles land.
    func testARuleThatCameBackAnimatesNothing() async throws {
        // Two REAL texts by two different people - Vera turned it off, Alex
        // turned it back on - so the bytes genuinely differ and this is a
        // "nothing animated even though messages arrived" case, not a duplicate.
        let on = try await lobby([alex, vera], game: 5010, salt: 11, passing: true, actor: 1)
        let backOn = try await lobby([alex, vera], game: 5010, salt: 11, passing: true, actor: 0)
        XCTAssertNotEqual(on, backOn, "two different people sealed these")
        let plan = await k.surfacePlan(showing: on, arriving: backOn)
        XCTAssertTrue(plan.beats.isEmpty)
    }

    /// A STALE SURFACE spans everything since what it is showing, not since the
    /// previous message: the extension was closed while a join and a start both
    /// landed, and the one chain that gets adopted differs from the lobby on
    /// screen by BOTH. Owner, asked directly: "OK yeah that's the correct
    /// behavior." One sequence, not a bare fade and not a jump.
    func testAStaleSurfaceReplaysTheWholeGapAsOneSequence() async throws {
        let empty = try await lobby([alex], game: 5011, salt: 12)
        let joined = try await lobby([alex, vera], game: 5011, salt: 12)
        let live = try await started(from: joined, joins: [alex, vera], game: 5011)
        let plan = await k.surfacePlan(showing: empty, arriving: live)
        XCTAssertEqual(plan.beats.map(\.kind), [.roster, .board],
                       "two messages' worth of change plays as one snap-then-fade")
    }

    /// Damaged bytes are not a sequence either - the reader degrades to no
    /// animation rather than to a guess, the same as every other wire reader.
    func testDamagedBytesPlanNothing() async throws {
        let two = try await lobby([alex, vera], game: 5012, salt: 13)
        let junk = await k.surfacePlan(showing: two, arriving: Data([1, 2, 3]))
        let none = await k.surfacePlan(showing: Data(), arriving: two)
        XCTAssertTrue(junk.beats.isEmpty)
        XCTAssertTrue(none.beats.isEmpty)
    }
}
