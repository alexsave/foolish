// TrumpFlightTests - the flipped trump flies to whoever draws it.
//
// Owner: "the flipped card should also have a deal animation to whoever gets
// it". The trump is the last card a game deals: once the stock is empty the
// next draw takes it from under the deck, face up, as the LAST card of that
// seat's refill. The kernel's plan names that card on the step
// (AnimPlanStep.trump_out, c/src/anim_plan.h) by its real identity for every
// viewer, leaving from the flipped slot. The board flew every card of the
// draw from the deck instead, and to another seat as backs - so the trump,
// lying face up a moment before, left the stock as a back and the slot just
// emptied.
//
// Held here against REAL kernel plans: a game is played bubble by bubble, the
// way two or four devices play it, until a move's refill takes the trump. The
// bubble that carries it is then opened at every seat, the receiver and the
// others, and the draw's flights are asked of `MessageTableView.drawFlights`
// with three distinct rects for the deck, the trump's slot and the hand, so
// where each card leaves from is unambiguous. At 2 and at 4 players.
//
// `TrumpFlight.flies` (`trump.flight` in dev.flags) guards it: off, the plan's
// trump is not passed and the draw flies exactly as it did before.
import XCTest
@testable import FoolishKit

@MainActor
final class TrumpFlightTests: XCTestCase {

    private let deck = CGRect(x: 10, y: 10, width: 66, height: 46)
    private let slot = CGRect(x: 18, y: 28, width: 46, height: 66)
    private let hand = CGRect(x: 0, y: 600, width: 390, height: 120)
    private let badge = CGRect(x: 300, y: 120, width: 40, height: 40)
    private let landed = CGRect(x: 150, y: 620, width: 50, height: 70)

    /// One opened bubble, at one seat: the stream it plays, the plan the board
    /// asks for it, and the step of that plan that deals the trump.
    private struct Opened {
        let seat: Int
        let events: [GameEvent]
        let plan: AnimPlan
        let index: Int
        var event: GameEvent { events[index] }
        var step: AnimPlan.Step { plan.steps[index] }
    }

    /// A started chat of `players`, played bubble by bubble - each one sealed
    /// by the seat that moved, on its own device - until a bubble's move deals
    /// the flipped trump. Hands back that bubble.
    private func bubbleDealingTheTrump(players: Int, salt: UInt8) async throws -> (payload: Data, actor: Int)? {
        let started = try await StartedChat.build(players: players, salt: salt,
                                                  gameId: 9100 + UInt64(players))
        var payload = started.started
        var env = try await MessageEnvelope.decode(payload: payload, viewer: -1)
        var next = 0
        // The board the previous bubble was sealed on: did it still hold the
        // trump? Read off each bubble's own opened board, never off the
        // sealing controller, whose view may be the held pre-settlement one.
        var hadTrump: Bool?
        var last: (payload: Data, actor: Int)?
        for _ in 0..<600 {
            var moved = false
            for t in 0..<players {
                let seat = (next + t) % players
                let c = MessageTurnController(parentPayload: payload, parent: env, mySeat: seat,
                                              suppressOpenReplay: true)
                await c.begin()
                guard let board = c.view else { return nil }
                if t == 0 {
                    if hadTrump == true, !board.hasFlipped { return last }
                    hadTrump = board.hasFlipped
                }
                if board.isOver { return nil }
                guard let move = c.legal.first(where: { $0.type != .wait }) else { continue }
                _ = await c.apply(move)
                guard !c.pending.isEmpty else { continue }
                let sealed = try await c.stagedPayload(sentAt: MessageKernel.clockNow() - 60)
                env = try await MessageEnvelope.decode(payload: sealed, viewer: -1)
                payload = sealed
                last = (sealed, seat)
                next = (seat + 1) % players
                moved = true
                break
            }
            if !moved { return nil }
        }
        return nil
    }

