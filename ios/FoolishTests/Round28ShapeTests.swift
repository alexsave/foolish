// Round28ShapeTests - the four shapes decided on the 1.0(28) walk of
// docs/ANIMATION_CATALOGUE.md, as rules that can be read without a board.
//
// Each of the four was a question the catalogue could not answer, and the
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
//   THE OUT BADGE (`outsWith`). `out` is a notice with no flight, so a badge
//   collapsed when the loop reaches it would collapse just after the move that
//   caused it. The owner wants it WITH the card motion.
//
//   GOODS CLEARED (`goodsCleared`). The mirror of `goodsOpening`, and the answer
//   its asymmetry had missed: not first, not last, but alongside.
import XCTest
@testable import FoolishKit

@MainActor
final class Round28ShapeTests: XCTestCase {

    private func c(_ suit: Int, _ v: Int) -> Card { Card(s: suit, v: v) }

    private func ev(_ kind: EventType, seat: Int = 1, cards: [Card?] = []) -> GameEvent {
        GameEvent(type: kind.rawValue, seat: seat, msg: 0, from: 1, to: 2,
                  cards: cards, target: nil, battle: nil, state: nil)
    }

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
        let groups = MessageTableView.parallelGroups([
            ev(.cover, seat: 1, cards: [c(0, 9)]),
            ev(.out, seat: 1),
            ev(.cardsToTrash, seat: 1),
        ])
        XCTAssertEqual(MessageTableView.outsWith(groups, 0), [1],
                       "the cover adopts the out that follows it")
    }

    /// The lookahead stops at the first group that moves a card. An out two
    /// beats later belongs to the move that caused IT, and a badge that
    /// collapsed early would be edge-on before its owner's last card had moved.
    func testTheLookaheadDoesNotReachPastAMoveThatMovesCards() {
        let groups = MessageTableView.parallelGroups([
            ev(.attackPass, seat: 0, cards: [c(1, 6)]),
            ev(.cover, seat: 1, cards: [c(1, 9)]),
            ev(.out, seat: 1),
        ])
        XCTAssertTrue(MessageTableView.outsWith(groups, 0).isEmpty,
                      "seat 1 goes out on the COVER, not on the attack before it")
        XCTAssertEqual(MessageTableView.outsWith(groups, 1), [1])
    }

    /// A pickup is card motion too - the player whose last cards were taken off
    /// the table goes out on the pickup that took them.
    func testAPickupAlsoCarriesTheOutThatFollowsIt() {
        let groups = MessageTableView.parallelGroups([
            ev(.pickup, seat: 2, cards: [c(0, 6), c(0, 9)]),
            ev(.out, seat: 0),
        ])
        XCTAssertEqual(MessageTableView.outsWith(groups, 0), [0])
    }

    /// Several seats going out on one move all collapse together, and the group
    /// carrying its OWN out (rather than a following notice) still reports it -
    /// that is the fallback the loop relies on when nothing preceded it.
    func testEveryOutOnOneMoveCollapsesTogetherAndAnOrphanOutStillCounts() {
        let many = MessageTableView.parallelGroups([
            ev(.cardsToTrash, seat: 1, cards: [c(0, 6)]),
            ev(.out, seat: 1),
            ev(.out, seat: 3),
        ])
        XCTAssertEqual(MessageTableView.outsWith(many, 0), [1, 3])
        // An `out` with no card motion in front of it: the group answers for
        // itself rather than reporting nothing and leaving a badge standing.
        let orphan = MessageTableView.parallelGroups([ev(.out, seat: 2)])
        XCTAssertEqual(MessageTableView.outsWith(orphan, 0), [2])
    }

    /// A stream with nobody going out collapses nothing. The obvious wrong
    /// implementation - "did the out set change" against a final view - answers
    /// yes here for every seat that was ALREADY out, which is the whole reason
    /// this reads events rather than diffing boards.
    func testAnOrdinaryStreamCollapsesNobody() {
        let groups = MessageTableView.parallelGroups([
            ev(.attackPass, seat: 0, cards: [c(1, 6)]),
            ev(.cover, seat: 1, cards: [c(1, 9)]),
            ev(.refill, seat: 0, cards: [c(2, 11)]),
        ])
        for i in 0..<groups.count {
            XCTAssertTrue(MessageTableView.outsWith(groups, i).isEmpty)
        }
    }

    // MARK: - goods cleared

    /// A throw-in that clears two goods turns both marks, and moves nobody.
    func testClearedGoodsTurnAndNothingChangesHands() {
        let shown = MessageTableView.RoleState(defender: 1, firstAttacker: 0, goodMask: 0b101)
        let cleared = MessageTableView.goodsCleared(shown: shown, stepGoodMask: 0)
        XCTAssertEqual(cleared?.goodMask, 0)
        XCTAssertEqual(cleared?.defender, 1, "clearing a good hands nothing over")
        XCTAssertEqual(cleared?.firstAttacker, 0)
    }

    /// Only the bits actually removed. A good that is still standing keeps its
    /// check, which is the case that separates this from "take the step's mask".
    func testAGoodThatStillStandsKeepsItsCheck() {
        let shown = MessageTableView.RoleState(defender: 1, firstAttacker: 0, goodMask: 0b101)
        let cleared = MessageTableView.goodsCleared(shown: shown, stepGoodMask: 0b100)
        XCTAssertEqual(cleared?.goodMask, 0b100, "seat 0's good cleared, seat 2's stands")
    }

    /// Nothing to turn: no goods, or a step that ADDS one. Adding is
    /// `goodsOpening`'s job and plays at the FRONT of the stream, so this
    /// returning a state for it would flip the same mark twice in one sequence.
    func testAddingAGoodIsNotThisRule() {
        let none = MessageTableView.RoleState(defender: 1, firstAttacker: 0, goodMask: 0)
        XCTAssertNil(MessageTableView.goodsCleared(shown: none, stepGoodMask: 0b100))
        XCTAssertNil(MessageTableView.goodsCleared(shown: none, stepGoodMask: 0))
        XCTAssertNil(MessageTableView.goodsCleared(shown: nil, stepGoodMask: 0))
        let some = MessageTableView.RoleState(defender: 1, firstAttacker: 0, goodMask: 0b001)
        XCTAssertNil(MessageTableView.goodsCleared(shown: some, stepGoodMask: 0b011),
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
                XCTAssertNil(MessageTableView.goodsCleared(shown: shown, stepGoodMask: mask),
                             "mask \(mask) only adds, so it clears nothing")
            }
            if onlyRemoves {
                XCTAssertNil(MessageTableView.goodsOpening(shown: shown, firstGoodMask: mask),
                             "mask \(mask) only removes, so it opens nothing")
            }
        }
    }
}
