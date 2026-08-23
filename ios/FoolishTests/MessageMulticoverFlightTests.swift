// A multicover's cards fly to the attacks they actually cover (round 16).
//
// Owner: "I've seen a multi card cover have all three cards animate towards a
// single attack card. On the sender side."
//
// The sender has two landing paths. The ordinary one (`placementFlights`) has
// always resolved a battle PER CARD off the settled view. The other is note
// 17's: a cover that empties the defender's hand ends the bout in the SAME
// kernel apply, so the board never renders a covered table and has to fly the
// landing from a snapshot taken before the apply. That snapshot held ONE rect -
// the slot the gesture named - and every card of the cover flew at it, fanned
// by a few points. With one card that is exactly right, which is why it stood;
// with three it is the bug, and the endgame is where multicovers live (the
// Cover button plays the kernel's greedy full-cover move, and a defender
// covering the table with their last cards is what ends the bout).
//
// So what these pin is the PAIRING: a cover names its targets positionally
// (`cards[i]` answers `attackCards[i]`, the shape PackedAction writes to the
// wire), and each card's flight must end on its own target's slot.
import XCTest
@testable import FoolishKit

@MainActor
final class MessageMulticoverFlightTests: XCTestCase {

    // MARK: fixture

    private func c(_ s: Int, _ v: Int) -> Card { Card(s: s, v: v) }

    /// Three uncovered attacks, laid out left to right the way FBattleGrid lays
    /// them - so "all three landed on one slot" and "each landed on its own"
    /// are visibly different answers, not two spellings of the same rect.
    private let slots: [Int: CGRect] = [
        0: CGRect(x: 100, y: 200, width: 62, height: 84),
        1: CGRect(x: 170, y: 200, width: 62, height: 84),
        2: CGRect(x: 240, y: 200, width: 62, height: 84),
    ]

    private var battles: [BattleView] {
        [BattleView(attack: c(0, 5), defense: nil),
         BattleView(attack: c(1, 6), defense: nil),
         BattleView(attack: c(2, 7), defense: nil)]
    }

    /// A three-card cover answering all three, DELIBERATELY not in table order:
    /// the pairing is by card, not by position on the table, and a builder that
    /// walked the battles instead of the move would still pass on a sorted one.
    private var multicover: Move {
        Move(type: .cover,
             cards:       [c(2, 10), c(0, 8), c(1, 9)],
             attackCards: [c(2, 7),  c(0, 5), c(1, 6)])
    }

    private var handRects: [String: CGRect] {
        [c(0, 8).identity:  CGRect(x: 40,  y: 600, width: 50, height: 70),
         c(1, 9).identity:  CGRect(x: 100, y: 600, width: 50, height: 70),
         c(2, 10).identity: CGRect(x: 160, y: 600, width: 50, height: 70)]
    }

    private func flights(_ move: Move, frames: [Int: CGRect]? = nil,
                         fallback: [Int: CGRect] = [:]) -> [Flight] {
        let landing = MessageTableView.coverLandingRects(
            move: move, battles: battles, frames: frames ?? slots, fallback: fallback)
        return MessageTableView.coverLandingFlights(
            cards: move.cards, landing: landing, fromRects: handRects)
    }

    // MARK: the bug

    /// THE ONE. Three cards, three destinations, each its own attack's slot.
    func testEachCardOfAMulticoverLandsOnTheAttackItCovers() {
        let out = flights(multicover)
        XCTAssertEqual(out.count, 3, "a card of the cover was dropped")

        let byCard = Dictionary(uniqueKeysWithValues: out.map { ($0.card!.identity, $0.to) })
        XCTAssertEqual(byCard[c(2, 10).identity], slots[2])
        XCTAssertEqual(byCard[c(0, 8).identity],  slots[0])
        XCTAssertEqual(byCard[c(1, 9).identity],  slots[1])

        // Said again as the shape the owner described, so the failure reads as
        // the report does: three cards, one destination.
        XCTAssertEqual(Set(out.map(\.to)).count, 3,
                       "the cards of a multicover all fly at the same attack")
    }

    /// Each card leaves from its OWN hand slot, not a shared one - the takeoff
    /// half of the same pairing.
    func testEachCardLeavesFromItsOwnHandSlot() {
        for f in flights(multicover) {
            XCTAssertEqual(f.from, handRects[f.card!.identity])
        }
        XCTAssertEqual(Set(flights(multicover).map(\.from)).count, 3)
    }

    /// A cover lies across, so its ghost rotates into the tilt over the flight
    /// rather than arriving flat and snapping when the real card replaces it -
    /// the same treatment `placementFlights` and `openReplayFlights` give the
    /// cover flights they build.
    func testACoverFliesInAlreadyRotating() {
        for f in flights(multicover) {
            XCTAssertEqual(f.angle, FBattleGrid.coverAngle, accuracy: 0.0001)
            XCTAssertEqual(f.fromAngle, 0, "it leaves the hand flat")
        }
    }

    // MARK: the single-card case, which was already right

