// OpeningDealShieldTests - nobody defends until the trump has turned.
//
// The owner, on the website: "while it does the opening deal, it shows ME as
// having the shield, no matter who ends up actually having it. Which doesn't
// make any sense because who goes first can't be determined until the flipped
// card is revealed." And the rule for the fix: "just don't show the shield until
// deal is done, otherwise it can flicker".
//
// The board draws its role marks from `shownRoles` (MessageTableView+Roles.swift):
// the ledger while a sequence runs, the live view at rest. A sequence seeds the
// ledger ONCE, from the board before the stream (`openReplayPriorState`) or, when
// there is none, from the stream's first event (MessageTableView+Sequence.swift
// `runEventStream`), and freezes it until the role beat hands the marks over.
// So during any stream that carries the opening deal every seat wears the SEED's
// marks, and a seed that names a defender puts a shield up for the whole deal.
//
// WHAT A CHAT OPENS - a lobby, the joins, Start, and every seat opening the
// started bubble and the first move's bubble. GREEN TODAY, and why is the point:
// neither replays the deal. The first move's bubble replays from
// `atomsBefore + 1` and the deal is step 0; the started bubble would take the
// deal-only branch of fio_replay_last_events_packed (c/ios/ios_api_replay.c, "a
// chain that IS only the deal"), but `residentReplayCode()` is nil for a chain
// with no action yet, so no stream is ever built. The board then draws the live,
// dealt view, whose defender is real. If a chat ever does animate the deal, the
// stream's boards are the kernel's START_MAGIC / DEAL / FLIPPED ones, which carry
// the lobby's `defender = 0` today (c/src/game.c), and `holdDealStream` turns
// this red.
import XCTest
@testable import FoolishKit

@MainActor
final class OpeningDealShieldTests: XCTestCase {

    private func freshSeed(_ salt: UInt8) -> Data {
        Data((0..<32).map { UInt8(truncatingIfNeeded: $0 &* 29 &+ Int(salt)) | 1 })
    }

    private let names = ["Alex", "Sveta", "Boris", "Dima", "Eva", "Fyodor", "Galya", "Igor"]
    private let tables = [2, 3, 6, 8]

    /// A started game's LIVE bubble: a lobby of `players`, every one joined, Start.
    private func startedGame(players: Int, salt: UInt8, gameId: UInt64) async throws -> Data {
        let k = MessageKernel.shared
        try await k.newGame(seed: freshSeed(salt), players: players)
        var joins = [MessageJoin(seat: 0, name: names[0])]
        var payload = try await k.seal(phase: 0, lastActorSeat: 0, gameId: gameId,
                                       parent8: Data(repeating: 0, count: 8), joins: joins)
        for seat in 1..<players {
            let env = try await MessageEnvelope.decode(payload: payload, viewer: -1)
            joins = (env.joins + [MessageJoin(seat: seat, name: names[seat])]).sorted { $0.seat < $1.seat }
            payload = try await k.seal(phase: 0, lastActorSeat: seat, gameId: gameId,
                                       parent8: MessageTurnController.firstEight(hex: env.digest),
                                       joins: joins)
        }
        let lobby = try await MessageEnvelope.decode(payload: payload, viewer: -1)
        return try await k.startFromLobby(lobbyPayload: payload, gameId: gameId, actingSeat: 0,
                                          parent8: MessageTurnController.firstEight(hex: lobby.digest),
                                          joins: lobby.joins, sentAt: 0)
    }

    /// The marks a board wears through `events`: the ledger seed `runEventStream`
    /// writes, or nil when there is no stream (the board then draws the live view).
    private func seededMarks(prior: GameView?, events: [GameEvent]) -> MessageTableView.RoleState? {
        guard !events.isEmpty else { return nil }
        var ledger = ShownLedger.Fields()
        ledger.seedMarks(from: prior ?? events.first?.state, outs: true)
        return ledger.roles
    }

