// THE CROSSING, not the rules.
//
// The veil, the hand's layout, the table under a sweep, the end screen's order
// and the ledger's ownership are all the kernel's now (c/src/anim_plan.c), and
// the rules themselves are pinned in C - tests.c walks each one, and the whole
// deck through each one. What C cannot see is the SWIFT SIDE of the crossing:
// a `Card.identity` string turned into a dense id and back, an enum whose raw
// values have to be the kernel's constants, a packed answer read off a byte
// buffer. That is what this file is for.
//
// WHY IT IS A FILE OF ITS OWN. The characterization suites (VeilOutsTests,
// HoldbackTests, SequenceTeardownTests, ReplayTakeoffTests, RoundThirtySixTests,
// UnstartedReplayTests, CountOwnershipTests) keep asserting the BEHAVIOUR, with
// the same expectations they had when the rules were Swift. They pass whether
// the answer came from Swift or from C, which is exactly what makes them the
// characterization suite - and exactly why they cannot catch a dense-id
// off-by-one, because both sides of every one of their comparisons goes through
// the same encoder.
//
// MUTATION-CHECKED, each applied and put back, with the FoolishTests run
// asserted to have actually happened - the first attempt reported "no failures"
// that were the kernel-freshness guard refusing to build, which is the trap the
// campaign has now hit twice.
//
//   S1  CardSet.id drops the (1...13) guard      -> traps in
//       testACardOutsideTheDeckIsNotRepresentable (an id off the end of the
//       identity table), taking the suite down at 583 of 597
//   S2  CardSet.id computes s * 13 + v           -> 96 failures across 12 suites
//   S3  CardSet.identities admits bit 52         -> traps, same shape as S1
//   S4  ShownClaim.bystander renumbered to 4     -> 3 failures, one of them
//       CountOwnershipTests', which is the suite that owns the rule
//   S5  FinishOrder reads the row count as total -> 2
//   S6  FinishOrder stops refusing an off-roster seat -> 1
//   S7  PreBoutTable.wire spells an unnameable card as the empty cell -> 2, one
//       of them MessageBoutEndHoldTests' real closing-cover stream
//   S8  HandLayout.fanCards regrows its empty-holdback fast path -> 1 (and it
//       SURVIVED until the test below was written, because nothing else in the
//       suite asks the fan about a card the kernel cannot name)
//   S9  FHandFan.displayOrder keeps its own Swift reconcile -> 8

import XCTest
@testable import FoolishKit

final class BoardWireTests: XCTestCase {

    // MARK: 1 - the card-id crossing, all 52 cards, both directions

    /// EVERY CARD OF THE DECK, THERE AND BACK. A dense-id off-by-one is
    /// invisible through almost the whole deck because both sides of a
    /// comparison go through the same encoder; it bites at the LAST card, where
    /// the set comes back empty and every rule silently answers "nothing".
    func testEveryCardOfTheDeckSurvivesTheCrossing() {
        for suit in 0..<4 {
            for value in 1...13 {
                let card = Card(s: suit, v: value)
                XCTAssertEqual(CardSet.id(of: card), UInt8(suit * 13 + value - 1),
                               "\(card.identity) crossed as the wrong id")
                XCTAssertEqual(CardSet.card(UInt8(suit * 13 + value - 1)), card,
                               "\(card.identity) came back as a different card")
                XCTAssertEqual(CardSet.byIdentity[card.identity], UInt8(suit * 13 + value - 1))
                XCTAssertEqual(CardSet.identities[suit * 13 + value - 1], card.identity)
            }
        }
        XCTAssertEqual(CardSet.identities.count, 52, "a deck is 52 cards, no more and no fewer")
        XCTAssertEqual(Set(CardSet.identities).count, 52, "and no identity is spent twice")
    }

    /// …and through a RULE, so the crossing is exercised the way the board
    /// exercises it. The ace of the last suit is the one that would go missing.
    func testTheLastCardOfTheDeckBehavesLikeTheFirst() {
        for card in [Card(s: 0, v: 1), Card(s: 3, v: 13)] {
            let ids: Set<String> = [card.identity]
            XCTAssertEqual(Veil.veiled(hidden: ids, pendingOpen: nil,
                                       handBeforeMyMove: nil, myHand: nil), ids)
            XCTAssertEqual(Veil.flying(hidden: ids, preHidden: []), ids)
            XCTAssertEqual(Veil.fan(veiled: ids, holdback: [card]), [])
            XCTAssertEqual(Veil.selectionAfterTap([], card: card, hand: [card]), ids)
            XCTAssertEqual(HandLayout.fanCards([], holding: [card]), [card])
            XCTAssertEqual(PreBoutTable.cardIds([BattleView(attack: card, defense: nil)]), ids)
        }
    }