    /// One card, one slot, no offset - the case that made the single-rect
    /// version look correct for as long as it did. It must stay pixel-identical.
    func testASingleCardCoverIsUntouched() {
        let one = Move(type: .cover, cards: [c(1, 9)], attackCards: [c(1, 6)])
        let out = flights(one)
        XCTAssertEqual(out.count, 1)
        XCTAssertEqual(out[0].to, slots[1], "a lone cover must land dead on its slot")
        XCTAssertEqual(out[0].from, handRects[c(1, 9).identity])
        XCTAssertEqual(out[0].id, "coverland-\(c(1, 9).identity)", "the flight id is the handoff key")
    }

    // MARK: the frames it reads

    /// The live measurement wins; the last non-empty one stands in for a slot
    /// that has not published yet. Same pair the single-rect version read.
    func testAnUnpublishedSlotFallsBackToTheLastMeasurement() {
        let partial: [Int: CGRect] = [0: slots[0]!]
        let out = flights(multicover, frames: partial, fallback: slots)
        XCTAssertEqual(out.count, 3)
        let byCard = Dictionary(uniqueKeysWithValues: out.map { ($0.card!.identity, $0.to) })
        XCTAssertEqual(byCard[c(1, 9).identity], slots[1])
        XCTAssertEqual(byCard[c(2, 10).identity], slots[2])
    }

    /// A card whose battle cannot be located at all is DROPPED, not sent
    /// somewhere plausible. The sweep then carries it off from rest, which is a
    /// missing flight; a defaulted rect would be a card flying to the wrong
    /// place, which reads as a different card entirely.
    func testACardWithNoMeasuredSlotIsDroppedNotGuessed() {
        let out = flights(multicover, frames: [1: slots[1]!])
        XCTAssertEqual(out.map { $0.card!.identity }, [c(1, 9).identity])
    }

    // MARK: what is not a cover

    func testOnlyACoverGetsCoverLandings() {
        let attack = Move(type: .attack, cards: [c(0, 8)], attackCards: nil)
        XCTAssertTrue(flights(attack).isEmpty)
    }

    /// A malformed pairing produces NOTHING rather than a partial guess: the
    /// positional pairing is the whole premise, so if the counts disagree there
    /// is no basis to pair anything at all.
    func testAMismatchedPairingIsRefusedWholesale() {
        let bad = Move(type: .cover, cards: [c(0, 8), c(1, 9)], attackCards: [c(0, 5)])
        XCTAssertTrue(flights(bad).isEmpty)
    }

    // MARK: against the real kernel

    /// The kernel really does hand the defender one move that covers several
    /// attacks (`calc_cover_moves_greedy`), and its pairing really is
    /// positional - so the deal-driven case lands the same way the fixture
    /// does. Without this the tests above would only prove the builder is
    /// self-consistent with a Move I wrote myself.
    func testARealKernelMulticoverAlsoSpreads() async throws {
        let k = MessageKernel.shared
        var found: (move: Move, battles: [BattleView])?
        for salt: UInt8 in 1...80 {
            let seed = Data((0..<32).map { UInt8(truncatingIfNeeded: $0 &* 17 &+ Int(salt)) | 1 })
            try await k.newGame(seed: seed, players: 2)
            guard let view = await k.residentView(viewer: -1) else { continue }
            let atk = view.defender == 0 ? 1 : 0
            let legal = await k.residentLegal(seat: atk)
            guard let pair = legal.first(where: { $0.type == .attack && $0.cards.count >= 2 })
            else { continue }
            try await k.apply(seat: atk, move: pair)
            guard let after = await k.residentView(viewer: -1) else { continue }
            let covers = await k.residentLegal(seat: after.defender)
            if let multi = covers.first(where: { $0.type == .cover && $0.cards.count >= 2 }) {
                found = (multi, after.battles); break
            }
        }
        guard let (move, table) = found else {
            XCTFail("no deal in 80 offered a multi-card cover"); return
        }
        XCTAssertEqual(move.attackCards?.count, move.cards.count,
                       "the kernel's cover pairs one target per card")

        // Measure the real table's slots, one rect per battle, and fly it.
        var frames: [Int: CGRect] = [:]
        for i in table.indices { frames[i] = CGRect(x: 100 + 70 * i, y: 200, width: 62, height: 84) }
        var from: [String: CGRect] = [:]
        for (i, card) in move.cards.enumerated() {
            from[card.identity] = CGRect(x: 40 + 60 * i, y: 600, width: 50, height: 70)
        }
        let landing = MessageTableView.coverLandingRects(move: move, battles: table,
                                                         frames: frames, fallback: [:])
        let out = MessageTableView.coverLandingFlights(cards: move.cards, landing: landing,
                                                       fromRects: from)
        XCTAssertEqual(out.count, move.cards.count, "a card of the kernel's cover was dropped")
        XCTAssertEqual(Set(out.map(\.to)).count, move.cards.count,
                       "the kernel's multicover flew every card at one attack")
        // …and each one is genuinely the slot of the attack the kernel paired it
        // with, not merely a distinct rect.
        for (card, attack) in zip(move.cards, move.attackCards ?? []) {
            let idx = table.firstIndex { $0.attack == attack }
            XCTAssertEqual(out.first { $0.card == card }?.to, frames[idx ?? -1])
        }
    }
}