    /// Holds one stream that carries the opening deal to the rule: the seed names
    /// nobody, no board before DEFENDER_MOVE names anybody, and from DEFENDER_MOVE
    /// on every board names `defender`.
    private func holdDealStream(_ events: [GameEvent], prior: GameView?, players: Int,
                                defender: Int, _ at: String) {
        guard let named = events.firstIndex(where: { $0.kind == .defenderMove }) else {
            XCTFail("\(at): a stream with the deal in it has no DEFENDER_MOVE (types \(events.map(\.type)))")
            return
        }
        let seats = 0..<players
        if let seeded = seededMarks(prior: prior, events: events) {
            XCTAssertFalse(seats.contains(seeded.defender),
                           "\(at): the deal is drawn with seat \(seeded.defender)'s shield up before anybody "
                           + "defends (seeded from \(prior == nil ? "the first event" : "the prior board"))")
            XCTAssertFalse(seats.contains(seeded.firstAttacker),
                           "\(at): the deal is drawn with seat \(seeded.firstAttacker) leading before anybody leads")
        }
        for (i, e) in events[..<named].enumerated() {
            guard let s = e.state else { continue }
            XCTAssertFalse(seats.contains(s.defender),
                           "\(at): event \(i) (type \(e.type)) is before DEFENDER_MOVE and names seat \(s.defender) "
                           + "as defender")
        }
        for (i, e) in events[named...].enumerated() {
            guard let s = e.state else { continue }
            XCTAssertEqual(s.defender, defender, "\(at): event \(named + i) names the real defender")
        }
    }

    /// WHAT A CHAT OPENS. Green today, and why is the point: no bubble a chat
    /// opens replays step 0, so the marks come off the live view.
    func testOpeningAStartedChatNeverDrawsAShieldBeforeTheDefenderIsNamed() async throws {
        var opens = 0
        for players in tables {
            for salt in UInt8(1)...UInt8(2) {
                let gid = UInt64(7000 + players * 10 + Int(salt))
                let live = try await startedGame(players: players, salt: salt &* 11 &+ UInt8(players), gameId: gid)
                let liveEnv = try await MessageEnvelope.decode(payload: live, viewer: -1)
                XCTAssertEqual(liveEnv.phase, 2, "fixture (\(players)p): Start seals a live game")

                // The started bubble, and the first move's bubble on top of it.
                var bubbles: [(name: String, payload: Data, env: MessageEnvelope)] = [("start", live, liveEnv)]
                for s in 0..<players where bubbles.count == 1 {
                    let opener = MessageTurnController(parentPayload: live, parent: liveEnv, mySeat: s)
                    await opener.begin()
                    if let lead = opener.legal.first(where: { $0.type != .wait }) {
                        await opener.apply(lead)
                        let p = try await opener.stagedPayload()
                        bubbles.append(("first move", p, try await MessageEnvelope.decode(payload: p, viewer: -1)))
                    }
                }
                XCTAssertEqual(bubbles.count, 2, "fixture (\(players)p): somebody leads")

                for (name, payload, env) in bubbles {
                    for viewer in 0..<players {
                        let c = MessageTurnController(parentPayload: payload, parent: env, mySeat: viewer)
                        await c.begin()
                        let at = "\(players)p salt \(salt), \(name) bubble, viewer \(viewer)"
                        let view = try XCTUnwrap(c.view, "\(at): a live board")
                        let events = c.openReplayEvents
                        opens += 1
                        if events.contains(where: { $0.kind == .deal }) {
                            holdDealStream(events, prior: c.openReplayPriorState, players: players,
                                           defender: view.defender, at)
                        } else {
                            // At rest the board draws `RoleState(view)`: the dealt board's own seats.
                            let marks = seededMarks(prior: c.openReplayPriorState, events: events)
                                ?? MessageTableView.RoleState(view)
                            XCTAssertTrue((0..<players).contains(marks.defender),
                                          "\(at): the opened board names its defender")
                            XCTAssertEqual(marks.defender, view.defender, "\(at): and it is the real one")
                        }
                    }
                }
            }
        }
        XCTAssertGreaterThan(opens, 0)
    }
}
