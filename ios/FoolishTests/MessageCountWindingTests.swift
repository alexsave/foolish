// The counts a board shows WHILE a move animates (round 16 defect report).
//
// The owner, on the last-defense scenario: "I sometimes saw the deck suddenly
// go to 5 cards, then deal, and now I have 6 cards? Is it a problem with the
// flipped card? Similarly sometimes in other scenarios id see a player briefly
// have their card count bumped, then they play a single card, and it goes back
// down to the expected value. If I replay it it doesn't happen again."
//
// Both are the same shape: a badge showing a number that is not any board in
// the sequence, corrected a beat later. A displayed count comes from exactly
// two places, and this file pins both against the kernel rather than against
// itself:
//
//  * the COUNT FREEZE - the board a sequence STARTS at, `AnimPlan.pre` over the
//    kernel's anim_build_plan. It re-derives the pre-move board from the first
//    event's own board, so it can only be right if it undoes that event exactly.
//    Asserted here against the board the kernel actually had before the move.
//  * `GameEvent.state` - the per-step board each flight settles to. Asserted to
//    walk from the pre-move board to the post-move board without ever going
//    backwards (a deck that refills mid-sequence, a discard that shrinks).
//
// Driven over real games at 2/3/4 players, every move, so the endgame shapes
// the report came from - an empty deck, the flipped trump being drawn, a
// bout-ending cover - are all inside the sweep rather than needing a fixture
// each.
// MUTATION-CHECKED against sdk/swift/AnimPlanWire.swift, each on its own:
//
//   the encoder stops sending each event's own board (has_counts always 0), so
//     the kernel falls back to undoing every event  -> 36 failures, the deck
//                                                      read 2 where it was 1
//   a redacted step's card count becomes its
//     identity count (a masked draw moves nothing)  -> 36 failures
//   the freeze's seat block is read one byte over   -> 635 failures
//   a step's card count is read one byte over       -> 434 failures
import XCTest
@testable import FoolishKit

@MainActor
final class MessageCountWindingTests: XCTestCase {

    private func seed(_ salt: UInt8) -> Data {
        Data((0..<32).map { UInt8(truncatingIfNeeded: $0 &* 31 &+ Int(salt)) | 1 })
    }

    private struct Counts: Equatable, CustomStringConvertible {
        let deck: Int, discard: Int, hand: [Int: Int]
        init(_ v: GameView) {
            deck = v.deckCount; discard = v.discardCount
            hand = Dictionary(uniqueKeysWithValues: v.players.map { ($0.seat, $0.handCount) })
        }
        init(deck: Int, discard: Int, hand: [Int: Int]) {
            self.deck = deck; self.discard = discard; self.hand = hand
        }
        var description: String { "deck \(deck), discard \(discard), hands \(hand.sorted { $0.key < $1.key })" }
    }

    /// Every BUBBLE of `players`-handed games: the board before it, the kernel's
    /// event stream for exactly it, and the board after.
    ///
    /// The bubble, not the move, because that is the boundary the board animates
    /// on. `lastMoveEvents` with no `atomsBefore` lets the KERNEL group the last
    /// turn, and its grouping is not one apply - a defence and the good that
    /// closes it come back together. The board never asks that way: it passes
    /// the chain's own delta (`MessageTurnController.animAtomsBefore`, from the
    /// envelope), so the stream is exactly what this bubble added. Comparing a
    /// freeze over a kernel-grouped stream against a one-apply-ago board
    /// measures the grouping, not the freeze - so each move here is sealed,
    /// and its seal's atom count becomes the next one's base.
    private func sweepBubbles(players: Int, games: Int,
                              _ body: (_ before: GameView, _ events: [GameEvent],
                                       _ after: GameView, _ label: String) throws -> Void) async throws {
        let k = MessageKernel.shared
        let joins = (0..<players).map { MessageJoin(seat: $0, name: "P\($0)") }
        for salt in 0..<games {
            try await k.newGame(seed: seed(UInt8(salt &* 7 &+ 1)), players: players)
            var base = -1                      // the genesis bubble adds all of itself
            for step in 0..<400 {
                guard let before = await k.residentView(viewer: -1), !before.isOver else { break }
                var acted = false
                for seat in 0..<players {
                    let legal = await k.residentLegal(seat: seat)
                    guard !legal.isEmpty else { continue }
                    // Cover when we can, so games reach the endgame through
                    // covers (the report's scenario) rather than pickup loops.
                    let move = legal.first { $0.type == .cover } ?? legal[0]
                    try await k.apply(seat: seat, move: move)
                    acted = true
                    // A chain long enough to stop sealing (a very long game runs
                    // the body past what one bubble may carry) ends this game
                    // rather than the sweep - there is nothing to check past a
                    // boundary the wire cannot state.
                    guard let payload = try? await k.seal(phase: 2, lastActorSeat: seat,
                                                          gameId: 9100,
                                                          parent8: Data(repeating: 0, count: 8),
                                                          joins: joins),
                          let env = try? await k.decode(payload: payload, viewer: -1),
                          let after = await k.residentView(viewer: -1) else { acted = false; break }
                    // Viewer -1 (a spectator) is the strict case: it sees every
                    // seat's counts, and the kernel redacts its card identities,
                    // so a freeze that leant on identities rather than counts
                    // fails here.
                    let events = await k.lastMoveEvents(viewer: -1, atomsBefore: base)
                    try body(before, events, after,
                             "\(players)p game \(salt) step \(step) seat \(seat) \(move.type)")
                    base = env.turn
                    break
                }
                if !acted { break }
            }
        }
    }

