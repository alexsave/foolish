// The 1.0(19) phantom 8-player board, reproduced offline.
//
// Two testers in a 2-3 person chat, mid-rematch, received a LIVE bubble whose
// body was a freshly dealt EIGHT-player game (deck 4 = 52 - 8*6) while its
// `joins` roster still named only the real table - so every unjoined seat
// rendered as "Seat N" (the owner's hint). The wire genuinely said 8.
//
// The mechanism these tests pin down:
//
//   * `MessageKernel.seal` (fio_msg_encode, c/ios/ios_api.c: `e.n_players =
//     g_game.num_players`) seals WHATEVER GAME IS RESIDENT. It has no idea
//     which chain the caller thinks it is sealing.
//   * `MessageTurnController.stagedPayload` calls it from a MainActor task
//     several actor-hops after `apply` (MessageTableView.play:
//     `Task { await controller.apply(move); await stageNow() }`), with no
//     guarantee the resident game is still the controller's own chain.
//   * ANY decode of a phase-0 WAITING bubble re-points the resident game to
//     the lobby's CAPACITY deal - 8 seats for a group chat
//     (`GameSurface.createWaiting` / `createRematchLobby`). The surface makes
//     such decodes routinely: `MessageSurfaceRouter.resolve` decodes the
//     tapped bubble before even looking at its phase, and `GameSurface.load`'s
//     `.lobby` route decodes it again ("Decoding also ADOPTS").
//
// A rematch is when a WAITING bubble for a NEW game and a live board for the
// OLD one coexist at the head of one thread, so it is exactly when a tap on
// the invite bubble can land between a play's `apply` and its auto-stage
// `seal` - and the seal then emits the lobby's untouched 8-seat capacity deal
// as a phase-2 (LIVE) bubble carrying the board's joins. Worse, that bubble
// names the live chain as its parent, so Rule P's child rule makes every
// device in the chat prefer the phantom.
import XCTest
@testable import FoolishKit

@MainActor
final class MessagePhantomSealTests: XCTestCase {

    private func freshSeed(_ salt: UInt8) -> Data {
        Data((0..<32).map { UInt8(truncatingIfNeeded: $0 &* 11 &+ Int(salt)) | 1 })
    }

    private func roster(_ names: [String]) -> [MessageJoin] {
        names.enumerated().map { MessageJoin(seat: $0.offset, name: $0.element) }
    }

    private func freshStore() -> MessageGameStore {
        MessageGameStore(defaults: UserDefaults(suiteName: "fmsg.test.\(UUID().uuidString)")!)
    }

    /// The rematch lobby exactly as `GameSurface.createRematchLobby` builds it:
    /// dealt at the group capacity (8), the full old roster prefilled, the
    /// fool's carry armed, sealed WAITING.
    private func rematchLobby(seed: Data, gameId: UInt64, joins: [MessageJoin], foolSeat: Int)
        async throws -> (payload: Data, env: MessageEnvelope) {
        let k = MessageKernel.shared
        try await k.newGame(seed: seed, players: 8)
        _ = await k.armRematchCarry(joins: joins, foolSeat: foolSeat)
        let payload = try await k.seal(phase: 0, lastActorSeat: 0, gameId: gameId,
                                       parent8: Data(repeating: 0, count: 8),
                                       joins: joins, sentAt: 0x1234)
        return (payload, try await MessageEnvelope.decode(payload: payload, viewer: -1))
    }

    /// Start the rematch (the atomic, correct path), then seat a controller on
    /// the live chain and stage one legal move on it - the state every board is
    /// in right after `MessageTableView.play` ran `controller.apply`.
    private func boardWithOneStagedMove(lobby: Data, lobbyEnv: MessageEnvelope,
                                        joins: [MessageJoin], gameId: UInt64)
        async throws -> (controller: MessageTurnController, live: Data, liveEnv: MessageEnvelope) {
        let k = MessageKernel.shared
        let live = try await k.startFromLobby(
            lobbyPayload: lobby, gameId: gameId, actingSeat: 1,
            parent8: MessageTurnController.firstEight(hex: lobbyEnv.digest),
            joins: joins, sentAt: 0x1234)
        let liveEnv = try await MessageEnvelope.decode(payload: live, viewer: -1)
        XCTAssertEqual(liveEnv.phase, 2)
        XCTAssertEqual(liveEnv.nPlayers, joins.count,
                       "control: startFromLobby's own reseat is correct - the rematch branch is not the bug")

        // Whichever seat may open the game is the one that will auto-stage.
        var actor = -1; var move: Move?
        for s in 0..<joins.count {
            if let m = await k.residentLegal(seat: s).first(where: { $0.type != .wait }) {
                actor = s; move = m; break
            }
        }
        let c = MessageTurnController(parentPayload: live, parent: liveEnv,
                                      mySeat: actor, store: freshStore())
        await c.begin()
        await c.apply(move!)
        XCTAssertEqual(c.pending.count, 1, "the move must be staged for auto-stage to seal")
        return (c, live, liveEnv)
    }