    /// A CARD THAT IS NOT A CARD HAS NO BIT, and the board never has one: a
    /// masked back renders as a back and is dropped at the source
    /// (`openReplayTouchedCardIds`), and the kernel's values are 1...13. Pinned
    /// because it is the one thing the bitset representation cannot express, so
    /// it must be a decision rather than a surprise.
    func testACardOutsideTheDeckIsNotRepresentable() {
        XCTAssertNil(CardSet.id(of: Card.hidden), "a masked back has no identity to veil")
        XCTAssertNil(CardSet.id(of: Card(s: 3, v: 14)), "and neither does a value no deck holds")
        XCTAssertNil(CardSet.id(of: Card(s: 4, v: 6)), "nor a suit no deck holds")
        XCTAssertNil(CardSet.id(of: Card(s: 0, v: 0)))
        XCTAssertTrue(Veil.veiled(hidden: ["not-a-card"], pendingOpen: nil,
                                  handBeforeMyMove: nil, myHand: nil).isEmpty,
                      "an identity the deck does not hold names nothing")
        // …and back the other way. Nothing the crossing builds ever sets a bit
        // above 51, but the bound is what stands between a corrupt answer and an
        // index out of range, so it is asserted rather than assumed.
        XCTAssertTrue(CardSet.identities(1 << 52).isEmpty, "a bit off the deck names no card")
        XCTAssertEqual(CardSet.identities(~UInt64(0)).count, 52, "and a full word names the deck")
    }

    // MARK: 2 - the ledger's claim codes

    /// The claim enum lives with its callers and the RULE lives in the kernel,
    /// so the two are joined by nothing but these four numbers. If they drift,
    /// every bystander write is silently judged as somebody else.
    ///
    /// MUTANT: renumber `.bystander` and this fails, along with the ownership
    /// assertion below.
    func testTheClaimCodesAreTheKernelsOwn() {
        XCTAssertEqual(ShownClaim.sequence.rawValue, ShownWrite.sequence)
        XCTAssertEqual(ShownClaim.arming.rawValue, ShownWrite.arming)
        XCTAssertEqual(ShownClaim.handOff.rawValue, ShownWrite.handOff)
        XCTAssertEqual(ShownClaim.bystander.rawValue, ShownWrite.bystander)
    }

    /// …and the rule read through them is the one the board has always kept.
    func testOnlyABystanderEverStandsDown() {
        for claim in [ShownClaim.sequence, .arming, .handOff] {
            XCTAssertTrue(ShownLedger.allows(claim, sequencing: true))
            XCTAssertTrue(ShownLedger.allows(claim, sequencing: false))
        }
        XCTAssertFalse(ShownLedger.allows(.bystander, sequencing: true))
        XCTAssertTrue(ShownLedger.allows(.bystander, sequencing: false))
    }

    // MARK: 3 - the end screen's order

    private func view(eliminated: [Int], fool: Int, seats: Int) -> GameView {
        GameView(status: 2, numPlayers: seats, powerSuit: 0, deckCount: 0, discardCount: 0,
                 hasFlipped: false, firstAttacker: 0, defender: 1, viewer: 0, goodMask: 0,
                 gameOver: fool, flipped: nil, battles: [], eliminationOrder: eliminated,
                 players: (0..<seats).map {
                     PlayerView(seat: $0, name: "P\($0)", status: 0, handCount: 0,
                                awaitingAttack: false, strategyKey: 0, hand: nil)
                 })
    }

    /// Rank 1 is the first player out; the fool takes the LAST place, which is
    /// the seat count and not the row count.
    func testTheFinishOrderRunsFirstOutToTheFool() {
        let rows = FinishOrder.places(view(eliminated: [2, 0, 3], fool: 1, seats: 4), mySeat: 0)
        XCTAssertEqual(rows.map(\.place), [1, 2, 3, 4])
        XCTAssertEqual(rows.map(\.seat), [2, 0, 3, 1])
        XCTAssertEqual(rows.map(\.isYou), [false, true, false, false])
        XCTAssertEqual(rows.map(\.total), [4, 4, 4, 4], "`total` is the seat count on every row")
        XCTAssertEqual(rows.map(\.isFool), [false, false, false, true])
    }

    /// A spectator holds no seat and owns no row - the kernel spends -1 on "no
    /// particular player" too, so this has to be asked rather than assumed.
    func testASpectatorOwnsNoRow() {
        let rows = FinishOrder.places(view(eliminated: [2, 0, 3], fool: 1, seats: 4), mySeat: -1)
        XCTAssertEqual(rows.count, 4)
        XCTAssertTrue(rows.allSatisfy { !$0.isYou })
    }