    /// THE FREEZE IS THE PRE-MOVE BOARD. Not "close to it", not "right for
    /// the seats that moved": the same deck, the same discard, the same count in
    /// every hand. This is the number every badge shows for the first frame of
    /// an arriving move, so anywhere it is wrong is a badge that displays a
    /// board that never existed and then corrects itself - which is the report.
    func testTheWalkBackReproducesTheBoardBeforeEveryMove() async throws {
        for players in [2, 3, 4] {
            try await sweepBubbles(players: players, games: 4) { before, events, after, label in
                guard !events.isEmpty else { return }
                let pre = AnimPlan(events, finalView: after).pre
                let got = Counts(deck: pre.deck, discard: pre.discard, hand: pre.hand)
                if got != Counts(before) {
                    let stream = events.map {
                        "\($0.kind.map(String.init(describing:)) ?? "?")@\($0.seat)"
                        + "x\($0.cards.count) -> deck \($0.state?.deckCount ?? -1)"
                    }.joined(separator: " | ")
                    XCTFail("\(label): freeze \(got), board was \(Counts(before))\n  after: "
                            + "\(Counts(after))\n  stream: \(stream)")
                }
            }
        }
    }

    /// …and it must be exercised, not skipped: a sweep that silently found no
    /// events would pass the test above by doing nothing.
    func testTheSweepActuallySeesTheShapesTheReportCameFrom() async throws {
        var moves = 0, withEvents = 0, emptyDeck = 0, refills = 0, boutEnds = 0
        try await sweepBubbles(players: 2, games: 4) { before, events, after, _ in
            moves += 1
            if !events.isEmpty { withEvents += 1 }
            if before.deckCount == 0 { emptyDeck += 1 }
            if events.contains(where: { $0.kind == .refill }) { refills += 1 }
            if events.contains(where: { $0.kind == .discard || $0.kind == .cardsToTrash }) { boutEnds += 1 }
        }
        XCTAssertGreaterThan(moves, 100, "the drive did not play any real games")
        XCTAssertGreaterThan(withEvents, 100, "no move produced a kernel event stream")
        XCTAssertGreaterThan(refills, 10, "no deal/refill was ever animated")
        XCTAssertGreaterThan(boutEnds, 10, "no bout ever ended")
        XCTAssertGreaterThan(emptyDeck, 5, "the endgame - an empty deck - was never reached")
    }

    /// THE FLIPPED TRUMP, NAMED. The deck badge's report came from one shape:
    /// a refill that hands out MORE cards than `deck_count` says are left,
    /// because the trump lying under the deck is dealt last and was never in
    /// that count. Undoing such a refill by putting every card back in the deck
    /// overshoots by one, and the badge opens a card too high.
    ///
    /// This finds that shape in real games and pins the freeze on it
    /// specifically - the sweep above would catch a regression, but only as one
    /// failure among whatever else broke, with nothing saying which rule died.
    func testARefillOffTheFlippedTrumpDoesNotPutAnExtraCardBackInTheDeck() async throws {
        var seen = 0
        for players in [2, 3] {
            try await sweepBubbles(players: players, games: 4) { before, events, after, label in
                // The tell: cards drawn exceed the deck they were drawn from.
                let drawn = events.filter { $0.kind == .refill || $0.kind == .deal }
                    .reduce(0) { $0 + $1.cards.count }
                guard drawn > 0, drawn > before.deckCount else { return }
                seen += 1
                let pre = AnimPlan(events, finalView: after).pre
                XCTAssertEqual(pre.deck, before.deckCount,
                               "\(label): \(drawn) cards drawn off a deck of \(before.deckCount)"
                               + " - the flipped trump was counted back into the deck")
            }
        }
        XCTAssertGreaterThan(seen, 3, "no game ever drew the flipped trump - nothing was tested")
    }

