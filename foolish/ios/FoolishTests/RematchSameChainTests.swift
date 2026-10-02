// A finished game is rematched ONCE, whoever taps New game.
//
// The owner's report: "Somehow a finished game was able to be forked into 3
// games. No this shouldn't be possible. It should not start a new chain I
// think, it should collapse the same game (yes, wiping out the history)."
//
// Reproduced here through the one entry the surface calls on a finished
// board's New game (`RematchLobby.build`), as three different seats of the
// SAME finished table, at 2, 3 and 4 players. Every tap must build the same
// lobby - the same game, the same next deal, descended from the finished chain
// - and an arriving rematch lobby must be adopted over the finished board,
// which is the decision `GameSurface.maybeAdoptIncoming` asks the kernel.
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

    /// What `seat`'s device seals when its human taps New game on that board.
    /// The board is built from the finished chain exactly as a device opening
    /// the bubble builds it.
    private func tap(_ finished: Data, seat: Int, players n: Int, sentAt: Int)
        async throws -> Data? {
        let env = try await MessageEnvelope.decode(payload: finished, viewer: -1)
        let board = MessageTurnController(parentPayload: finished, parent: env, mySeat: seat)
        await board.begin()
        return try await RematchLobby.build(
            finished: board.basePayload, view: board.view, names: board.names,
            mySeat: seat, myName: names[seat], passing: board.passingAllowed,
            capacity: 8, sentAt: sentAt)?.payload
    }

    private func checkOneLobby(players n: Int, passing: Bool) async throws {
        let gameId = UInt64(0x5EED_0000 + n * 2 + (passing ? 1 : 0))
        let finished = try await finishedGame(players: n, passing: passing, gameId: gameId)
        let tappers = Array(0..<min(3, n))

        // The three taps race: each seals at its own second, as real devices do.
        var lobbies: [Data] = []
        var envs: [MessageEnvelope] = []
        for s in tappers {
            let p = try await tap(finished.payload, seat: s, players: n, sentAt: 0x4000 + s)
            let lobby = try XCTUnwrap(p, "\(n)p seat \(s): a finished board's New game built no rematch")
            lobbies.append(lobby)
            envs.append(try await MessageEnvelope.peek(payload: lobby))
        }
        let label = "\(n)p \(passing ? "passing" : "podkidnoy")"

        // ONE GAME. Today every tap mints its own.
        XCTAssertEqual(Set(envs.map(\.gameId)), [finished.env.gameId],
                       "\(label): the taps made \(Set(envs.map(\.gameId)).count) games, not the finished one")
        // DESCENDED FROM THE FINISHED CHAIN, so Rule 4 and the stale gate see a child.
        let finished8 = String(finished.env.digest.prefix(16))
        for (s, e) in zip(tappers, envs) {
            XCTAssertEqual(e.parent8, finished8,
                           "\(label) seat \(s): the rematch lobby's parent is \(e.parent8), not the finished chain")
        }
        // AN ARRIVING REMATCH WINS over the finished board it grew out of - the
        // question maybeAdoptIncoming asks. > 0 means the second argument wins.
        for (s, lobby) in zip(tappers, lobbies) {
            let pref = try await k.preferred(finished.payload, lobby)
            XCTAssertGreaterThan(pref, 0,
                                 "\(label) seat \(s): Rule P keeps the FINISHED board over its rematch")
        }
        // IDENTICAL BYTES apart from the send clock: the same taps sealed at the
        // same second are the same bubble.
        var same: [Data] = []
        for s in tappers {
            if let p = try await tap(finished.payload, seat: s, players: n, sentAt: 0x5000) {
                same.append(p)
            }
        }
        XCTAssertEqual(Set(same).count, 1,
                       "\(label): \(Set(same).count) different lobbies from one finished table")
        // NO LOSER TO TELL (Rule N, docs/IMESSAGE_SUPERSEDED_MOVES.md): the
        // lobbies race, Rule P keeps one, and the device whose tap lost holds
        // a lobby of exactly the same table - so the stale-branch gate, the
        // one place a lost move is reported today, must not call it behind.
        for a in envs {
            for b in envs where a.digest != b.digest {
                XCTAssertFalse(StaleBranchGate.isAhead(.init(a), of: .init(b)),
                               "\(label): one identical rematch lobby reads as ahead of another")
            }
        }
        // ITS BUBBLE SAYS REMATCH: a full table invites nobody to join, and
        // "<name> joined" would name the finished game's last mover.
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
    }

    /// START ON THE REMATCH deals the next generation of the same game, with
    /// the fool under the sword - and a board of the finished game will not
    /// fold that chain into itself.
    func testStartingTheRematchDealsTheNextGeneration4p() async throws {
        let n = 4
        let finished = try await finishedGame(players: n, passing: true, gameId: 0x5EED_4444)
        let tapped = try await tap(finished.payload, seat: 2, players: n, sentAt: 0x4100)
        let lobby = try XCTUnwrap(tapped)
        let lobbyEnv = try await MessageEnvelope.decode(payload: lobby, viewer: -1)
        let foolSeat = await k.penaltyFoolSeat(joins: lobbyEnv.joins,
                                               carryKey: lobbyEnv.carryKey!,
                                               carryFool: lobbyEnv.carryFool!)
        let fool = try XCTUnwrap(foolSeat, "the rematch lobby punishes nobody")
        let live = try await k.startFromLobby(
            lobbyPayload: lobby, gameId: UInt64(lobbyEnv.gameId)!, actingSeat: 1,
            parent8: MessageTurnController.firstEight(hex: lobbyEnv.digest),
            joins: lobbyEnv.joins, sentAt: 0x4200)
        let liveEnv = try await MessageEnvelope.decode(payload: live, viewer: -1)
        XCTAssertEqual(liveEnv.phase, 2)
        XCTAssertEqual(liveEnv.gameId, finished.env.gameId, "Start left the game")
        XCTAssertEqual(liveEnv.generation, 1, "Start dropped the rematch generation")
        XCTAssertEqual(lobbyEnv.generation, 1)
        XCTAssertEqual(finished.env.generation, 0)
        let view = await k.residentView(viewer: -1)
        XCTAssertEqual(view?.defender, fool, "the fool is not the rematch's first defender")
        let pref = try await k.preferred(finished.payload, live)
        XCTAssertGreaterThan(pref, 0, "the finished game beat its rematch in play")

        // The finished board's controller must not adopt the rematch as a
        // continuation of itself - another deal, under the same id.
        let board = MessageTurnController(parentPayload: finished.payload,
                                          parent: finished.env, mySeat: 2)
        await board.begin()
        XCTAssertTrue(board.canAdopt(seat: 2, gameId: finished.env.gameId,
                                     generation: finished.env.generation),
                      "sanity: the board takes its own deal's chains")
        XCTAssertFalse(board.canAdopt(seat: 2, gameId: liveEnv.gameId, generation: liveEnv.generation),
                       "the finished board would fold the rematch's chain into itself")
        XCTAssertFalse(finished.env.isSameDeal(liveEnv))

        // The stale gate: an old bubble of the finished game is BEHIND the rematch.
        XCTAssertTrue(StaleBranchGate.isAhead(.init(lobbyEnv), of: .init(finished.env)),
                      "the rematch lobby is not ahead of the game it replaced")
        XCTAssertFalse(StaleBranchGate.isAhead(.init(finished.env), of: .init(lobbyEnv)))
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
            mySeat: 1, myName: names[1], passing: true, capacity: 8, sentAt: 0x4300,
            sameChain: false)
        let lobby = try XCTUnwrap(built)
        let lobbyEnv = try await MessageEnvelope.peek(payload: lobby.payload)
        XCTAssertNotEqual(lobbyEnv.gameId, env.gameId, "the flag-off rematch kept the game id")
        XCTAssertEqual(lobbyEnv.generation, 0)
        XCTAssertEqual(lobbyEnv.parent8, "0000000000000000")
        XCTAssertEqual(lobby.mySeat, 0, "the flag-off rematch seats its tapper at 0")
        XCTAssertEqual(lobbyEnv.nPlayers, 8, "the flag-off rematch is open at the chat's capacity")
    }

    func testEveryTapOnAFinishedTableBuildsTheSameRematch2p() async throws {
        try await checkOneLobby(players: 2, passing: true)
    }

    func testEveryTapOnAFinishedTableBuildsTheSameRematch3p() async throws {
        try await checkOneLobby(players: 3, passing: true)
    }

    func testEveryTapOnAFinishedTableBuildsTheSameRematch4p() async throws {
        try await checkOneLobby(players: 4, passing: true)
    }

    func testAPodkidnoyTableRematchesAsPodkidnoy4p() async throws {
        try await checkOneLobby(players: 4, passing: false)
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
            let p = try await tap(live, seat: 0, players: n, sentAt: 0x6000)
            XCTAssertNil(p, "\(n)p: a mid-game New game was treated as a rematch")
        }
    }
}
