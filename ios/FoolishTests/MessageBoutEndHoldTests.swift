// A cover that ENDS the bout holds before the table is swept (round 16).
//
// The owner: "when you cover and cause the deck to discard (last defense), it
// should give some time to let people see what you covered with. So play the
// cover animation, brief pause, THEN the discard and deals and so on."
//
// Two things had to be true for that pause to be worth anything, and both are
// tested here:
//
//  1. THE RULE - the sequence holds after a cover, and ONLY after a cover that
//     its own bout end follows. A hold anywhere else is a stall.
//  2. THE TABLE - there is something to look at during it. When a cover ends
//     the bout in the same kernel apply, the board's prior view is the table
//     WITHOUT that cover (it went straight from "uncovered attack" to "empty"),
//     so the live sweep has to be re-taken from the kernel's own covered table
//     or the hold holds on a hole. That table is `preBoutTable`, and the second
//     half of this file is about it.
import XCTest
@testable import FoolishKit

@MainActor
final class MessageBoutEndHoldTests: XCTestCase {

    private func ev(_ kind: EventType, seat: Int = 1) -> GameEvent {
        GameEvent(type: kind.rawValue, seat: seat, msg: 0, from: 1, to: 2,
                  cards: [], target: nil, battle: nil, state: nil)
    }

    private func holds(_ kinds: [EventType]) -> [Bool] {
        AnimBeats(kinds.map { ev($0) }).beats.map(\.holds)
    }

    // MARK: the rule

    /// THE CASE: cover, then the bout closes in the same breath.
    func testACoverThatClosesTheBoutHolds() {
        XCTAssertEqual(holds([.cover, .discard, .refill]), [true, false, false])
        XCTAssertEqual(holds([.cover, .cardsToTrash, .refill]), [true, false, false])
    }

    /// A MULTI-CARD cover is one group, so it holds once - after both cards have
    /// flown, not between them (which would break round 16's parallel covers).
    func testAMultiCardCoverHoldsOnceAfterTheWholeGroup() {
        let groups = AnimBeats([ev(.cover), ev(.cover), ev(.discard)])
        XCTAssertEqual(groups.beats.map(\.count), [2, 1], "the two cards are one beat")
        XCTAssertTrue(groups.beats[0].holds)
        XCTAssertFalse(groups.beats[1].holds)
    }

    /// Notices between the cover and the trash do not break the pair. A cover
    /// that empties the defender's hand puts their OUT in the way; the last bout
    /// of a game adds a magic transition. Neither moves a card, so neither is a
    /// beat the hold should fall on - or a wall it should stop at.
    func testNoticesBetweenTheCoverAndTheTrashDoNotBreakThePair() {
        XCTAssertEqual(holds([.cover, .out, .discard]), [true, false, false])
        XCTAssertEqual(holds([.cover, .out, .magicTransition, .cardsToTrash]),
                       [true, false, false, false])
    }

    /// A cover the bout did NOT close on does not hold - the sequence is still
    /// going somewhere and a pause mid-flight is a stutter, not a beat.
    func testACoverThatDidNotEndTheBoutDoesNotHold() {
        XCTAssertEqual(holds([.cover]), [false])
        XCTAssertEqual(holds([.cover, .attackPass]), [false, false])
        // A card MOVING after the cover ends the scan even though a discard
        // follows it: that discard belongs to a later beat, not to this cover.
        XCTAssertEqual(holds([.cover, .refill, .discard]), [false, false, false])
        XCTAssertEqual(holds([.cover, .pickup, .discard]), [false, false, false])
    }

    /// THE COMMON BOUT END is not this one. Defender covers and sends; an
    /// attacker then says good, and THAT bubble carries the discard with no
    /// cover in it. Nothing holds - the table has been sitting there readable
    /// since the previous bubble, and a pause before the sweep would be a pause
    /// in front of a board nobody just changed.
    func testAGoodBubblesDiscardDoesNotHold() {
        XCTAssertEqual(holds([.discard, .refill, .refill]), [false, false, false])
    }

    /// Nothing else holds, and the bounds are the bounds.
    func testNothingElseHoldsAndTheBoundsHold() {
        XCTAssertEqual(holds([.pickup, .refill]), [false, false])
        XCTAssertEqual(holds([.deal, .deal, .flipped]), [false, false, false])
        XCTAssertEqual(holds([]), [])
        XCTAssertEqual(AnimBeats([]).beats.count, 0, "an empty stream has no beats to hold after")
    }

    /// The hold is a REAL duration - long enough to read a card, and it scales
    /// with the flights around it rather than being a bare constant that shrinks
    /// to nothing when a filmed sequence is slowed down.
    ///
    /// ROUND 20 pins the number the owner asked for by name: a second and a
    /// half at the shipping `flightTime`. Both bounds are still here - it is a
    /// pause, not a stall - they are just an order of magnitude apart now,
    /// because the first attempt at "a beat" (0.9 flights) was measured and
    /// still read as part of the motion.
    func testTheHoldIsAReadableBeatAndScalesWithTheFlights() {
        XCTAssertEqual(boutEndHold, 1.5, accuracy: 0.001, "the owner asked for 1.5s")
        XCTAssertGreaterThan(boutEndHold, flightTime * 2, "long enough that the eye stops")
        XCTAssertLessThan(boutEndHold, flightTime * 5, "a pause, not a stall")
        XCTAssertGreaterThan(boutEndHold, flightGap * 10, "not the ordinary inter-step gap")
    }

    // MARK: the table the hold holds on

    private func freshSeed(_ salt: UInt8) -> Data {
        Data((0..<32).map { UInt8(truncatingIfNeeded: $0 &* 29 &+ Int(salt)) | 1 })
    }

