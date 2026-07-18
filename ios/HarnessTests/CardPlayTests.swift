// CardPlayTests — the shared gesture→move resolver (CardPlay), the one piece both
// boards route every play through. Pure logic over a kernel legal-menu; no UI.

import XCTest
import FoolishKit

final class CardPlayTests: XCTestCase {
    // Handy card builders. suit 0=spades, 1=hearts, 2=clubs, 3=diamonds.
    private func c(_ s: Int, _ v: Int) -> Card { Card(s: s, v: v) }

    func test_attacker_singleAttack() {
        let six = c(0, 6)
        let legal = [Move(type: .attack, cards: [six])]
        let move = CardPlay.resolve(cards: [six], target: .table, isDefender: false,
                                    battles: [], legal: legal)
        XCTAssertEqual(move?.type, .attack)
        XCTAssertEqual(move?.cards, [six])
        XCTAssertTrue(CardPlay.canAttack([six], legal: legal))
    }

    func test_attacker_multiAttackSameRank() {
        let a = [c(0, 6), c(1, 6)]
        let legal = [Move(type: .attack, cards: a)]
        // Order must not matter (selection is a set).
        let move = CardPlay.resolve(cards: [c(1, 6), c(0, 6)], target: .table, isDefender: false,
                                    battles: [], legal: legal)
        XCTAssertEqual(move?.type, .attack)
    }

    func test_defender_coverOntoBattle() {
        let attack = c(0, 9), cover = c(0, 13)   // A♠ covers 9♠
        let battles = [BattleView(attack: attack, defense: nil)]
        let legal = [Move(type: .cover, cards: [cover], attackCards: [attack])]
        let move = CardPlay.resolve(cards: [cover], target: .battle(0), isDefender: true,
                                    battles: battles, legal: legal)
        XCTAssertEqual(move?.type, .cover)
        XCTAssertEqual(CardPlay.coverableBattles(cards: [cover], battles: battles, legal: legal), [0])
    }

    func test_defender_passOnEmptyTable() {
        let pass = c(1, 6)
        let legal = [Move(type: .pass, cards: [pass])]
        let move = CardPlay.resolve(cards: [pass], target: .table, isDefender: true,
                                    battles: [], legal: legal)
        XCTAssertEqual(move?.type, .pass)
        XCTAssertTrue(CardPlay.canPass([pass], legal: legal))
    }

    func test_defender_autoUniqueCoverOnEmptyTable() {
        // Drop a card on empty space with no pass legal → auto-target the one cover.
        let attack = c(0, 9), cover = c(0, 13)
        let battles = [BattleView(attack: attack, defense: nil)]
        let legal = [Move(type: .cover, cards: [cover], attackCards: [attack])]
        let move = CardPlay.resolve(cards: [cover], target: .table, isDefender: true,
                                    battles: battles, legal: legal)
        XCTAssertEqual(move?.type, .cover)
    }

    func test_noMatch_returnsNil() {
        let legal = [Move(type: .attack, cards: [c(0, 6)])]
        XCTAssertNil(CardPlay.resolve(cards: [c(2, 10)], target: .table, isDefender: false,
                                      battles: [], legal: legal))
        XCTAssertNil(CardPlay.resolve(cards: [], target: .table, isDefender: false,
                                      battles: [], legal: legal))
    }

    func test_controlMoves() {
        let legal = [Move(type: .pickup), Move(type: .good)]
        XCTAssertTrue(CardPlay.has(.pickup, in: legal))
        XCTAssertTrue(CardPlay.has(.good, in: legal))
        XCTAssertFalse(CardPlay.has(.attack, in: legal))
    }

    func test_canSayGood_onlyWhenAllCovered() {
        let a = c(0, 9), d = c(0, 13)
        let legal = [Move(type: .good)]
        // an uncovered attack on the table -> Good hidden
        XCTAssertFalse(CardPlay.canSayGood(battles: [BattleView(attack: a, defense: nil)], legal: legal))
        // all covered -> Good shows
        XCTAssertTrue(CardPlay.canSayGood(battles: [BattleView(attack: a, defense: d)], legal: legal))
        // empty table -> nothing to finish
        XCTAssertFalse(CardPlay.canSayGood(battles: [], legal: legal))
    }

    func test_boardDrop_target() {
        let battles = [0: CGRect(x: 100, y: 100, width: 60, height: 80),
                       1: CGRect(x: 200, y: 100, width: 60, height: 80)]
        let hand = CGRect(x: 0, y: 400, width: 375, height: 78)
        // over an attack slot -> cover that battle
        XCTAssertEqual(BoardDrop.target(at: CGPoint(x: 225, y: 130), battles: battles, handFrame: hand), .battle(1))
        // back in the hand -> cancel
        XCTAssertEqual(BoardDrop.target(at: CGPoint(x: 100, y: 420), battles: battles, handFrame: hand), .hand)
        // empty table -> attack/pass
        XCTAssertEqual(BoardDrop.target(at: CGPoint(x: 300, y: 300), battles: battles, handFrame: hand), .table)
    }
}
