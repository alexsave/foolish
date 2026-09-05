// RoundThirtySixTests.swift - the fixes from the owner's Eva test pass
// (evanotes.txt). One file, because the reports are one pass; each test names
// the sentence it answers.

import XCTest
import SwiftUI
@testable import FoolishKit

@MainActor
final class HandLandingAnchorTests: XCTestCase {

    /// THE VANISHING PICKUP. A flight into the hand is built in the same
    /// MainActor turn that opens the incoming cards' slots, so `handFrame` is
    /// still the box the hand had BEFORE the row split - and the fan is
    /// bottom-anchored, so a one-row box and a two-row box share a bottom edge
    /// and nothing else. Anchoring the analytical slot on `minY` therefore aimed
    /// every card a whole row too low.
    ///
    /// Owner: "the cards on the table kinda started flying a bit down, then
    /// literally just diappeared. Vanieshed. No idea where they went."
    func testALandingSlotIsAnchoredOnTheHandsBottomEdge() {
        let width: CGFloat = 398
        let cards = (0..<11).map { Card(s: $0 / 13, v: $0 % 13 + 1) }
        XCTAssertEqual(FHandFan.rowCount(cards: cards, availableWidth: width), 2,
                       "the fixture must actually split, or this asserts nothing")

        // The STALE frame: the one-row box the hand had before the pickup, sat
        // on the board's bottom edge at y = 500.
        let oneRow = FHandFan.height(count: 6, availableWidth: width)
        let stale = CGRect(x: 0, y: 500 - oneRow, width: width, height: oneRow)
        // The frame the hand will actually have once it has two rows. Same
        // bottom edge - that is the whole point.
        let twoRow = FHandFan.height(count: 11, availableWidth: width)
        let settled = CGRect(x: 0, y: 500 - twoRow, width: width, height: twoRow)
        XCTAssertEqual(stale.maxY, settled.maxY)
        XCTAssertGreaterThan(twoRow - oneRow, 80, "the two boxes really are a row apart")

        let slots = FHandFan.slotRects(cards: cards, width: width)
        for c in cards {
            let local = try! XCTUnwrap(slots[c.identity])
            let aimed = MessageTableView.inBoardSpace(local, laidOutCount: cards.count,
                                                      handFrame: stale)
            let truth = local.offsetBy(dx: settled.minX, dy: settled.minY)
            XCTAssertEqual(aimed.midX, truth.midX, accuracy: 0.01, "\(c.identity)")
            XCTAssertEqual(aimed.midY, truth.midY, accuracy: 0.01,
                           "\(c.identity) was not aimed at its settled slot")
            XCTAssertLessThanOrEqual(aimed.maxY, settled.maxY + 0.01,
                                     "\(c.identity) landed below the hand")
        }
    }

    /// …and the rule is a no-op when the frame is already right, which is every
    /// flight that does not change the row count. A fix that only worked in the
    /// broken case would be a second special case, not a rule.
    func testAnAlreadySettledFrameIsUnchanged() {
        let width: CGFloat = 340
        let cards = (0..<5).map { Card(s: 0, v: $0 + 2) }
        let h = FHandFan.height(count: cards.count, availableWidth: width)
        let frame = CGRect(x: 12, y: 400, width: width, height: h)
        let slots = FHandFan.slotRects(cards: cards, width: width)
        for c in cards {
            let local = try! XCTUnwrap(slots[c.identity])
            XCTAssertEqual(
                MessageTableView.inBoardSpace(local, laidOutCount: cards.count,
                                              handFrame: frame),
                local.offsetBy(dx: frame.minX, dy: frame.minY))
        }
    }
}

@MainActor
final class PreBoutTableTests: XCTestCase {

    private func card(_ s: Int, _ v: Int) -> Card { Card(s: s, v: v) }

    private func view(battles: [BattleView]) -> GameView {
        GameView(status: 1, numPlayers: 2, powerSuit: 3, deckCount: 10, discardCount: 0,
                 hasFlipped: true, firstAttacker: 0, defender: 1, viewer: 0,
                 goodMask: 0, gameOver: -1, flipped: card(3, 12), battles: battles,
                 eliminationOrder: [],
                 players: [PlayerView(seat: 0, name: "", status: 2, handCount: 6,
                                      awaitingAttack: false, strategyKey: 0, hand: nil),
                           PlayerView(seat: 1, name: "", status: 2, handCount: 6,
                                      awaitingAttack: false, strategyKey: 0, hand: nil)])
    }

    private func ev(_ kind: EventType, seat: Int, cards: [Card],
                    state: GameView? = nil) -> GameEvent {
        GameEvent(type: kind.rawValue, seat: seat, msg: 0, from: 1, to: 2,
                  cards: cards.map { Optional($0) }, target: nil, battle: nil, state: state)
    }

    private func pickup(_ cards: [Card]) -> GameEvent { ev(.pickup, seat: 1, cards: cards) }

