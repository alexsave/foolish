// Round28ShapeTests - the five shapes decided on the 1.0(28) walk of
// docs/ANIMATION_CATALOGUE.md, as rules that can be read without a board.
//
// Each of the five was a question the catalogue could not answer, and the
// answers are load-bearing now rather than incidental - which is the whole
// reason they get a test apiece. What they are NOT is a test of the flights
// themselves: a rule here says which cards pair with which slot, which seats
// collapse with which beat, and which marks turn when. Whether the pixels then
// move is `runEventStream`'s job and the rig's.
//
//   THE GAME-OVER HOLD (`gameOverHold`). Was a bare 500ms inside settleResults;
//   the owner asked for one second, and it is expressed against `flightTime` so
//   a filmed game-over keeps its proportions under HARNESS_SLOWMO.
//
//   UNDOING A PICKUP (`undoReleaseTargets`). The one retraction that had no
//   animation at all - a whole table's worth of cards moving between two frames.
//
//   THE OUT BADGE (`AnimBeats.Beat.outs`). `out` is a notice with no flight, so a badge
//   collapsed when the loop reaches it would collapse just after the move that
//   caused it. The owner wants it WITH the card motion.
//
//   GOODS CLEARED (`RoleBeat.goodsCleared`). The mirror of `goodsOpening`, and the answer
//   its asymmetry had missed: not first, not last, but alongside.
//
//   THE PASS / TRANSFER (`RoleBeat.passHandOff`), the fifth and the last to be built.
//   Four things in one beat, and the only one of the four that TRAVELS is the
//   shield - so the rule's whole job is to say when the defence changed hands
//   and where it went, against a kernel stream that does not say either.
import XCTest
@testable import FoolishKit

@MainActor
final class Round28ShapeTests: XCTestCase {

    private func c(_ suit: Int, _ v: Int) -> Card { Card(s: suit, v: v) }

    private func ev(_ kind: EventType, seat: Int = 1, cards: [Card?] = []) -> GameEvent {
        GameEvent(type: kind.rawValue, seat: seat, msg: 0, from: 1, to: 2,
                  cards: cards, target: nil, battle: nil, state: nil)
    }

    /// The kernel's shape for a stream, and its first beat - what
    /// `runEventStream` hands each rule below.
    private func plan(_ events: [GameEvent]) -> AnimBeats { AnimBeats(events) }
    private func beat(_ events: [GameEvent]) -> AnimBeats.Beat { AnimBeats(events).beats[0] }

    // MARK: - the game-over hold

    /// One second at the shipping flight time, which is what was asked for.
    func testTheGameOverHoldIsASecond() {
        // `flightTime` is 0.5 unless a dev slowmo is set; the suite runs without
        // one, so this is the shipping value.
        XCTAssertEqual(flightTime, 0.5, accuracy: 0.0001, "the shipping flight time moved")
        XCTAssertEqual(gameOverHold, 1.0, accuracy: 0.0001)
    }

    /// And it is a MULTIPLE of a flight, not a constant that happens to equal
    /// one second - so a slowed-down film keeps the hold in proportion instead
    /// of watching it shrink to nothing as the flights around it stretch. This
    /// is the half of the change a bare `== 1.0` would pass right through.
    func testTheGameOverHoldScalesWithTheFlights() {
        XCTAssertEqual(gameOverHold / flightTime, 2.0, accuracy: 0.0001)
        // Longer than the settle it replaced, and shorter than the bout-end hold
        // - the last board of a game earns a longer look than a plain beat and a
        // shorter one than the bout the whole table is still reading.
        XCTAssertGreaterThan(gameOverHold, 0.5)
        XCTAssertLessThan(gameOverHold, boutEndHold)
    }

    // MARK: - undoing a pickup

