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
}
