// Multiple covers played as ONE move animate together (round 16).
//
// The kernel emits one COVER event per card - one engine hook per pair, each
// with its own board snapshot - so a two-card cover reaches a receiver as two
// events. Played as two steps the receiver watches the cards leave the hand one
// after the other, while the player who made the move saw them go at once.
//
// The move boundary is NOT on the chain to group by: a v6 body spends one COVER
// atom per card, so one two-card cover and two one-card covers are the same
// atoms in the same order. The boundary that IS on the chain is the bubble, so
// that is what the board groups by - and the case that must survive is two
// covers sent as TWO BUBBLES, where opening the second must animate one cover
// and not both. That is round 16's bubble delta doing the work, and the third
// test here is the one that would catch it regressing.
import XCTest
@testable import FoolishKit

@MainActor
final class MessageParallelCoverTests: XCTestCase {

    private func seed(_ salt: UInt8) -> Data {
        Data((0..<32).map { UInt8(truncatingIfNeeded: $0 &* 17 &+ Int(salt)) | 1 })
    }

    /// A dealt 2-player game where the first attacker can play TWO cards of one
    /// value and the defender can then cover both. Returns (attack, the two
    /// single-card covers), or nil for a deal that offers no such line.
    private func twoCardBout(salt: UInt8) async throws -> (atk: Int, def: Int, attack: Move)? {
        let k = MessageKernel.shared
        try await k.newGame(seed: seed(salt), players: 2)
        guard let view = await k.residentView(viewer: -1) else { return nil }
        let atk = view.defender == 0 ? 1 : 0
        let legal = await k.residentLegal(seat: atk)
        guard let pair = legal.first(where: { $0.type == .attack && $0.cards.count == 2 })
        else { return nil }
        return (atk, view.defender, pair)
    }

    /// The receiver's view of the last bubble: seal as `actor`, decode as
    /// `viewer`, and read the events that bubble carries.
    private func received(gameId: UInt64, actor: Int, viewer: Int,
                          joins: [MessageJoin], base: Int) async throws -> [GameEvent] {
        let k = MessageKernel.shared
        let payload = try await k.seal(phase: 2, lastActorSeat: actor, gameId: gameId,
                                       parent8: Data(repeating: 0, count: 8), joins: joins)
        let env = try await k.decode(payload: payload, viewer: viewer)
        XCTAssertGreaterThan(env.turn, base, "nothing was actually played")
        return await k.lastMoveEvents(viewer: viewer, atomsBefore: base)
    }

    private let joins = [MessageJoin(seat: 0, name: "Alex"), MessageJoin(seat: 1, name: "Bob")]

    /// Find a deal that offers a two-card attack, play it, and hand back the
    /// board mid-bout with the atom count so far.
    private func openTwoCardBout() async throws -> (def: Int, base: Int, covers: [Move])? {
        let k = MessageKernel.shared
        for salt: UInt8 in 1...60 {
            guard let bout = try await twoCardBout(salt: salt) else { continue }
            try await k.apply(seat: bout.atk, move: bout.attack)
            // Seal once so the attack is a bubble of its own; its turn is the
            // base the cover bubble measures from.
            let after = try await k.seal(phase: 2, lastActorSeat: bout.atk, gameId: 8100,
                                         parent8: Data(repeating: 0, count: 8), joins: joins)
            let env = try await k.decode(payload: after, viewer: -1)

            let legal = await k.residentLegal(seat: bout.def)
            let singles = legal.filter { $0.type == .cover && $0.cards.count == 1 }
            // Two distinct single-card covers, each answering a different attack.
            guard singles.count >= 2 else { continue }
            var chosen: [Move] = []
            var usedTargets: Set<String> = [], usedCards: Set<String> = []
            for m in singles {
                guard let card = m.cards.first, let target = m.attackCards?.first else { continue }
                let cid = "\(card.s)-\(card.v)", tid = "\(target.s)-\(target.v)"
                if usedCards.contains(cid) || usedTargets.contains(tid) { continue }
                usedCards.insert(cid); usedTargets.insert(tid)
                chosen.append(m)
                if chosen.count == 2 { break }
            }
            guard chosen.count == 2 else { continue }
            return (bout.def, env.turn, chosen)
        }
        return nil
    }

    /// TWO CARDS, ONE MOVE: one step, both cards in it.
    func testTwoCardsCoveredInOneMoveFlyTogether() async throws {
        let k = MessageKernel.shared
        guard let bout = try await openTwoCardBout() else {
            XCTFail("no deal in 60 offered a two-card bout"); return
        }
        let cards = bout.covers.compactMap { $0.cards.first }
        let targets = bout.covers.compactMap { $0.attackCards?.first }
        XCTAssertEqual(cards.count, 2); XCTAssertEqual(targets.count, 2)
        try await k.apply(seat: bout.def,
                          move: Move(type: .cover, cards: cards, attackCards: targets))

        let events = try await received(gameId: 8100, actor: bout.def, viewer: 0,
                                        joins: joins, base: bout.base)
        let covers = events.filter { $0.kind == .cover }
        XCTAssertEqual(covers.count, 2, "the kernel emits one COVER event per card")

        let groups = MessageTableView.parallelGroups(events)
        let coverGroups = groups.filter { $0.first?.kind == .cover }
        XCTAssertEqual(coverGroups.count, 1, "the two cards were played as two beats")
        XCTAssertEqual(coverGroups.first?.count, 2, "both cards must fly in one beat")
    }