    /// Each card that left my hand pairs with the battle it is going back to.
    func testEachCardGoesBackToItsOwnBattle() {
        let a1 = c(0, 6), d1 = c(0, 9), a2 = c(2, 7)
        let battles = [BattleView(attack: a1, defense: d1), BattleView(attack: a2, defense: nil)]
        let pairs = MessageTableView.undoReleaseTargets([a1, d1, a2], in: battles)
        XCTAssertEqual(pairs.count, 3)
        // The pairing is by CARD, not by order: the defence of battle 0 must go
        // back to battle 0, not to the second slot because it was second in hand.
        XCTAssertEqual(pairs.first { $0.0 == d1 }?.1, 0, "the cover goes back onto the attack it covered")
        XCTAssertEqual(pairs.first { $0.0 == a1 }?.1, 0)
        XCTAssertEqual(pairs.first { $0.0 == a2 }?.1, 1)
    }

    /// A card that is not on the restored table is DROPPED, not flown somewhere
    /// arbitrary. This is what lets the board call this for every undo and read
    /// an empty result as "not this shape" - the alternative is a card sailing
    /// to whichever slot happened to be first.
    func testACardThatIsNotOnTheTableIsDropped() {
        let onTable = c(0, 6), inHand = c(3, 14)
        let battles = [BattleView(attack: onTable, defense: nil)]
        let pairs = MessageTableView.undoReleaseTargets([onTable, inHand], in: battles)
        XCTAssertEqual(pairs.count, 1)
        XCTAssertEqual(pairs.first?.0, onTable)
        XCTAssertTrue(MessageTableView.undoReleaseTargets([inHand], in: battles).isEmpty,
                      "nothing of mine is going back to the table, so this is not a pickup undo")
        XCTAssertTrue(MessageTableView.undoReleaseTargets([], in: battles).isEmpty)
    }

    // MARK: - the out badge

    /// THE CASE: a cover empties the defender's hand, so the kernel says `out`
    /// right after it. The badge must collapse WITH the cover, not after it.
    func testABadgeCollapsesWithTheMoveThatEndsThePlayer() {
        let groups = plan([
            ev(.cover, seat: 1, cards: [c(0, 9)]),
            ev(.out, seat: 1),
            ev(.cardsToTrash, seat: 1),
        ])
        XCTAssertEqual(groups.beats[0].outs, [1],
                       "the cover adopts the out that follows it")
    }

    /// The lookahead stops at the first group that moves a card. An out two
    /// beats later belongs to the move that caused IT, and a badge that
    /// collapsed early would be edge-on before its owner's last card had moved.
    func testTheLookaheadDoesNotReachPastAMoveThatMovesCards() {
        let groups = plan([
            ev(.attackPass, seat: 0, cards: [c(1, 6)]),
            ev(.cover, seat: 1, cards: [c(1, 9)]),
            ev(.out, seat: 1),
        ])
        XCTAssertTrue(groups.beats[0].outs.isEmpty,
                      "seat 1 goes out on the COVER, not on the attack before it")
        XCTAssertEqual(groups.beats[1].outs, [1])
    }

    /// A pickup is card motion too - the player whose last cards were taken off
    /// the table goes out on the pickup that took them.
    func testAPickupAlsoCarriesTheOutThatFollowsIt() {
        let groups = plan([
            ev(.pickup, seat: 2, cards: [c(0, 6), c(0, 9)]),
            ev(.out, seat: 0),
        ])
        XCTAssertEqual(groups.beats[0].outs, [0])
    }

    /// Several seats going out on one move all collapse together, and the group
    /// carrying its OWN out (rather than a following notice) still reports it -
    /// that is the fallback the loop relies on when nothing preceded it.
    func testEveryOutOnOneMoveCollapsesTogetherAndAnOrphanOutStillCounts() {
        let many = plan([
            ev(.cardsToTrash, seat: 1, cards: [c(0, 6)]),
            ev(.out, seat: 1),
            ev(.out, seat: 3),
        ])
        XCTAssertEqual(many.beats[0].outs, [1, 3])
        // An `out` with no card motion in front of it: the group answers for
        // itself rather than reporting nothing and leaving a badge standing.
        let orphan = plan([ev(.out, seat: 2)])
        XCTAssertEqual(orphan.beats[0].outs, [2])
    }