    /// One apply of a real game whose kernel stream holds BOTH a cover and the
    /// bout end, i.e. the owner's case: covering was the whole move, and the
    /// table went with it. Reached by playing games out - it is an ENDGAME
    /// shape (the defender's last card goes down, so no attacker can add
    /// another), which no opening deal can be arranged into.
    private struct ClosingCover {
        let events: [GameEvent]
        let priorBattles: [BattleView]     // the board the defender saw, pre-apply
        let card: Card                     // what they covered with
    }

    private func findClosingCover() async throws -> ClosingCover? {
        let k = MessageKernel.shared
        for salt: UInt8 in 1...40 {
            try await k.newGame(seed: freshSeed(salt), players: 2)
            for _ in 0..<400 {
                guard let view = await k.residentView(viewer: -1), !view.isOver else { break }
                var acted = false
                for seat in 0..<view.players.count {
                    let legal = await k.residentLegal(seat: seat)
                    guard !legal.isEmpty else { continue }
                    // Prefer covering, so games reach the endgame through the
                    // move this test is about rather than round after round of
                    // pickups.
                    let move = legal.first { $0.type == .cover } ?? legal[0]
                    let prior = view.battles
                    try await k.apply(seat: seat, move: move)
                    acted = true
                    guard move.type == .cover, let card = move.cards.first else { break }
                    let events = await k.lastMoveEvents(viewer: seat)
                    if events.contains(where: { $0.kind == .cover }),
                       events.contains(where: { $0.kind == .discard || $0.kind == .cardsToTrash }) {
                        return ClosingCover(events: events, priorBattles: prior, card: card)
                    }
                    break
                }
                if !acted { break }
            }
        }
        return nil
    }

    /// THE HALF THAT MAKES THE PAUSE WORTH ANYTHING.
    ///
    /// The board a bout-ending cover is swept off used to be built from the
    /// view BEFORE the apply - and that view does not have the cover on it,
    /// because the kernel went from "attack, uncovered" straight to "table
    /// empty" in one step. Holding on that board is holding on a hole where the
    /// card the player just played should be. So the live sweep is now re-taken
    /// from the kernel's own stream, and this is that table: it must name the
    /// covering card, which the prior view provably does not.
    ///
    /// Mutation guards: asserted by card IDENTITY, so a `preBoutTable` that
    /// walked back one state too far (the uncovered table) still clears the
    /// "non-empty" bar and fails here; and the prior view is asserted to LACK
    /// the card, so this cannot pass by the two boards being the same board.
    func testTheSweptTableCarriesTheCoverThePriorViewNeverHeld() async throws {
        guard let found = try await findClosingCover() else {
            throw XCTSkip("no 2p game in 40 reached a cover that closed its own bout")
        }
        XCTAssertFalse(found.priorBattles.contains {
            $0.attack == found.card || $0.defense == found.card
        }, "the pre-apply view already had the cover - this is not the case under test")

        let table = MessageTurnController.preBoutTable(found.events)
        XCTAssertFalse(table.isEmpty)
        XCTAssertTrue(table.contains { $0.defense == found.card },
                      "the swept table must carry the cover that ended the bout")
        // …and it must not LOSE anything the prior view had, which is the guard
        // `coveredSweep` enforces before it accepts the swap.
        for b in found.priorBattles {
            XCTAssertTrue(table.contains { $0.attack == b.attack },
                          "the kernel's table dropped a card the board was showing")
        }
    }

    /// THE SWAP GUARD. `coveredSweep` accepts the kernel's table only when it
    /// accounts for everything the board is already showing - it may ADD the
    /// cover, never drop a card.
    ///
    /// The refusal case is real, not defensive decoration: `preBoutTable`
    /// reconstructs a PICKUP as one uncovered slot per card, which for a table
    /// of covered pairs is the same cards in a different, flatter shape. Taking
    /// that would un-cover the table mid-sequence. It is only reachable if a
    /// future caller drops the cover-only gate, so this pins the contract at the
    /// function rather than at its one call site.
    func testTheSweptTableIsOnlySwappedForOneThatKeepsEveryCard() async throws {
        guard let found = try await findClosingCover() else {
            throw XCTSkip("no 2p game in 40 reached a cover that closed its own bout")
        }
        let taken = try XCTUnwrap(MessageTableView.coveredSweep(found.events,
                                                               current: found.priorBattles),
                                  "the kernel's covered table must be accepted")
        XCTAssertTrue(taken.contains { $0.defense == found.card })

        // A board holding a card the stream never mentions is NOT swapped out.
        let stranger = Card(s: 9, v: 9)
        XCTAssertNil(MessageTableView.coveredSweep(
            found.events, current: found.priorBattles + [BattleView(attack: stranger, defense: nil)]))
        // …and an empty stream is nothing to swap TO.
        XCTAssertNil(MessageTableView.coveredSweep([], current: found.priorBattles))
    }

    /// …and the rule fires on that real stream, not only on synthesized kinds.
    func testARealClosingCoverHolds() async throws {
        guard let found = try await findClosingCover() else {
            throw XCTSkip("no 2p game in 40 reached a cover that closed its own bout")
        }
        let groups = AnimBeats(found.events).beats
        let ci = try XCTUnwrap(groups.firstIndex { $0.kind == .cover })
        XCTAssertTrue(groups[ci].holds,
                      "a cover that closed its bout must hold before the sweep")
        for i in 0..<groups.count where i != ci {
            XCTAssertFalse(groups[i].holds, "only the cover holds")
        }
    }
}
