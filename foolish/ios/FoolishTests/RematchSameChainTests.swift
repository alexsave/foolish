// A finished game is rematched as THE SAME GAME, whoever taps New game.
//
// The owner's report: "Somehow a finished game was able to be forked into 3
// games. No this shouldn't be possible. It should not start a new chain I
// think, it should collapse the same game (yes, wiping out the history)." And
// the design, after a format change was turned down: "just when someone hits a
// new game, don't start a fresh chain! To randomize, just do some rng based on
// timestamp of new game start. Then don't allow whoever creates a game to
// start it, and we're all set. The seed is locked in."
//
// Reproduced here through the one entry the surface calls on a finished
// board's New game (`RematchLobby.build`), as three different seats of the
// SAME finished table, at 2, 3 and 4 players. Every tap must continue the same
// game in the same chain - its id, its parent - with a deal of its own moment;
// the arriving lobby must win over the finished board (the decision
// `GameSurface.maybeAdoptIncoming` asks the kernel); the tapper may not Start
// it and everyone else may.
import XCTest
@testable import FoolishKit

@MainActor
final class RematchSameChainTests: XCTestCase {

    private let names = ["Alex", "Mira", "Jonas", "Priya"]
    private let k = MessageKernel.shared

    private func seed(_ salt: Int) -> Data {
        Data((0..<32).map { UInt8(truncatingIfNeeded: $0 &* 29 &+ salt) | 1 })
    }

    /// A FINISHED n-player chain with every seat named, played the way a table
    /// plays: a full lobby, Start, then the lowest seat with a move moves until
    /// somebody is the fool. Seeds are walked until the game ends with a fool,
    /// so the fixture is a real rematchable result whatever the deal.
    private func finishedGame(players n: Int, passing: Bool, gameId: UInt64)
        async throws -> (payload: Data, env: MessageEnvelope) {
        let joins = (0..<n).map { MessageJoin(seat: $0, name: names[$0]) }
        for salt in 1...40 {
            try await k.newGame(seed: seed(salt * 7 + n), players: n)
            await k.setPassing(passing)
            _ = await k.armRematchCarry(joins: [], foolSeat: 0)   // an ordinary lobby
            let lobby = try await k.seal(phase: 0, lastActorSeat: n - 1, gameId: gameId,
                                         parent8: Data(repeating: 0, count: 8),
                                         joins: joins, sentAt: 0x1111)
            let lobbyEnv = try await MessageEnvelope.decode(payload: lobby, viewer: -1)
            let live = try await k.startFromLobby(
                lobbyPayload: lobby, gameId: gameId, actingSeat: 0,
                parent8: MessageTurnController.firstEight(hex: lobbyEnv.digest),
                joins: joins, sentAt: 0x2222)
            let liveEnv = try await MessageEnvelope.decode(payload: live, viewer: -1)
            var last = 0
            for _ in 0..<3000 {
                if let v = await k.residentView(viewer: -1), v.isOver { break }
                var moved = false
                for s in 0..<n {
                    if let m = await k.residentLegal(seat: s).first(where: { $0.type != .wait }) {
                        try await k.apply(seat: s, move: m)
                        last = s; moved = true
                        break
                    }
                }
                if !moved { break }
            }
            guard let v = await k.residentView(viewer: -1), v.isOver, v.gameOver >= 0 else { continue }
            let done = try await k.seal(phase: 3, lastActorSeat: last, gameId: gameId,
                                        parent8: MessageTurnController.firstEight(hex: liveEnv.digest),
                                        joins: joins, sentAt: 0x3333)
            return (done, try await MessageEnvelope.decode(payload: done, viewer: -1))
        }
        throw XCTSkip("no seed in range finished a \(n)-player game with a fool")
    }

    /// What `seat`'s device seals when its human taps New game on that board
    /// at `tappedAtMs`. The board is built from the finished chain exactly as a
    /// device opening the bubble builds it.
    private func tap(_ finished: Data, seat: Int, players n: Int, tappedAtMs: UInt64,
                     sameChain: Bool = true) async throws -> Data? {
        let env = try await MessageEnvelope.decode(payload: finished, viewer: -1)
        let board = MessageTurnController(parentPayload: finished, parent: env, mySeat: seat)
        await board.begin()
        return try await RematchLobby.build(
            finished: board.basePayload, view: board.view, names: board.names,
            mySeat: seat, myName: names[seat], passing: board.passingAllowed,
            capacity: 8, tappedAtMs: tappedAtMs, sameChain: sameChain)?.payload
    }