    /// A stream with nobody going out collapses nothing. The obvious wrong
    /// implementation - "did the out set change" against a final view - answers
    /// yes here for every seat that was ALREADY out, which is the whole reason
    /// this reads events rather than diffing boards.
    func testAnOrdinaryStreamCollapsesNobody() {
        let groups = plan([
            ev(.attackPass, seat: 0, cards: [c(1, 6)]),
            ev(.cover, seat: 1, cards: [c(1, 9)]),
            ev(.refill, seat: 0, cards: [c(2, 11)]),
        ])
        for b in groups.beats { XCTAssertTrue(b.outs.isEmpty) }
    }

    // MARK: - goods cleared

    /// A throw-in that clears two goods turns both marks, and moves nobody.
    func testClearedGoodsTurnAndNothingChangesHands() {
        let shown = MessageTableView.RoleState(defender: 1, firstAttacker: 0, goodMask: 0b101)
        let cleared = RoleBeat.goodsCleared(shown: shown, stepGoodMask: 0)
        XCTAssertEqual(cleared?.goodMask, 0)
        XCTAssertEqual(cleared?.defender, 1, "clearing a good hands nothing over")
        XCTAssertEqual(cleared?.firstAttacker, 0)
    }

    /// Only the bits actually removed. A good that is still standing keeps its
    /// check, which is the case that separates this from "take the step's mask".
    func testAGoodThatStillStandsKeepsItsCheck() {
        let shown = MessageTableView.RoleState(defender: 1, firstAttacker: 0, goodMask: 0b101)
        let cleared = RoleBeat.goodsCleared(shown: shown, stepGoodMask: 0b100)
        XCTAssertEqual(cleared?.goodMask, 0b100, "seat 0's good cleared, seat 2's stands")
    }

    /// Nothing to turn: no goods, or a step that ADDS one. Adding is
    /// `goodsOpening`'s job and plays at the FRONT of the stream, so this
    /// returning a state for it would flip the same mark twice in one sequence.
    func testAddingAGoodIsNotThisRule() {
        let none = MessageTableView.RoleState(defender: 1, firstAttacker: 0, goodMask: 0)
        XCTAssertNil(RoleBeat.goodsCleared(shown: none, stepGoodMask: 0b100))
        XCTAssertNil(RoleBeat.goodsCleared(shown: none, stepGoodMask: 0))
        XCTAssertNil(RoleBeat.goodsCleared(shown: nil, stepGoodMask: 0))
        let some = MessageTableView.RoleState(defender: 1, firstAttacker: 0, goodMask: 0b001)
        XCTAssertNil(RoleBeat.goodsCleared(shown: some, stepGoodMask: 0b011),
                     "one good added while another stands clears nothing")
    }

    /// The two rules read the same mask in opposite directions, and each is
    /// deaf to the other's case: a mask that only gains bits clears nothing, a
    /// mask that only loses bits opens nothing. That is what keeps one mark from
    /// being turned twice in a sequence - once at the front and once alongside.
    ///
    /// Not asserted as "never both", because one bubble CAN carry a good being
    /// set and a later throw-in clearing it, and both firing is then correct:
    /// they belong to two different steps of the same stream.
    func testEachRuleIsDeafToTheOthersCase() {
        let shown = MessageTableView.RoleState(defender: 1, firstAttacker: 0, goodMask: 0b010)
        for mask in 0..<8 {
            let onlyAdds = (shown.goodMask & ~mask) == 0
            let onlyRemoves = (mask & ~shown.goodMask) == 0
            if onlyAdds {
                XCTAssertNil(RoleBeat.goodsCleared(shown: shown, stepGoodMask: mask),
                             "mask \(mask) only adds, so it clears nothing")
            }
            if onlyRemoves {
                XCTAssertNil(RoleBeat.goodsOpening(shown: shown, firstGoodMask: mask),
                             "mask \(mask) only removes, so it opens nothing")
            }
        }
    }