    /// THE SAME TWO CARDS AS TWO MOVES, STAGED INTO ONE BUBBLE: also one step.
    ///
    /// Not an oversight - the chain genuinely cannot tell this apart from the
    /// test above (same atoms, same order), and a bubble is the finest boundary
    /// it does record. They arrived together, so they fly together. Asserted
    /// rather than left implicit so that if the codec ever does learn move
    /// boundaries, whoever changes this has to change a test that says why.
    func testTwoSeparateCoversInOneBubbleAlsoFlyTogether() async throws {
        let k = MessageKernel.shared
        guard let bout = try await openTwoCardBout() else {
            XCTFail("no deal in 60 offered a two-card bout"); return
        }
        for m in bout.covers { try await k.apply(seat: bout.def, move: m) }

        let events = try await received(gameId: 8101, actor: bout.def, viewer: 0,
                                        joins: joins, base: bout.base)
        XCTAssertEqual(events.filter { $0.kind == .cover }.count, 2,
                       "both covers are in this bubble")
        let coverGroups = MessageTableView.parallelGroups(events)
            .filter { $0.first?.kind == .cover }
        XCTAssertEqual(coverGroups.map(\.count), [2])
    }

    /// TWO BUBBLES, ONE COVER EACH: opening the second animates ITS cover and
    /// nothing else.
    ///
    /// This is the case the grouping must not eat. Round 16 made a bubble state
    /// how much of the chain it added, and `lastMoveEvents` cuts on exactly
    /// that - so the second bubble's stream contains one cover, and there is
    /// nothing for `parallelGroups` to merge it with. Drop the delta and this
    /// test fails: the second open would carry both covers and fly them as one.
    func testTwoBubblesWithOneCoverEachEachAnimateOnlyTheirOwn() async throws {
        let k = MessageKernel.shared
        guard let bout = try await openTwoCardBout() else {
            XCTFail("no deal in 60 offered a two-card bout"); return
        }

        // Bubble A: the first cover, sent on its own.
        try await k.apply(seat: bout.def, move: bout.covers[0])
        let a = try await k.seal(phase: 2, lastActorSeat: bout.def, gameId: 8102,
                                 parent8: Data(repeating: 0, count: 8), joins: joins)
        let envA = try await k.decode(payload: a, viewer: 0)
        let eventsA = await k.lastMoveEvents(viewer: 0, atomsBefore: bout.base)
        XCTAssertEqual(eventsA.filter { $0.kind == .cover }.count, 1,
                       "bubble A carries one cover")
        XCTAssertEqual(MessageTableView.parallelGroups(eventsA)
                        .filter { $0.first?.kind == .cover }.map(\.count), [1])

        // Bubble B: the second cover, sent after it.
        try await k.apply(seat: bout.def, move: bout.covers[1])
        let b = try await k.seal(phase: 2, lastActorSeat: bout.def, gameId: 8102,
                                 parent8: MessageTurnController.firstEight(hex: envA.digest),
                                 joins: joins)
        let envB = try await k.decode(payload: b, viewer: 0)
        XCTAssertGreaterThan(envB.turn, envA.turn)
        let eventsB = await k.lastMoveEvents(viewer: 0, atomsBefore: envA.turn)
        XCTAssertEqual(eventsB.filter { $0.kind == .cover }.count, 1,
                       "opening bubble B replayed bubble A's cover too")
        XCTAssertEqual(MessageTableView.parallelGroups(eventsB)
                        .filter { $0.first?.kind == .cover }.map(\.count), [1],
                       "the second bubble must animate its own cover alone")
    }

    // MARK: the grouping rule itself

    /// Only covers group, and only within one action. Everything else keeps its
    /// own beat - a bout's closing discard shares the cover's action but is its
    /// consequence, not part of the same movement.
    func testGroupingIsCoverOnlyAndActionScoped() {
        func ev(_ kind: EventType, seat: Int) -> GameEvent {
            GameEvent(type: kind.rawValue, seat: seat, msg: 0, from: 1, to: 2,
                      cards: [], target: nil, battle: nil, state: nil)
        }
        // Same seat, adjacent covers -> one beat.
        XCTAssertEqual(MessageTableView.parallelGroups(
            [ev(.cover, seat: 1), ev(.cover, seat: 1)]).map(\.count), [2])
        // Different seat -> two beats (a pass moved the shield mid-turn).
        XCTAssertEqual(MessageTableView.parallelGroups(
            [ev(.cover, seat: 1), ev(.cover, seat: 2)]).map(\.count), [1, 1])
        // NOT adjacent -> two beats. A bout that closed between two covers puts
        // its discard in the way, and the covers belong to different bouts.
        XCTAssertEqual(MessageTableView.parallelGroups(
            [ev(.cover, seat: 1), ev(.discard, seat: -1), ev(.cover, seat: 1)]).map(\.count),
            [1, 1, 1])
        // A cover's own consequences keep their beats.
        XCTAssertEqual(MessageTableView.parallelGroups(
            [ev(.cover, seat: 1), ev(.cover, seat: 1),
             ev(.discard, seat: -1), ev(.refill, seat: 0)]).map(\.count),
            [2, 1, 1])
        // Nothing else groups, however adjacent.
        XCTAssertEqual(MessageTableView.parallelGroups(
            [ev(.refill, seat: 0), ev(.refill, seat: 0)]).map(\.count), [1, 1])
        XCTAssertEqual(MessageTableView.parallelGroups([]).count, 0)
    }
}