    private let t0: UInt64 = 1_790_000_000_000   // 2026-09, unix ms

    private func checkThreeTaps(players n: Int, passing: Bool, sameChain: Bool = true) async throws {
        let gameId = UInt64(0x5EED_0000 + n * 2 + (passing ? 1 : 0))
        let finished = try await finishedGame(players: n, passing: passing, gameId: gameId)
        let tappers = Array(0..<min(3, n))
        let label = "\(n)p \(passing ? "passing" : "podkidnoy")"

        // The three taps race: each at its own moment, as real devices do.
        var lobbies: [Data] = []
        var envs: [MessageEnvelope] = []
        for s in tappers {
            let p = try await tap(finished.payload, seat: s, players: n,
                                  tappedAtMs: t0 + UInt64(s) * 1_337, sameChain: sameChain)
            let lobby = try XCTUnwrap(p, "\(label) seat \(s): a finished board's New game built no rematch")
            lobbies.append(lobby)
            envs.append(try await MessageEnvelope.peek(payload: lobby))
        }

        // ONE GAME. The old fresh chain minted a new id on every tap.
        XCTAssertEqual(Set(envs.map(\.gameId)), [finished.env.gameId],
                       "\(label): the taps made \(Set(envs.map(\.gameId)).count) games, not the finished one")
        // THE SAME CHAIN: every lobby names the finished chain as its parent.
        let finished8 = String(finished.env.digest.prefix(16))
        for (s, e) in zip(tappers, envs) {
            XCTAssertEqual(e.parent8, finished8,
                           "\(label) seat \(s): the rematch lobby's parent is \(e.parent8), not the finished chain")
            XCTAssertEqual(e.lastActorSeat, s, "\(label) seat \(s): the lobby is not its tapper's")
        }
        // A DEAL OF ITS OWN MOMENT: three taps, three deals.
        XCTAssertEqual(Set(envs.map(\.dealTag)).count, tappers.count,
                       "\(label): taps at different moments share a deal")
        // AN ARRIVING REMATCH WINS over the finished board it grew out of, both
        // ways round - the question maybeAdoptIncoming asks. > 0 means the
        // second argument wins.
        for (s, lobby) in zip(tappers, lobbies) {
            let pref = try await k.preferred(finished.payload, lobby)
            XCTAssertGreaterThan(pref, 0, "\(label) seat \(s): Rule P keeps the FINISHED board over its rematch")
            let back = try await k.preferred(lobby, finished.payload)
            XCTAssertLessThan(back, 0, "\(label) seat \(s): (reversed) Rule P keeps the FINISHED board")
        }
        // ONE MOMENT IS ONE DEAL, whoever taps.
        let same0 = try await tap(finished.payload, seat: 0, players: n, tappedAtMs: t0 + 99,
                                  sameChain: sameChain)
        let same1 = try await tap(finished.payload, seat: n - 1, players: n, tappedAtMs: t0 + 99,
                                  sameChain: sameChain)
        let p0 = try XCTUnwrap(same0), p1 = try XCTUnwrap(same1)
        let e0 = try await MessageEnvelope.peek(payload: p0)
        let e1 = try await MessageEnvelope.peek(payload: p1)
        XCTAssertEqual(e0.dealTag, e1.dealTag, "\(label): two seats tapping at one moment got two deals")
        // ITS BUBBLE SAYS REMATCH: a full table invites nobody to join, and
        // "<name> joined" would announce the tapper as if they had walked in.
        for (s, lobby) in zip(tappers, lobbies) {
            let (_, _, summary) = await MessageSummary.forStagedBubble(payload: lobby, leftName: nil)
            XCTAssertEqual(summary, FStrings.t("ios.msg.rematch"),
                           "\(label) seat \(s): the rematch bubble reads \"\(summary)\"")
        }
        // THE TABLE CARRIES OVER: every seat, the rules, and the fool's penalty.
        for (s, e) in zip(tappers, envs) {
            XCTAssertEqual(e.joins.sorted { $0.seat < $1.seat },
                           finished.env.joins.sorted { $0.seat < $1.seat },
                           "\(label) seat \(s): the rematch is not seated as the finished game was")
            XCTAssertEqual(e.passingAllowed, passing, "\(label) seat \(s): the rules did not carry over")
            XCTAssertTrue(e.carriesPenalty, "\(label) seat \(s): the fool's penalty did not carry over")
        }
        // THE CREATOR CANNOT START IT; everyone else at the table can.
        for (creator, e) in zip(tappers, envs) {
            for seat in 0..<n {
                let offered = LobbyControls.offered(
                    mySeat: seat, joined: e.joins.count, capacity: e.nPlayers,
                    iSentTheInvite: e.lastActorSeat == seat,
                    iChangedTheRules: LobbyControls.iAmTheChanger(env: e, mySeat: seat, baseline: nil))
                if seat == creator {
                    XCTAssertEqual(offered, .waiting,
                                   "\(label): the creator (seat \(seat)) is offered \(offered), not Waiting")
                    XCTAssertTrue(LobbyControls.canExit(mySeat: seat, joined: e.joins.count),
                                  "\(label): the creator has neither Start nor Leave")
                } else {
                    XCTAssertEqual(offered, .start,
                                   "\(label): seat \(seat), not the creator, is offered \(offered), not Start")
                }
            }
        }
    }