    // MARK: - what the beat carries back

    /// THE CROSSING, which is the half a C case cannot pin: the packed answer
    /// has to arrive in Swift as the same beats. A rule proved in
    /// c/tests/tests.c and then read out of the wrong byte here is a rule
    /// nothing catches, so these assert the fields the board actually consumes.
    ///
    /// The cards a beat PUTS DOWN, first. Only attacks, passes and covers land
    /// on the table; a pickup takes cards off it, and the sweep is drawn from
    /// this set.
    func testABeatNamesTheCardsItPutsOnTheTable() {
        let p = plan([ev(.attackPass, seat: 0, cards: [c(1, 6)]),
                      ev(.cover, seat: 1, cards: [c(1, 9)]),
                      ev(.pickup, seat: 1, cards: [c(2, 7)])])
        XCTAssertEqual(p.placed, [c(1, 6).identity, c(1, 9).identity],
                       "the whole stream's table placements")
        XCTAssertEqual(p.beats[0].placed, [c(1, 6).identity])
        XCTAssertEqual(p.beats[1].placed, [c(1, 9).identity])
        XCTAssertTrue(p.beats[2].placed.isEmpty, "a pickup takes cards OFF the table")
        XCTAssertTrue(p.beats[0].placedAny)
        XCTAssertFalse(p.beats[2].placedAny)
        // A multi-card cover is one beat and names both of its cards.
        let two = plan([ev(.cover, seat: 1, cards: [c(0, 9)]),
                        ev(.cover, seat: 1, cards: [c(3, 11)])])
        XCTAssertEqual(two.beats.count, 1)
        XCTAssertEqual(two.beats[0].placed, [c(0, 9).identity, c(3, 11).identity])
    }

    // MARK: - the pass / transfer

    /// THE CASE, and the whole shape in one assertion: seat 1 is defending,
    /// lays cards on the table, and the defence has moved on to seat 2 by the
    /// time the bubble closes. That is a transfer, and the shield goes with the
    /// card.
    ///
    /// The seats it does NOT touch are half the test. A pass never moves the
    /// opening sword (`handle_pass` does not touch `first_attacker`), so a rule
    /// that took the bubble's final board wholesale would hand that sword over
    /// with the transfer card whenever the same bubble also ended a bout.
    func testATransferHandsTheShieldToTheNextDefender() {
        let shown = MessageTableView.RoleState(defender: 1, firstAttacker: 0, goodMask: 0b100)
        let group = [ev(.attackPass, seat: 1, cards: [c(0, 6)])]
        let handOff = RoleBeat.passHandOff(shown: shown, beat: beat(group),
                                           finalDefender: 2)
        XCTAssertEqual(handOff?.defender, 2)
        XCTAssertEqual(handOff?.firstAttacker, 0, "a transfer does not move the opening sword")
        // The goods a transfer clears are `goodsCleared`'s business, fired in
        // this same beat off the step's own mask. Clearing them here as well
        // would turn one mark twice in one tick, from two different sources.
        XCTAssertEqual(handOff?.goodMask, 0b100, "the transfer cleared a good behind goodsCleared's back")
    }

    /// AN ORDINARY ATTACK IS THE SAME EVENT TYPE, and must hand nothing over.
    ///
    /// `EVW_T_ATTACK_PASS` carries both moves; the wire tells them apart only by
    /// a message template the board never renders. So the rule leans on the
    /// rules instead - a defender may not attack (`handle_attack` rejects
    /// `player_idx == g->defender`) - and the seat is the whole discriminator.
    /// Without it, an attack thrown in during a bubble that ALSO transferred
    /// would fly a second shield from a seat that never held one.
    func testAnAttackByAnybodyElseIsNotATransfer() {
        let shown = MessageTableView.RoleState(defender: 1, firstAttacker: 0)
        // Seat 0 attacking while seat 1 defends: a throw-in, not a transfer -
        // even though the defence really does move on later in the same bubble.
        XCTAssertNil(RoleBeat.passHandOff(
            shown: shown, beat: beat([ev(.attackPass, seat: 0, cards: [c(0, 6)])]),
            finalDefender: 2))
        // …and no other step is a transfer either, whoever made it.
        for kind: EventType in [.cover, .pickup, .refill, .discard, .out, .defenderMove] {
            XCTAssertNil(RoleBeat.passHandOff(
                shown: shown, beat: beat([ev(kind, seat: 1, cards: [c(0, 6)])]),
                finalDefender: 2), "\(kind) handed the shield over")
        }
    }