    /// THE FLATTENED GRID. The kernel hands a pickup over as one flat list, and
    /// the fallback reading lays every card in its own uncovered cell - so a
    /// table that really held one covered pair and one bare attack comes back as
    /// THREE cells. `battlesArea` renders the sweep through the same FBattleGrid
    /// with the same identities, so a differently shaped table does not cut, it
    /// ANIMATES every card into its new cell first.
    ///
    /// Owner: "when the opponent picked up 6 of diamonds, k of diamonds, k of
    /// hearts, they did not animate directly from their table positions, but
    /// seemed to spread out to an evenly spaced row, AND THEN fly to the hand."
    func testAPickupTakesItsShapeFromTheRealPriorTable() {
        let sixD = card(3, 6), kingD = card(3, 13), kingH = card(1, 13)
        let real = [BattleView(attack: sixD, defense: kingH),
                    BattleView(attack: kingD, defense: nil)]
        let evs = [pickup([sixD, kingD, kingH])]

        // Without the prior board there is nothing better to say than the flat
        // reading - three cells, which is the shape that produced the report.
        XCTAssertEqual(MessageTurnController.preBoutTable(evs).count, 3)

        let out = MessageTurnController.preBoutTable(evs, prior: view(battles: real))
        XCTAssertEqual(out.count, 2, "the covered pair was split back apart")
        XCTAssertEqual(out.first?.defense, kingH, "…and the cover is still a cover")
    }

    /// A PRIOR BOARD FROM SOME OTHER MOMENT IS NOT THIS TABLE. The test is exact
    /// account, not overlap: guessing with a board that holds different cards
    /// would be worse than the flat reading, which is at least about the right
    /// cards.
    func testAPriorTableThatDoesNotAccountForThePickupIsRefused() {
        let sixD = card(3, 6), kingD = card(3, 13), kingH = card(1, 13)
        let evs = [pickup([sixD, kingD, kingH])]
        // One card short.
        let stale = [BattleView(attack: sixD, defense: kingH)]
        XCTAssertEqual(MessageTurnController.preBoutTable(evs, prior: view(battles: stale)).count, 3,
                       "a table missing a picked-up card must not be used")
        // One card too many.
        let extra = [BattleView(attack: sixD, defense: kingH),
                     BattleView(attack: kingD, defense: card(0, 7))]
        XCTAssertEqual(MessageTurnController.preBoutTable(evs, prior: view(battles: extra)).count, 3,
                       "a table holding a card the pickup does not take must not be used")
    }

    /// A multi-action turn carries the real table on its own earlier step, and
    /// that beats the prior board - it is nearer the pickup in time.
    func testAnEarlierStepInTheStreamWins() {
        let sixD = card(3, 6), kingD = card(3, 13), kingH = card(1, 13)
        let real = [BattleView(attack: sixD, defense: kingH),
                    BattleView(attack: kingD, defense: nil)]
        let evs = [ev(.attackPass, seat: 0, cards: [kingD], state: view(battles: real)),
                   pickup([sixD, kingD, kingH])]
        let out = MessageTurnController.preBoutTable(evs, prior: nil)
        XCTAssertEqual(out.count, 2)
        XCTAssertEqual(out.first?.defense, kingH)
    }

    /// The discard/trash side is untouched - it already walked back through the
    /// stream, and this round only taught the pickup to do the same.
    func testTheDiscardWalkIsUnchanged() {
        let sixD = card(3, 6), kingH = card(1, 13)
        let real = [BattleView(attack: sixD, defense: kingH)]
        let evs = [ev(.cover, seat: 1, cards: [kingH], state: view(battles: real)),
                   ev(.cardsToTrash, seat: -1, cards: [sixD, kingH], state: view(battles: []))]
        XCTAssertEqual(MessageTurnController.preBoutTable(evs), real)
    }
}

@MainActor
final class ShownTableTests: XCTestCase {

    private func b(_ v: Int) -> BattleView {
        BattleView(attack: Card(s: 0, v: v), defense: Card(s: 1, v: v))
    }

    /// THE BLINK. An arrival publishes its (already cleared) view a paint before
    /// anything sets the sweep, because the sweep is set inside the `onChange`
    /// that runs AFTER the body. For that paint the grid had nothing to draw, so
    /// the table was torn down and put straight back - four cards out and in
    /// again. The rig caught it as `cells=2` -> `cells=0` -> `cells=2`, and the
    /// owner as "super annoying glitch with ghost cards fading halfway in
    /// quickly and immediatley out".
    func testTheTableNeverGoesEmptyWhileABoutEndIsStillPending() {
        let pre = [b(9), b(10)]
        let t = MessageTableView.shownTable(live: [], sweep: [], pending: pre)
        XCTAssertEqual(t.shown, pre, "the table blinked empty between the arrival and its sweep")
        XCTAssertTrue(t.sweeping, "…and it is a sweep, so nothing on it is a drop target")
    }

    /// The live table always wins: a pending replay must never paint over a
    /// board that still has real cards on it.
    func testALiveTableOutranksBothReconstructions() {
        let live = [b(7)]
        let t = MessageTableView.shownTable(live: live, sweep: [b(9), b(10)], pending: [b(11)])
        XCTAssertEqual(t.shown, live)
        XCTAssertFalse(t.sweeping)
    }

    /// A sweep captured by MY OWN move outranks the open-replay reconstruction -
    /// it is the real prior view, and the reconstruction is at best a guess at
    /// the same thing (MessageTurnController.preBoutTable).
    func testMyOwnSweepOutranksThePendingReconstruction() {
        let sweep = [b(9), b(10)]
        let t = MessageTableView.shownTable(live: [], sweep: sweep, pending: [b(11)])
        XCTAssertEqual(t.shown, sweep)
        XCTAssertTrue(t.sweeping)
    }

    /// A settled empty table is still empty, and is NOT a sweep - otherwise the
    /// grid would refuse taps on a board that is simply waiting for a move.
    func testASettledEmptyTableIsNotASweep() {
        let t = MessageTableView.shownTable(live: [], sweep: [], pending: [])
        XCTAssertTrue(t.shown.isEmpty)
        XCTAssertFalse(t.sweeping)
    }
}
