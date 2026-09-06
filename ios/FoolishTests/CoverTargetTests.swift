// CoverTargetTests.swift - round 16: "when we tap a card then tap cover, if it
// is ambiguous as to which card will be covered, go ahead and cover the highest
// value card that can be covered by that card. Trump is higher than non trump.
// If there are multiple highest value cards that can be covered, just choose
// one. And this doesn't apply to drag cover."
//
// What is under test is a CHOICE, not a rule: the kernel's
// `play_best_cover_target` picks one entry out of the kernel's own legal menu,
// so the menu is this file's input (it crosses as MoveWire bytes) and the
// legality of a cover stays where it lives. Every case therefore asserts on the
// CARD that ends up covered rather than on an index - an index only means
// something relative to a table order, and "which attack did it choose" is the
// whole question.

import XCTest
@testable import FoolishKit

final class CoverTargetTests: XCTestCase {

    // Kernel rank values: 5='6' … 10='J', 11='Q', 12='K', 13='A'.
    private func c(_ suit: Suit, _ v: Int) -> Card { Card(s: suit.rawValue, v: v) }

    /// A table of uncovered attacks, plus the menu the kernel would enumerate
    /// for a one-card selection that can cover exactly `coverable` of them.
    private func table(_ attacks: [Card], with card: Card, coverable: [Card])
        -> (battles: [BattleView], legal: [Move]) {
        (attacks.map { BattleView(attack: $0, defense: nil) },
         coverable.map { Move(type: .cover, cards: [card], attackCards: [$0]) })
    }

    /// One kernel answer about a selection on a table.
    private func probe(_ cards: [Card], _ t: (battles: [BattleView], legal: [Move]),
                       trump: Suit?, target: PlayTarget = .table) -> PlayProbe {
        PlayWire.probe(menu: MoveWire.encode(t.legal), battles: t.battles,
                       powerSuit: trump?.rawValue ?? -1, isDefender: true,
                       selection: cards, target: target)
    }

    /// Which card the cover button would actually put the selection on.
    private func covered(_ card: Card, _ attacks: [Card], coverable: [Card],
                         trump: Suit?) -> Card? {
        let t = table(attacks, with: card, coverable: coverable)
        guard let i = probe([card], t, trump: trump).bestCover else { return nil }
        return t.battles[i].attack
    }

    // MARK: the rule

    func testItCoversTheHighestValueAttackItCanBeat() {
        let ace = c(.clubs, 13)
        let attacks = [c(.spades, 5), c(.hearts, 12), c(.diamonds, 8)]   // 6, K, 9
        XCTAssertEqual(covered(ace, attacks, coverable: attacks, trump: .clubs),
                       c(.hearts, 12), "did not spend the ace on the king")
    }

    /// The old behaviour, so the change is pinned as a change: the first
    /// coverable INDEX is the leftmost attack, which here is the smallest card.
    func testItIsNoLongerJustTheLeftmostAttack() {
        let ace = c(.clubs, 13)
        let attacks = [c(.spades, 5), c(.hearts, 12), c(.diamonds, 8)]
        let t = table(attacks, with: ace, coverable: attacks)
        let leftmost = probe([ace], t, trump: .clubs).coverable.sorted().first
        XCTAssertEqual(leftmost, 0, "fixture: the leftmost coverable is not index 0")
        XCTAssertNotEqual(probe([ace], t, trump: .clubs).bestCover, leftmost)
    }

    /// "Trump is higher than non trump" - a six of trumps outranks an ace.
    func testATrumpOutranksABiggerNonTrump() {
        let trumpAce = c(.spades, 13)
        let attacks = [c(.hearts, 13), c(.spades, 5)]     // A♥ (13), 6♠ (5, trump)
        XCTAssertEqual(covered(trumpAce, attacks, coverable: attacks, trump: .spades),
                       c(.spades, 5), "an ace off-suit beat a trump six")
    }

    /// …and the trump rule is about the TRUMP SUIT, not about a suit that
    /// happens to be listed first: the same table with a different trump picks
    /// the other card.
    func testTheTrumpRuleFollowsTheTrumpSuit() {
        let card = c(.clubs, 13)
        let attacks = [c(.hearts, 13), c(.spades, 5)]
        XCTAssertEqual(covered(card, attacks, coverable: attacks, trump: .spades), c(.spades, 5))
        XCTAssertEqual(covered(card, attacks, coverable: attacks, trump: .clubs), c(.hearts, 13))
    }

    /// Only what this selection can ACTUALLY beat is in the running - the menu
    /// is the authority, so a bigger attack that is not on it is ignored.
    func testTheBiggestAttackIsIgnoredIfThisCardCannotBeatIt() {
        let nine = c(.clubs, 8)
        let attacks = [c(.hearts, 6), c(.spades, 13), c(.hearts, 7)]   // 7, A♠, 8
        XCTAssertEqual(covered(nine, attacks, coverable: [attacks[0], attacks[2]], trump: .diamonds),
                       c(.hearts, 7), "chose an attack that was not on the legal menu")
    }