    /// NOTHING TO HAND OVER: the defence did not move, or it has already been
    /// handed over. The second is what lets `runEventStream` fire this rule per
    /// group AND keep its closing role beat - by the time that beat runs,
    /// `roleShown` already names the new defender, so it finds nothing and the
    /// shield cannot fly twice for one pass.
    func testAShieldAlreadyHandedOverDoesNotFlyAgain() {
        let shown = MessageTableView.RoleState(defender: 1, firstAttacker: 0)
        let group = [ev(.attackPass, seat: 1, cards: [c(0, 6)])]
        XCTAssertNil(RoleBeat.passHandOff(shown: shown, beat: beat(group),
                                          finalDefender: 1),
                     "the defence did not move, so this was an attack after all")
        let after = MessageTableView.RoleState(defender: 2, firstAttacker: 0)
        XCTAssertNil(RoleBeat.passHandOff(shown: after, beat: beat(group),
                                          finalDefender: 2),
                     "the hand-off replayed itself on a second look")
        // A board with no marks yet has nothing to hand over FROM.
        XCTAssertNil(RoleBeat.passHandOff(shown: nil, beat: beat(group), finalDefender: 2))
    }

    /// AND EXACTLY ONE MARK TRAVELS. The owner: "shield should always fly, my
    /// sword should rotate in, and their next sword should rotate out" - three
    /// marks change, one of them goes somewhere. The two swords are gestures
    /// made in place (`roleFlights`' standing rule, played by `FRoleCoin` off
    /// the departing / arriving seats), so a second flight here would be the
    /// board claiming somebody handed a sword over during a transfer.
    func testATransferThrowsTheShieldAndNothingElse() throws {
        // The OPENER gets a pad too, and it is the pad that gives this test its
        // teeth: `roleFlights` withholds a flight whose take-off pad has not
        // published, so a rule that handed the opening sword over as well would
        // pass a two-seat table by simply having nowhere to fly it from.
        let pads = [0: CGRect(x: 60, y: 280, width: 40, height: 40),
                    1: CGRect(x: 300, y: 280, width: 40, height: 40),
                    2: CGRect(x: 160, y: 70, width: 40, height: 40)]
        let shown = MessageTableView.RoleState(defender: 1, firstAttacker: 0)
        let handOff = try XCTUnwrap(RoleBeat.passHandOff(
            shown: shown, beat: beat([ev(.attackPass, seat: 1, cards: [c(0, 6)])]),
            finalDefender: 2))
        let flights = MessageTableView.roleFlights(from: shown, to: handOff, pads: pads)
        XCTAssertEqual(flights.count, 1, "a transfer throws exactly one mark")
        XCTAssertEqual(flights.first?.kind, .shield)
        XCTAssertEqual(flights.first?.fromSeat, 1)
        XCTAssertEqual(flights.first?.toSeat, 2)
    }