    /// A game still running ranks only who is out, and an untouched one nobody.
    ///
    /// This is also where `total` is pinned against the ROW count, which the
    /// four-of-four case above cannot tell apart: one row on a four-seat table,
    /// and a row is the fool at place 4 and not at place 1.
    func testAGameWithNoFoolYetRanksOnlyWhoIsOut() {
        let rows = FinishOrder.places(view(eliminated: [2], fool: -1, seats: 4), mySeat: 0)
        XCTAssertEqual(rows.map(\.place), [1])
        XCTAssertEqual(rows.map(\.total), [4], "`total` is the seat count, not the row count")
        XCTAssertFalse(rows[0].isFool, "first out is the opposite of the fool")
        XCTAssertTrue(FinishOrder.places(view(eliminated: [], fool: -1, seats: 4),
                                         mySeat: 0).isEmpty)
    }

    /// A seat off the roster is refused OUTRIGHT rather than skipped. Skipping
    /// would shift every rank below it by one, which is a wrong answer where an
    /// empty screen is an obvious one.
    func testAnImpossibleSeatEmptiesTheScreenRatherThanShiftingIt() {
        XCTAssertTrue(FinishOrder.places(view(eliminated: [2, 99], fool: 1, seats: 4),
                                         mySeat: 0).isEmpty)
    }

    // MARK: 4 - a table cell nobody can name

    /// A CARD THAT IS THERE AND CANNOT BE SPOKEN ABOUT is not the same as an
    /// empty cell, and spelling it as one is how a table that is really losing a
    /// card gets accepted as one that only adds a cover. It crosses as its own
    /// byte and the kernel refuses to certify the swap over it.
    ///
    /// MUTANT: emit the empty-cell byte for an unnameable card and both of these
    /// fail - the stranger vanishes and the swap is granted.
    func testATableHoldingACardNobodyCanNameIsNeverAccountedFor() {
        let six = Card(s: 0, v: 6), nine = Card(s: 1, v: 9)
        let stranger = Card(s: 9, v: 9)
        let real = [BattleView(attack: six, defense: nine)]
        XCTAssertTrue(PreBoutTable.covers(real, [BattleView(attack: six, defense: nil)]),
                      "a table that adds the cover accounts for the one without it")
        XCTAssertFalse(PreBoutTable.covers(real, [BattleView(attack: stranger, defense: nil)]),
                       "a card the kernel cannot name is never accounted for")
        XCTAssertFalse(PreBoutTable.covers(real, [BattleView(attack: six, defense: stranger)]),
                       "…on either side of a battle")
    }

    // MARK: 5 - one derivation for where my cards sit

    /// `FHandFan.displayOrder` and `HandLayout.laidOut` are the SAME rule, one
    /// with nothing deferred. Two readings of "where do my cards sit" is round
    /// 12's deal landing in the wrong slot, so this pins that there is one.
    ///
    /// MUTANT: give `displayOrder` back its own Swift reconcile and the deferred
    /// case below diverges.
    func testTheFanAndTheBoardAgreeOnWhereMyCardsSit() {
        let hand = [Card(s: 0, v: 6), Card(s: 1, v: 9), Card(s: 3, v: 13)]
        let order = hand.reversed().map(\.identity)
        XCTAssertEqual(FHandFan.displayOrder(cards: hand, order: order),
                       HandLayout.laidOut(hand: hand, deferred: [], order: order))
        XCTAssertEqual(FHandFan.displayOrder(cards: hand, order: order), hand.reversed())
        // A deferred card is out of the laid-out hand, arrangement or no
        // arrangement - the half `displayOrder` alone cannot express.
        XCTAssertEqual(HandLayout.laidOut(hand: hand, deferred: [hand[1].identity],
                                          order: order).map(\.identity),
                       [hand[2].identity, hand[0].identity])
    }

    /// ONE RULE, NOT TWO. `fanCards` used to short-circuit an empty holdback and
    /// hand the array straight back, which answered a card the kernel cannot name
    /// differently depending on whether anything was held. The board never has
    /// such a card, but a function with two rules is a function whose tests only
    /// exercise one of them.
    ///
    /// MUTANT: restore the `guard !holdback.isEmpty else { return hand }` and the
    /// first of these fails.
    func testTheFanAnswersACardItCannotNameTheSameWayEitherWay() {
        let six = Card(s: 0, v: 6), nine = Card(s: 1, v: 9)
        let ghost = Card(s: 3, v: 14)
        XCTAssertEqual(HandLayout.fanCards([six, ghost], holding: []), [six])
        XCTAssertEqual(HandLayout.fanCards([six, ghost], holding: [nine]), [six, nine])
    }

    /// A duplicate identity in the hand collapses instead of crashing. The old
    /// Swift reconcile built a `Dictionary(uniqueKeysWithValues:)` over the
    /// hand, which traps on a repeat; the kernel drops it, and a fan that draws
    /// one card is a better failure than a fan that kills the extension.
    func testARepeatedCardCollapsesRatherThanTrapping() {
        let six = Card(s: 0, v: 6)
        XCTAssertEqual(FHandFan.displayOrder(cards: [six, six], order: []), [six])
        XCTAssertEqual(HandLayout.fanCards([six, six], holding: [six]).count, 2,
                       "the fan keeps the hand it was given and adds nothing it already has")
    }
}