    /// ANOTHER SEAT'S START DEALS the rematch, with the fool under the sword -
    /// and the dealt game outranks the finished one on a device that never saw
    /// the lobby, while the finished board will not fold that chain into itself.
    private func checkAnotherSeatStarts(players n: Int) async throws {
        let finished = try await finishedGame(players: n, passing: true, gameId: 0x5EED_4400 + UInt64(n))
        let creator = n - 1
        let tapped = try await tap(finished.payload, seat: creator, players: n, tappedAtMs: t0 + 7)
        let lobby = try XCTUnwrap(tapped)
        let lobbyEnv = try await MessageEnvelope.decode(payload: lobby, viewer: -1)
        let foolSeat = await k.penaltyFoolSeat(joins: lobbyEnv.joins,
                                               carryKey: lobbyEnv.carryKey!,
                                               carryFool: lobbyEnv.carryFool!)
        let fool = try XCTUnwrap(foolSeat, "\(n)p: the rematch lobby punishes nobody")
        let starter = 0
        let offered = LobbyControls.offered(
            mySeat: starter, joined: lobbyEnv.joins.count, capacity: lobbyEnv.nPlayers,
            iSentTheInvite: lobbyEnv.lastActorSeat == starter,
            iChangedTheRules: LobbyControls.iAmTheChanger(env: lobbyEnv, mySeat: starter, baseline: nil))
        XCTAssertEqual(offered, .start, "\(n)p: seat \(starter) is not offered Start on another's rematch")
        let live = try await k.startFromLobby(
            lobbyPayload: lobby, gameId: UInt64(lobbyEnv.gameId)!, actingSeat: starter,
            parent8: MessageTurnController.firstEight(hex: lobbyEnv.digest),
            joins: lobbyEnv.joins, sentAt: 0x4200)
        let liveEnv = try await MessageEnvelope.decode(payload: live, viewer: -1)
        XCTAssertEqual(liveEnv.phase, 2, "\(n)p: Start did not deal")
        XCTAssertEqual(liveEnv.gameId, finished.env.gameId, "\(n)p: Start left the game")
        XCTAssertEqual(liveEnv.dealTag, lobbyEnv.dealTag, "\(n)p: Start dealt another deal than the lobby's")
        XCTAssertNotEqual(lobbyEnv.dealTag, finished.env.dealTag, "\(n)p: the rematch re-deals the finished deal")
        let view = await k.residentView(viewer: -1)
        XCTAssertEqual(view?.defender, fool, "\(n)p: the fool is not the rematch's first defender")
        let pref = try await k.preferred(finished.payload, live)
        XCTAssertGreaterThan(pref, 0, "\(n)p: the finished game beat its rematch in play")
        let back = try await k.preferred(live, finished.payload)
        XCTAssertLessThan(back, 0, "\(n)p: (reversed) the finished game beat its rematch in play")

        // The finished board's controller must not adopt the rematch as a
        // continuation of itself - another deal, under the same id.
        let board = MessageTurnController(parentPayload: finished.payload,
                                          parent: finished.env, mySeat: 0)
        await board.begin()
        XCTAssertTrue(board.canAdopt(seat: 0, gameId: finished.env.gameId,
                                     dealTag: finished.env.dealTag),
                      "sanity: the board takes its own deal's chains")
        XCTAssertFalse(board.canAdopt(seat: 0, gameId: liveEnv.gameId, dealTag: liveEnv.dealTag),
                       "\(n)p: the finished board would fold the rematch's chain into itself")
        XCTAssertFalse(finished.env.isSameDeal(liveEnv))
    }