    /// `payload` opened at `seat`, as a tapped bubble opens it.
    private func open(_ payload: Data, seat: Int) async throws -> Opened? {
        let env = try await MessageEnvelope.decode(payload: payload, viewer: -1)
        let c = MessageTurnController(parentPayload: payload, parent: env, mySeat: seat)
        await c.begin()
        guard let view = c.view else { return nil }
        let events = c.openReplayEvents
        let plan = AnimPlan(events, finalView: view)
        guard plan.steps.count == events.count,
              let i = plan.steps.firstIndex(where: { $0.trumpOut != nil }) else { return nil }
        return Opened(seat: seat, events: events, plan: plan, index: i)
    }

    /// The draw's flights at `o.seat`, with the plan's trump passed as the board
    /// passes it while `trump.flight` is on.
    private func flights(_ o: Opened, trumpOut: Card?) -> [Flight] {
        MessageTableView.drawFlights(o.event, mine: o.event.seat == o.seat, trumpOut: trumpOut,
                                     deck: deck, trumpSlot: slot, hand: hand, badge: badge,
                                     lastChance: false) { _, _, _ in self.landed } ?? []
    }

    /// The trump's flight and everyone else's, at every seat of one bubble.
    private func holdTheTrumpFlight(players: Int) async throws {
        var found: (payload: Data, actor: Int)?
        for salt in UInt8(1)...UInt8(12) {
            found = try await bubbleDealingTheTrump(players: players, salt: salt)
            if found != nil { break }
        }
        guard let (payload, _) = found else {
            return XCTFail("\(players)p: no played game dealt the trump out")
        }
        var sawReceiver = false, sawOther = false
        for seat in 0..<players {
            guard let o = try await open(payload, seat: seat) else {
                XCTFail("\(players)p seat \(seat): the opened stream has no step that deals the trump")
                continue
            }
            let at = "\(players)p seat \(seat) (trump to seat \(o.event.seat))"
            let trump = try XCTUnwrap(o.step.trumpOut, at)
            XCTAssertEqual(o.step.trumpFrom, EventLoc.flipped.rawValue, "\(at): the plan names the slot")
            let n = o.event.cards.count
            XCTAssertGreaterThanOrEqual(n, 1, "\(at): the draw carries the trump")

            let f = flights(o, trumpOut: trump)
            XCTAssertEqual(f.count, n, "\(at): one flight per card drawn")
            let trumpFlights = f.filter { $0.card == trump }
            XCTAssertEqual(trumpFlights.count, 1,
                           "\(at): the trump flies face up, once (flights \(f.map(\.id)))")
            XCTAssertEqual(trumpFlights.first?.from, slot,
                           "\(at): the trump leaves from its own slot, not the deck")
            XCTAssertEqual(trumpFlights.first?.ghostSize(at: 0), slot.size,
                           "\(at): the trump takes off at the slot card's own size, not the ghost's")
            let rest = f.filter { $0.card != trump }
            XCTAssertEqual(rest.count, n - 1, "\(at): the rest of the draw")
            XCTAssertTrue(rest.allSatisfy { $0.from == deck },
                          "\(at): every other card of the draw leaves the deck")
            if o.event.seat == o.seat {
                sawReceiver = true
                XCTAssertTrue(f.allSatisfy { $0.card != nil }, "\(at): my own draw is face up")
                XCTAssertTrue(f.allSatisfy { $0.to == landed }, "\(at): into my hand")
            } else {
                sawOther = true
                XCTAssertTrue(rest.allSatisfy { $0.card == nil },
                              "\(at): another seat's stock cards are backs")
                XCTAssertTrue(f.allSatisfy { badge.insetBy(dx: -40, dy: 0).contains($0.to.origin) },
                              "\(at): to that seat's badge")
            }

            // A SLOT THAT HAS NOT PUBLISHED is polled for, like any frame; on
            // the last chance the trump still flies, from the deck.
            let mine = o.event.seat == o.seat
            XCTAssertNil(MessageTableView.drawFlights(o.event, mine: mine, trumpOut: trump, deck: deck,
                                                      trumpSlot: .zero, hand: hand, badge: badge,
                                                      lastChance: false) { _, _, _ in self.landed },
                         "\(at): no slot yet - poll again")
            let late = MessageTableView.drawFlights(o.event, mine: mine, trumpOut: trump, deck: deck,
                                                    trumpSlot: .zero, hand: hand, badge: badge,
                                                    lastChance: true) { _, _, _ in self.landed } ?? []
            XCTAssertEqual(late.first { $0.card == trump }?.from, deck,
                           "\(at): last chance - the trump still flies, from the deck")

            // FLAG OFF: the plan's trump is not passed, and the draw is exactly
            // what it was before - every card from the deck, backs to a badge.
            let off = flights(o, trumpOut: nil)
            XCTAssertEqual(off.count, max(n, 1), "\(at) flag off: one flight per card")
            XCTAssertTrue(off.allSatisfy { $0.from == deck }, "\(at) flag off: all from the deck")
            if o.event.seat != o.seat {
                XCTAssertTrue(off.allSatisfy { $0.card == nil }, "\(at) flag off: backs")
            }
        }
        XCTAssertTrue(sawReceiver, "\(players)p: the receiver's view was held")
        XCTAssertTrue(sawOther, "\(players)p: another seat's view was held")
    }