    /// THE INVARIANT THE ONE-UNDO WALK-BACK RESTS ON: a refill is never the
    /// first event of a bubble. It is always a bout end's consequence, so a
    /// pickup, a trash or a magic transition comes first - which is what lets
    /// the freeze anchor on the first event's own board and undo exactly one
    /// event, keeping the refill's broken deck arithmetic out of the answer
    /// entirely. If the kernel ever leads a stream with a refill, this fails
    /// here rather than as a one-card drift on somebody's screen.
    func testNoBubbleEverBeginsWithARefill() async throws {
        for players in [2, 3, 4] {
            try await sweepBubbles(players: players, games: 3) { _, events, _, label in
                XCTAssertNotEqual(events.first?.kind, .refill,
                                  "\(label): a stream led with a refill")
            }
        }
    }

    /// THE PER-STEP BOARDS ONLY GO FORWARD. Each event carries the board its
    /// flight settles to, and the badges are pinned to it as the flight lands.
    /// Across one move those boards must walk from the pre-move board to the
    /// post-move board and never back: a deck that grows mid-sequence is the
    /// "deck suddenly went to 5" report, and a hand that grows before its owner
    /// has been dealt anything is the "briefly bumped" one.
    func testEveryStepsBoardWalksForwardsOnly() async throws {
        for players in [2, 3, 4] {
            try await sweepBubbles(players: players, games: 3) { before, events, after, label in
                var deck = before.deckCount, discard = before.discardCount
                for ev in events {
                    guard let s = ev.state else { continue }
                    XCTAssertLessThanOrEqual(s.deckCount, deck,
                                             "\(label): the deck GREW during \(String(describing: ev.kind))")
                    XCTAssertGreaterThanOrEqual(s.discardCount, discard,
                                                "\(label): the discard SHRANK during \(String(describing: ev.kind))")
                    deck = s.deckCount; discard = s.discardCount
                }
                guard events.contains(where: { $0.state != nil }) else { return }
                XCTAssertEqual(deck, after.deckCount, "\(label): the last step is not the settled board")
                XCTAssertEqual(discard, after.discardCount, "\(label): the last step is not the settled board")
            }
        }
    }

    /// THE PLAN DESCRIBES THE STREAM IT WAS GIVEN, step for step. The board only
    /// reads `pre` today, but the rest of the plan crosses the same wire, and a
    /// step that misreports how many cards fly or which board it settles to is a
    /// freeze waiting to be wrong for the next consumer.
    ///
    /// The card COUNT is the load-bearing half: a spectator's refill arrives as
    /// card backs with no identities at all, and a wire that sent the identity
    /// count in place of the card count would say a two-card draw moves nothing.
    func testEveryStepCarriesItsOwnEventsCardsAndBoard() async throws {
        var redactedSteps = 0
        for players in [2, 3] {
            try await sweepBubbles(players: players, games: 3) { _, events, after, label in
                guard !events.isEmpty else { return }
                let plan = AnimPlan(events, finalView: after)
                XCTAssertEqual(plan.steps.count, events.count, "\(label): a step per event")
                guard plan.steps.count == events.count else { return }
                for (i, ev) in events.enumerated() {
                    let step = plan.steps[i]
                    XCTAssertEqual(step.cardCount, ev.cards.count,
                                   "\(label): step \(i) flies \(step.cardCount) of \(ev.cards.count) cards")
                    XCTAssertEqual(step.type, ev.type, "\(label): step \(i) type")
                    if ev.cards.contains(where: { $0 == nil || $0?.isHidden == true }) { redactedSteps += 1 }
                    guard let board = ev.state else { continue }
                    XCTAssertEqual(step.counts.deck, board.deckCount, "\(label): step \(i) deck")
                    XCTAssertEqual(step.counts.discard, board.discardCount, "\(label): step \(i) discard")
                    for p in board.players {
                        XCTAssertEqual(step.counts.hand[p.seat], p.handCount,
                                       "\(label): step \(i) hand[\(p.seat)]")
                    }
                }
            }
        }
        XCTAssertGreaterThan(redactedSteps, 10,
                             "no step carried redacted cards - the masked case was not tested")
    }

    /// The last step of a move must BE the board the move produced, hand counts
    /// included - that is the whole contract the badges rely on to hand back to
    /// the real view without a jump when the overrides are released.
    func testTheLastStepIsTheSettledBoard() async throws {
        for players in [2, 3, 4] {
            try await sweepBubbles(players: players, games: 3) { _, events, after, label in
                guard let last = events.last(where: { $0.state != nil })?.state else { return }
                XCTAssertEqual(Counts(deck: last.deckCount, discard: last.discardCount,
                                      hand: Dictionary(uniqueKeysWithValues:
                                        last.players.map { ($0.seat, $0.handCount) })),
                               Counts(after),
                               "\(label): releasing the overrides here would jump")
            }
        }
    }
}