    /// THE REAL KERNEL, and the reason this rule takes the new defender as an
    /// argument instead of reading it off the step like every other rule here.
    ///
    /// A pass is snapshotted BEFORE the hand-over (`SNAP(ENGINE_HOOK_PASS)`,
    /// then `g->defender = next`) and emits no DEFENDER_MOVE step of its own,
    /// so the transfer's own board still shows the passer defending. An
    /// implementation that read `group.last?.state?.defender` - which is what
    /// `goodsCleared` and the out collapse both legitimately do - would find nothing
    /// changed and animate nothing at all, and it would be green against every
    /// synthetic event above because those carry whatever state the test wrote.
    /// So this plays a real transfer and asserts the trap is there.
    func testARealKernelTransferSnapshotsTheOldDefenderAndStillHandsOver() async throws {
        guard let found = try await findTransfer() else {
            throw XCTSkip("no 3p game in 40 reached a legal transfer")
        }
        let pass = try XCTUnwrap(AnimBeats(found.events).beats
                                    .first { $0.attackPassSeats != 0 },
                                 "the kernel's stream for a pass has no transfer step in it")
        let passEvents = Array(found.events[pass.range])

        // THE TRAP, stated as an assertion: the step's own board is one
        // hand-over behind the board the bubble carries.
        XCTAssertEqual(passEvents.last?.state?.defender, found.before.defender,
                       "the kernel started snapshotting the pass AFTER the hand-over - "
                       + "this rule could read the step directly now")
        XCTAssertNotEqual(found.after.defender, found.before.defender)

        // …and the board each beat settles to came across with it. This is what
        // `goodsCleared` is asked against inside `runEventStream`, so a step's
        // own good mask never reaching the kernel would leave every cleared
        // check waiting for the closing beat again.
        let shape = AnimBeats(found.events)
        XCTAssertEqual(shape.firstGoodMask, found.events.first?.state?.goodMask,
                       "the stream opens on its first step's board")
        XCTAssertEqual(pass.goodMask, passEvents.last?.state?.goodMask,
                       "a beat settles to its LAST step's board")

        let shown = MessageTableView.RoleState(found.before)
        let handOff = try XCTUnwrap(RoleBeat.passHandOff(
            shown: shown, beat: pass, finalDefender: found.after.defender))
        XCTAssertEqual(handOff.defender, found.after.defender)
        XCTAssertEqual(handOff.firstAttacker, found.before.firstAttacker,
                       "the opening sword moved on a transfer")
        // …and the same stream read the way the trap would read it says nothing.
        XCTAssertNil(RoleBeat.passHandOff(
            shown: shown, beat: pass,
            finalDefender: passEvents.last?.state?.defender ?? -1))
    }

    /// A real transfer, and the two boards either side of it: what the passer
    /// was looking at, and what the bubble they sealed carries.
    private struct RealTransfer {
        let events: [GameEvent]
        let before: GameView
        let after: GameView
    }

    private func freshSeed(_ salt: UInt8) -> Data {
        Data((0..<32).map { UInt8(truncatingIfNeeded: $0 &* 17 &+ Int(salt)) | 1 })
    }

    /// Drive real games until a defender is offered a transfer, then take it.
    /// Three seats, so the shield lands on somebody who is neither the passer
    /// nor the opener - the shape a 2p game cannot pose.
    ///
    /// Every other seat plays the first thing on its menu EXCEPT a pass, so the
    /// warm-up can never spend the move being hunted for.
    private func findTransfer(players n: Int = 3) async throws -> RealTransfer? {
        let k = MessageKernel.shared
        for salt: UInt8 in 1...40 {
            try await k.newGame(seed: freshSeed(salt), players: n)
            for _ in 0..<200 {
                guard let view = await k.residentView(viewer: -1), !view.isOver else { break }
                let defence = await k.residentLegal(seat: view.defender)
                if let pass = defence.first(where: { $0.type == .pass }) {
                    try await k.apply(seat: view.defender, move: pass)
                    guard let after = await k.residentView(viewer: -1) else { return nil }
                    return RealTransfer(events: await k.lastMoveEvents(viewer: view.defender),
                                        before: view, after: after)
                }
                var acted = false
                for seat in 0..<n {
                    let legal = await k.residentLegal(seat: seat)
                    guard let m = legal.first(where: { $0.type != .wait && $0.type != .pass })
                    else { continue }
                    try await k.apply(seat: seat, move: m)
                    acted = true
                    break
                }
                if !acted { break }
            }
        }
        return nil
    }
}