    func testTheFlagShipsOn() {
        XCTAssertTrue(TrumpFlight.fliesByDefault, "the trump's own flight is what ships")
        XCTAssertEqual(TrumpFlight.flies, TrumpFlight.fliesByDefault, "no dev.flags override is in play")
    }

    /// The slot the flight leaves from is the rect the well draws the trump
    /// in: `flippedOrigin` off the well's corner, one 46x66 card, scaled with
    /// the well.
    func testTheSlotIsWhereTheWellDrawsTheTrump() {
        let well = CGRect(x: 8, y: 14, width: 92, height: 108)
        XCTAssertEqual(FDeckWell.trumpSlot(inWell: well),
                       CGRect(x: 8 + FDeckWell.flippedOrigin.x, y: 14 + FDeckWell.flippedOrigin.y,
                              width: 46, height: 66))
        XCTAssertEqual(FDeckWell.trumpSlot(inWell: well, scale: 0.5),
                       CGRect(x: 8 + FDeckWell.flippedOrigin.x / 2, y: 14 + FDeckWell.flippedOrigin.y / 2,
                              width: 23, height: 33))
        XCTAssertEqual(FDeckWell.trumpSlot(inWell: .zero), .zero, "an unmeasured well publishes nothing")
    }

    /// The ghost's size runs from `fromSize` to `size` over the flight, both
    /// defaulting to `Flight.ghost`. (The reversal's swap is held in
    /// ConflictModelTests, beside the angles it mirrors.)
    func testAGhostGrowsFromItsSourceSizeToItsLandingSize() {
        let plain = Flight(id: "p", card: nil, from: deck, to: badge)
        XCTAssertEqual(plain.ghostSize(at: 0), Flight.ghost, "an unsized flight is the ghost at take-off")
        XCTAssertEqual(plain.ghostSize(at: 1), Flight.ghost, "…and at landing")
        let card = Card(s: 2, v: 7)
        let f = Flight(id: "t", card: card, from: slot, to: landed, fromSize: slot.size)
        XCTAssertEqual(f.ghostSize(at: 0), slot.size, "take-off is the source card's size")
        XCTAssertEqual(f.ghostSize(at: 1), Flight.ghost, "landing is the ghost's")
        XCTAssertEqual(f.ghostSize(at: 0.5), CGSize(width: 48, height: 68), "and it grows in between")
    }

    func testTheTrumpFliesFromItsSlotAt2Players() async throws {
        try await holdTheTrumpFlight(players: 2)
    }

    func testTheTrumpFliesFromItsSlotAt4Players() async throws {
        try await holdTheTrumpFlight(players: 4)
    }
}
