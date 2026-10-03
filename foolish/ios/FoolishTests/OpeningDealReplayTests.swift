// OpeningDealReplayTests - a started chat plays the opening deal.
//
// The owner, on iMessage: "Did not see card deal". Start seals a LIVE bubble at
// turn 0 - dealt, nobody has moved - and a board animates by replaying the
// chain's own replay code. The kernel refused to encode a game before its first
// attack (replay_encode_v6_from_game: "no attack logged -> nothing to encode"),
// so `residentReplayCode()` was nil for every started bubble, the open stream
// was empty, and the deal - step 0 of every v6 replay, paced card by card by
// the kernel's plan - never played on any device, from either entry point:
//
//   1. OPENING the started bubble (a cold open: a fresh controller, `begin`);
//   2. Start ARRIVING on a lobby that is on screen (GameSurface.playArrival:
//      the surface plan fades the lobby out, then `adopt(winner:env:)` seats a
//      fresh controller on the started chain - the same `begin`), and Start
//      TAPPED here (GameSurface.startGame: the same fade, the same controller).
//
// Both end in `MessageTurnController.begin` -> `MessageKernel.openChain`, so
// that is the seam pinned here, for every seat and a spectator, at 2 and 4
// players (most testing so far has been 2p): the stream holds players x 6 deal
// events, round-robin from seat 0, one card each, on the kernel's clock.
// The no-shield rule during that deal is OpeningDealShieldTests'.
//
// `OpenDeal.shows` (`open.deal` in dev.flags) guards the behaviour: off, a
// started bubble opens quiet exactly as it did before.
import XCTest
@testable import FoolishKit

/// A started chat, built the way the extension builds it: a lobby, every seat
/// joined, Start. Hands back the last lobby bubble and the started one.
enum StartedChat {
    static let names = ["Alex", "Sveta", "Boris", "Dima", "Eva", "Fyodor", "Galya", "Igor"]

    static func seed(_ salt: UInt8) -> Data {
        Data((0..<32).map { UInt8(truncatingIfNeeded: $0 &* 29 &+ Int(salt)) | 1 })
    }

    static func build(players: Int, salt: UInt8, gameId: UInt64) async throws -> (lobby: Data, started: Data) {
        let k = MessageKernel.shared
        try await k.newGame(seed: seed(salt), players: players)
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
        let started = try await k.startFromLobby(lobbyPayload: payload, gameId: gameId, actingSeat: 0,
                                                 parent8: MessageTurnController.firstEight(hex: lobby.digest),
                                                 joins: lobby.joins, sentAt: 0)
        return (payload, started)
    }
}

@MainActor
final class OpeningDealReplayTests: XCTestCase {

    private let tables = [2, 4]

    /// The stream `begin` publishes for `seat` on the started chain.
    private func opened(_ started: Data, seat: Int) async throws -> MessageTurnController {
        let env = try await MessageEnvelope.decode(payload: started, viewer: -1)
        XCTAssertEqual(env.phase, 2, "fixture: Start seals a LIVE chain")
        XCTAssertEqual(env.turn, 0, "fixture: and nobody has moved")
        let c = MessageTurnController(parentPayload: started, parent: env, mySeat: seat)
        await c.begin()
        return c
    }

    /// The deal, as the stream carries it: players x CARDS_PER_PLAYER events,
    /// one card each, round-robin from seat 0.
    private func holdDeal(_ events: [GameEvent], players: Int, _ at: String) {
        let deal = events.filter { $0.kind == .deal }
        XCTAssertEqual(deal.count, players * 6,
                       "\(at): the opened board deals \(deal.count) cards, want \(players * 6) "
                       + "(stream of \(events.count) events)")
        for (k, e) in deal.enumerated() {
            XCTAssertEqual(e.seat, k % players, "\(at): dealt card \(k) goes to seat \(e.seat), want round-robin")
            XCTAssertEqual(e.cards.count, 1, "\(at): dealt card \(k) is one card")
        }
    }

    /// ENTRY POINT 1: the started bubble, opened cold, by every seat and by a
    /// spectator.
    func testOpeningAStartedBubbleDealsForEverySeat() async throws {
        XCTAssertTrue(OpenDeal.showsByDefault, "the deal on open is what ships")
        XCTAssertEqual(OpenDeal.shows, OpenDeal.showsByDefault, "no dev.flags override is in play")
        for players in tables {
            for salt in UInt8(1)...UInt8(2) {
                let chat = try await StartedChat.build(players: players, salt: salt &* 13 &+ UInt8(players),
                                                       gameId: UInt64(8100 + players * 10 + Int(salt)))
                for seat in -1..<players {
                    let c = try await opened(chat.started, seat: seat)
                    holdDeal(c.openReplayEvents, players: players, "\(players)p salt \(salt), seat \(seat) opens")
                }
            }
        }
    }

    /// ENTRY POINT 2: Start arriving on the lobby on screen, or tapped here.
    /// The surface plan fades the lobby out first - the deal comes after it, on
    /// the board the fade hands over to - and the board that takes over is a
    /// fresh controller on the started chain, exactly as in `GameSurface.adopt`
    /// (seatOnBoard) and `GameSurface.startGame`.
    func testStartArrivingFadesThenDealsForEverySeat() async throws {
        for players in tables {
            let chat = try await StartedChat.build(players: players, salt: 40 &+ UInt8(players),
                                                   gameId: UInt64(8200 + players))
            let plan = await MessageKernel.shared.surfacePlan(showing: chat.lobby, arriving: chat.started)
            let last = try XCTUnwrap(plan.beats.last, "\(players)p: Start arriving on a lobby is staged")
            XCTAssertEqual(last.kind, .board, "\(players)p: the last beat is the board")
            XCTAssertEqual(last.transition, .fade, "\(players)p: and the lobby fades into it")
            for seat in 0..<players {
                let c = try await opened(chat.started, seat: seat)
                holdDeal(c.openReplayEvents, players: players, "\(players)p Start arrives at seat \(seat)")
            }
        }
    }

    /// ON THE KERNEL'S CLOCK. The board flies each beat for what the plan says
    /// (`MessageTableView.pace`, MessageFlightPaceTests), so the deal it plays
    /// lasts exactly the kernel's per-card time for every card dealt.
    func testTheOpenedDealRunsAtTheKernelsPace() async throws {
        for players in tables {
            let chat = try await StartedChat.build(players: players, salt: 70 &+ UInt8(players),
                                                   gameId: UInt64(8300 + players))
            let c = try await opened(chat.started, seat: 0)
            let view = try XCTUnwrap(c.view, "\(players)p: a live board")
            let events = c.openReplayEvents
            let plan = AnimPlan(events, finalView: view)
            let deals = AnimBeats(events).beats.filter { $0.kind == .deal }
            var flying = 0.0
            for beat in deals { flying += MessageTableView.pace(of: beat, in: plan).flight }
            XCTAssertEqual(deals.count, players * 6, "\(players)p: one beat per dealt card")
            XCTAssertEqual(flying, Double(players * 6 * ANIM_DEAL_CARD_MS) / 1000, accuracy: 1e-9,
                           "\(players)p: the deal flies \(flying)s, want \(players * 6) x \(ANIM_DEAL_CARD_MS)ms")
        }
    }
}