    func testNothingCoverableChoosesNothing() {
        let six = c(.hearts, 5)
        let attacks = [c(.spades, 13)]
        XCTAssertNil(covered(six, attacks, coverable: [], trump: .spades))
    }

    // MARK: "just choose one"

    /// A tie (same value, neither trump) resolves to the leftmost, and keeps
    /// resolving there - the owner allows any of them, but a choice that moved
    /// between two identical taps would read as a bug.
    func testATieAlwaysChoosesTheSameOne() {
        let card = c(.spades, 13)
        let attacks = [c(.hearts, 9), c(.clubs, 9), c(.diamonds, 9)]
        let t = table(attacks, with: card, coverable: attacks)
        let first = probe([card], t, trump: .spades).bestCover
        XCTAssertEqual(first, 0)
        for _ in 0..<20 {
            XCTAssertEqual(probe([card], t, trump: .spades).bestCover, first,
                           "the same table chose a different attack on a second tap")
        }
    }

    /// The choice must not depend on the order the attackers happened to throw
    /// in: every permutation of one table covers the SAME CARD.
    func testTableOrderDoesNotChangeWhichCardIsCovered() {
        let ace = c(.clubs, 13)
        let attacks = [c(.spades, 5), c(.hearts, 12), c(.diamonds, 8)]
        for perm in [[0, 1, 2], [2, 1, 0], [1, 0, 2], [2, 0, 1], [0, 2, 1], [1, 2, 0]] {
            let order = perm.map { attacks[$0] }
            XCTAssertEqual(covered(ace, order, coverable: order, trump: .clubs), c(.hearts, 12),
                           "order \(perm) covered something else")
        }
    }

    // MARK: it has to be playable

    /// Whatever it picks has to survive the resolver the tap then runs it
    /// through - a policy that named an index no legal cover matches would
    /// reject the move instead of playing it.
    func testTheChosenTargetResolvesToALegalCover() throws {
        let ace = c(.clubs, 13)
        let attacks = [c(.spades, 5), c(.hearts, 12), c(.diamonds, 8)]
        let t = table(attacks, with: ace, coverable: attacks)
        let i = try XCTUnwrap(probe([ace], t, trump: .clubs).bestCover)
        let move = probe([ace], t, trump: .clubs, target: .battle(i)).move
        XCTAssertEqual(move?.type, .cover)
        XCTAssertEqual(move?.attackCards, [c(.hearts, 12)])
    }

    /// A multi-card cover is chosen the same way, and still resolves: the menu
    /// entry that covers the strongest attack is the one that gets played.
    func testAMultiCardCoverPicksTheSameWay() throws {
        let cards = [c(.clubs, 13), c(.clubs, 12)]
        let attacks = [c(.hearts, 5), c(.hearts, 11)]
        let battles = attacks.map { BattleView(attack: $0, defense: nil) }
        // One legal entry per pairing, as the kernel enumerates them.
        let legal = [Move(type: .cover, cards: cards, attackCards: [attacks[0]]),
                     Move(type: .cover, cards: cards, attackCards: [attacks[1]])]
        let t = (battles: battles, legal: legal)
        let i = try XCTUnwrap(probe(cards, t, trump: .spades).bestCover)
        XCTAssertEqual(battles[i].attack, c(.hearts, 11), "did not aim at the jack")
        // The menu names ONE attack for a two-card cover, so the wire pads the
        // second slot with the no-card sentinel - which must never read back as
        // a real attack. The jack is what comes home; the pad is a hidden card.
        let played = probe(cards, t, trump: .spades, target: .battle(i)).move
        XCTAssertEqual(played?.attackCards?.first, c(.hearts, 11))
        XCTAssertEqual(played?.attackCards?.dropFirst().first, Card.hidden)
    }

    // MARK: what must NOT change

    /// The DRAG path is untouched: dropping on a named battle still covers THAT
    /// battle, even when a bigger one is coverable beside it.
    func testDraggingOntoABattleStillCoversThatBattle() {
        let ace = c(.clubs, 13)
        let attacks = [c(.spades, 5), c(.hearts, 12)]
        let t = table(attacks, with: ace, coverable: attacks)
        let move = probe([ace], t, trump: .clubs, target: .battle(0)).move
        XCTAssertEqual(move?.attackCards, [c(.spades, 5)],
                       "a drag onto the six was redirected to the king")
    }

    /// And the highlight still offers every coverable attack - the button
    /// chooses, it does not narrow what the player may aim at.
    func testEveryCoverableAttackIsStillOffered() {
        let ace = c(.clubs, 13)
        let attacks = [c(.spades, 5), c(.hearts, 12), c(.diamonds, 8)]
        let t = table(attacks, with: ace, coverable: attacks)
        XCTAssertEqual(probe([ace], t, trump: .clubs).coverable, [0, 1, 2])
    }
}