    /// CONTROL: with nothing intervening between `apply` and the seal, the
    /// auto-staged bubble describes the controller's own game. Passes today -
    /// it is here so the failing twin below provably isolates the interloper
    /// decode as the trigger.
    func testAutoStageSealsTheBoardItStaged() async throws {
        let joins = roster(["Alex", "Bob", "Irina"])
        let (lobby, lobbyEnv) = try await rematchLobby(seed: freshSeed(51), gameId: 6100,
                                                       joins: joins, foolSeat: 2)
        let (c, _, liveEnv) = try await boardWithOneStagedMove(lobby: lobby, lobbyEnv: lobbyEnv,
                                                              joins: joins, gameId: 6100)
        let staged = try await c.stagedPayload(sentAt: 0x2345)
        let env = try await MessageEnvelope.peek(payload: staged)
        XCTAssertEqual(env.phase, 2)
        XCTAssertEqual(env.nPlayers, 3)
        XCTAssertGreaterThan(env.turn, liveEnv.turn, "the staged move rode the bubble")
    }

    /// THE BUG. Same board, same staged move - but between `apply` and the
    /// seal, the surface decodes the rematch invite bubble (the same
    /// `MessageEnvelope.decode` that `MessageSurfaceRouter.resolve` and
    /// `GameSurface.load`'s `.lobby` route perform when that bubble is tapped,
    /// which re-points the resident game to the lobby's 8-seat capacity deal).
    /// `stagedPayload` then seals THAT: a phase-2 bubble whose body is a fresh
    /// 8-player deal, whose joins still name only the real table - the 1.0(19)
    /// phantom, "Seat 4".."Seat 8" and all.
    func testAutoStageAfterAForeignDecodeMustStillSealTheControllersChain() async throws {
        let joins = roster(["Alex", "Bob", "Irina"])
        let (lobby, lobbyEnv) = try await rematchLobby(seed: freshSeed(52), gameId: 6101,
                                                       joins: joins, foolSeat: 2)
        let (c, live, _) = try await boardWithOneStagedMove(lobby: lobby, lobbyEnv: lobbyEnv,
                                                           joins: joins, gameId: 6101)

        // THE INTERLOPER: the user taps the rematch invite still sitting in the
        // transcript while the play task is between `apply` and `stageNow`.
        _ = try await MessageEnvelope.decode(payload: lobby, viewer: -1)

        // The auto-stage (MessageTableView.stageNow -> stagedPayload) seals.
        let staged = try await c.stagedPayload(sentAt: 0x2345)

        // Read the phantom the way a RECIPIENT would (a full decode - it is
        // accepted on every device), and gather the evidence.
        let env = try await MessageEnvelope.decode(payload: staged, viewer: -1)
        let view = await MessageKernel.shared.residentView(viewer: -1)
        let pref = try await MessageKernel.shared.preferred(live, staged)
        // Rule P: pref > 0 means the SECOND argument (the phantom) wins - it
        // names the live chain as parent8, so the child rule outranks the real
        // game on every device in the chat.
        let verdict = pref > 0 ? "THE PHANTOM WINS Rule P against the live chain"
                               : "the live chain wins Rule P"
        let evidence = "sealed: phase \(env.phase), nPlayers \(env.nPlayers), " +
            "joins \(env.joins.count) (unjoined seats render as Seat N), " +
            "deck \(view?.deckCount ?? -1), hands \(view?.players.map(\.handCount) ?? []), " +
            "carriesPenalty \(env.carriesPenalty), \(verdict)"

        XCTAssertEqual(env.nPlayers, joins.count,
                       "the auto-staged bubble must describe the controller's own chain, " +
                       "not whatever the resident game was re-pointed to - \(evidence)")
        XCTAssertFalse(env.carriesPenalty,
                       "a LIVE bubble must not still carry the lobby's penalty question - \(evidence)")
    }
}