    /// A REMATCH LOBBY IS AN ORDINARY LOBBY to any reader: format 6, routed to
    /// the lobby surface, never the damaged screen an unknown format would get.
    func testTheRematchLobbyReadsAsAnOrdinaryLobby() async throws {
        let finished = try await finishedGame(players: 3, passing: true, gameId: 0x5EED_6666)
        let tapped = try await tap(finished.payload, seat: 1, players: 3, tappedAtMs: t0)
        let lobby = try XCTUnwrap(tapped)
        XCTAssertEqual(lobby[1], 6, "the rematch lobby is format \(lobby[1]), not the format 6 every build reads")
        let screen = await MessageSurfaceRouter.resolve(payload: lobby, startNewGame: false, chatKey: "t")
        XCTAssertEqual(screen, .lobby(payload: lobby), "the rematch lobby routes to \(screen), not the lobby")
    }

    /// THE FLAG OFF is the fresh chain it always was: a new game id, rotated
    /// to the tapper, which is what `rematch.samechain=0` puts back.
    func testWithTheFlagOffARematchIsAFreshChain() async throws {
        let finished = try await finishedGame(players: 3, passing: true, gameId: 0x5EED_3333)
        let env = try await MessageEnvelope.decode(payload: finished.payload, viewer: -1)
        let board = MessageTurnController(parentPayload: finished.payload, parent: env, mySeat: 1)
        await board.begin()
        let built = try await RematchLobby.build(
            finished: board.basePayload, view: board.view, names: board.names,
            mySeat: 1, myName: names[1], passing: true, capacity: 8, tappedAtMs: t0,
            sameChain: false)
        let lobby = try XCTUnwrap(built)
        let lobbyEnv = try await MessageEnvelope.peek(payload: lobby.payload)
        XCTAssertNotEqual(lobbyEnv.gameId, env.gameId, "the flag-off rematch kept the game id")
        XCTAssertEqual(lobbyEnv.parent8, "0000000000000000")
        XCTAssertEqual(lobby.mySeat, 0, "the flag-off rematch seats its tapper at 0")
        XCTAssertEqual(lobbyEnv.nPlayers, 8, "the flag-off rematch is open at the chat's capacity")
    }

    func testThreeTapsAreThreeLobbiesOfTheSameGame2p() async throws {
        try await checkThreeTaps(players: 2, passing: true)
    }

    func testThreeTapsAreThreeLobbiesOfTheSameGame3p() async throws {
        try await checkThreeTaps(players: 3, passing: true)
    }

    func testThreeTapsAreThreeLobbiesOfTheSameGame4p() async throws {
        try await checkThreeTaps(players: 4, passing: true)
    }

    func testAPodkidnoyTableRematchesAsPodkidnoy4p() async throws {
        try await checkThreeTaps(players: 4, passing: false)
    }

    func testAnotherSeatsStartDealsTheRematch2p() async throws {
        try await checkAnotherSeatStarts(players: 2)
    }

    func testAnotherSeatsStartDealsTheRematch3p() async throws {
        try await checkAnotherSeatStarts(players: 3)
    }

    func testAnotherSeatsStartDealsTheRematch4p() async throws {
        try await checkAnotherSeatStarts(players: 4)
    }

    /// A board that is NOT finished has no rematch: its New game is an ordinary
    /// one (a fresh chain from the setup screen), at any size.
    func testAMidGameNewGameIsNotARematch() async throws {
        for n in [2, 3, 4] {
            let joins = (0..<n).map { MessageJoin(seat: $0, name: names[$0]) }
            try await k.newGame(seed: seed(90 + n), players: n)
            await k.setPassing(true)
            _ = await k.armRematchCarry(joins: [], foolSeat: 0)
            let lobby = try await k.seal(phase: 0, lastActorSeat: 0, gameId: 0xABC0 + UInt64(n),
                                         parent8: Data(repeating: 0, count: 8), joins: joins)
            let lobbyEnv = try await MessageEnvelope.decode(payload: lobby, viewer: -1)
            let live = try await k.startFromLobby(
                lobbyPayload: lobby, gameId: 0xABC0 + UInt64(n), actingSeat: 0,
                parent8: MessageTurnController.firstEight(hex: lobbyEnv.digest), joins: joins)
            let p = try await tap(live, seat: 0, players: n, tappedAtMs: t0)
            XCTAssertNil(p, "\(n)p: a mid-game New game was treated as a rematch")
        }
    }
}
