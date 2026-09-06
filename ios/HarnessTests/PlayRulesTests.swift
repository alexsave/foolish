// PlayRulesTests - the shared gesture-to-move resolver, the one piece both boards
// route every play through. It is the KERNEL's now (c/src/legal.c `play_*`,
// reached through PlayWire); this file is the characterization suite it was
// lifted against, unchanged in what it asserts.
//
// The menus are built by hand, because a menu is an INPUT to these rules - a
// board hands in the menu it was published, and the kernel re-derives nothing.
// `MoveWire.encode` is what lets a hand-built menu cross without this file
// growing a second copy of the wire layout.

import XCTest
import FoolishKit

final class PlayRulesTests: XCTestCase {
    // Handy card builders. suit 0=spades, 1=hearts, 2=clubs, 3=diamonds.
    private func c(_ s: Int, _ v: Int) -> Card { Card(s: s, v: v) }

    private func probe(_ cards: [Card], _ target: PlayTarget, defender: Bool,
                       battles: [BattleView] = [], legal: [Move], trump: Int = 3) -> PlayProbe {
        PlayWire.probe(menu: MoveWire.encode(legal), battles: battles, powerSuit: trump,
                       isDefender: defender, selection: cards, target: target)
    }

    func test_attacker_singleAttack() {
        let six = c(0, 6)
        let legal = [Move(type: .attack, cards: [six])]
        let p = probe([six], .table, defender: false, legal: legal)
        XCTAssertEqual(p.move?.type, .attack)
        XCTAssertEqual(p.move?.cards, [six])
        XCTAssertTrue(p.canAttack)
    }

    func test_attacker_multiAttackSameRank() {
        let a = [c(0, 6), c(1, 6)]
        let legal = [Move(type: .attack, cards: a)]
        // Order must not matter (selection is a set).
        let p = probe([c(1, 6), c(0, 6)], .table, defender: false, legal: legal)
        XCTAssertEqual(p.move?.type, .attack)
    }

    func test_defender_coverOntoBattle() {
        let attack = c(0, 9), cover = c(0, 13)   // A♠ covers 9♠
        let battles = [BattleView(attack: attack, defense: nil)]
        let legal = [Move(type: .cover, cards: [cover], attackCards: [attack])]
        let p = probe([cover], .battle(0), defender: true, battles: battles, legal: legal)
        XCTAssertEqual(p.move?.type, .cover)
        XCTAssertEqual(p.coverable, [0])
    }

    func test_defender_passOnEmptyTable() {
        let pass = c(1, 6)
        let legal = [Move(type: .pass, cards: [pass])]
        let p = probe([pass], .table, defender: true, legal: legal)
        XCTAssertEqual(p.move?.type, .pass)
        XCTAssertTrue(p.canPass)
    }

    func test_defender_autoUniqueCoverOnEmptyTable() {
        // Drop a card on empty space with no pass legal → auto-target the one cover.
        let attack = c(0, 9), cover = c(0, 13)
        let battles = [BattleView(attack: attack, defense: nil)]
        let legal = [Move(type: .cover, cards: [cover], attackCards: [attack])]
        XCTAssertEqual(probe([cover], .table, defender: true, battles: battles, legal: legal).move?.type,
                       .cover)
    }

    func test_noMatch_returnsNil() {
        let legal = [Move(type: .attack, cards: [c(0, 6)])]
        XCTAssertNil(probe([c(2, 10)], .table, defender: false, legal: legal).move)
        XCTAssertNil(probe([], .table, defender: false, legal: legal).move)
    }

    func test_controlMoves() {
        // Pickup and Good are read straight off the menu the board was handed;
        // only Good carries the extra UI rule (see below).
        let legal = [Move(type: .pickup), Move(type: .good)]
        XCTAssertTrue(legal.contains { $0.type == .pickup })
        XCTAssertTrue(legal.contains { $0.type == .good })
        XCTAssertFalse(legal.contains { $0.type == .attack })
    }

    func test_canSayGood_onlyWhenAllCovered() {
        let a = c(0, 9), d = c(0, 13)
        let legal = [Move(type: .good)]
        func good(_ battles: [BattleView]) -> Bool {
            probe([], .table, defender: false, battles: battles, legal: legal).canSayGood
        }
        // an uncovered attack on the table -> Good hidden
        XCTAssertFalse(good([BattleView(attack: a, defense: nil)]))
        // all covered -> Good shows
        XCTAssertTrue(good([BattleView(attack: a, defense: d)]))
        // empty table -> nothing to finish
        XCTAssertFalse(good([]))
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

    // MARK: the wire the menu crosses on

    /// MoveWire.encode exists so a hand-built menu can reach the kernel; it is
    /// only trustworthy if it is the exact inverse of the decoder every board
    /// already runs on the kernel's own bytes.
    func test_menuWireRoundTrips() {
        let moves = [Move(type: .attack, cards: [c(0, 6)]),
                     Move(type: .attack, cards: [c(0, 6), c(1, 6)]),
                     Move(type: .cover, cards: [c(2, 13)], attackCards: [c(1, 9)]),
                     Move(type: .pickup), Move(type: .good), Move(type: .wait)]
        let back = MoveWire.decode(MoveWire.encode(moves))
        XCTAssertEqual(back.map(\.type), moves.map(\.type))
        XCTAssertEqual(back[1].cards, moves[1].cards)
        XCTAssertEqual(back[2].attackCards, moves[2].attackCards)
        XCTAssertTrue(MoveWire.decode(MoveWire.emptyMenu).isEmpty)
    }
}
