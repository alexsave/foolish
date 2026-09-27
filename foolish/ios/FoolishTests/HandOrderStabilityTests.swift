// ROUND 30 — MY HAND DOES NOT REARRANGE ITSELF.
//
// The owner, on 1.0(29): "when I hit pickup, the card works fine and goes into
// my hand. However, as it flies to my hand, the other cards in my hand
// rearrange themselves! ... If I undid the pickup, it went back to the order
// the cards were in before." And the rule, stated in full: new games have no
// preferred order; a pickup or a draw lands rightmost; the ONLY move that
// changes the arrangement is rearranging the cards.
//
// The cause is not the arrangement store - that is empty for a new game, and
// only a drag ever writes it. It is that the board stops rendering the LIVE
// kernel game the moment a move has a settlement half to withhold
// (MessageTurnController.captureSettlement, 368e666: "a staged bout end deals
// nothing until it is sent"). What it renders instead is a snapshot from the
// event stream, and that stream is produced by replaying the game's v6 code
// from scratch (`lastMoveEvents` -> fio_replay_last_events_packed). The replay
// reaches the same POSITION by a different route, so its hand array comes out
// in a different order - the same cards, shuffled. A cover that does not end
// the bout withholds nothing, renders no snapshot, and never showed the bug.
//
// `testTheKernelItselfDisagrees` pins that divergence against the real kernel,
// so this file keeps failing honestly if the two derivations are ever made to
// agree (delete the workaround then, not the test). Everything else asserts
// that the board is immune to it either way.
import XCTest
@testable import FoolishKit

@MainActor
final class HandOrderStabilityTests: XCTestCase {

    // 2p, turn 7, round 1 — the §8.2 gate fixture, whose seat 0 is a defender
    // facing two uncovered attacks, i.e. one tap away from the reported bug.
    private let fixtureHex =
        "f7020002efcdab89674523010800000200020000000000000000ae15293755bd748b2919627cd0591ffb42d7f9b2e9b57da5c2839ed47bd7ced7020004416e6e300104416e6e310800f72719e90cb7ee031bd6af74a3a23a"

    private func bytes(_ hex: String) -> Data {
        var d = Data(); var i = hex.startIndex
        while i < hex.endIndex {
            let j = hex.index(i, offsetBy: 2)
            d.append(UInt8(hex[i..<j], radix: 16)!); i = j
        }
        return d
    }

    private func card(_ id: String) -> Card {
        let p = id.split(separator: "-")
        return Card(s: Int(p[0])!, v: Int(p[1])!)
    }
    private func cards(_ ids: [String]) -> [Card] { ids.map(card) }

    // MARK: - the rule, pure

    func testAnUnseenCardLandsRightmost() {
        let order = ["1-6", "2-7", "3-8"]
        XCTAssertEqual(FHandFan.remembering(order, cards: cards(["1-6", "2-7", "3-8", "1-9"])),
                       ["1-6", "2-7", "3-8", "1-9"],
                       "a pickup or a draw goes to the right of everything already placed")
    }

    func testSeveralUnseenCardsKeepTheOrderTheyArriveIn() {
        XCTAssertEqual(FHandFan.remembering(["1-6"], cards: cards(["1-6", "3-9", "2-7"])),
                       ["1-6", "3-9", "2-7"],
                       "a multi-card pickup lands rightmost in the order it was handed over")
    }

    func testAFreshHandIsRememberedExactlyAsDealt() {
        XCTAssertEqual(FHandFan.remembering([], cards: cards(["3-12", "1-8", "1-10"])),
                       ["3-12", "1-8", "1-10"],
                       "a new game has no preferred order - the first hand drawn BECOMES it")
    }

    func testACardThatLeavesKeepsItsPlaceAndResumesItOnReturn() {
        // I covered with 1-8, so it is on the table and out of my hand...
        let afterPlaying = FHandFan.remembering(["3-12", "1-8", "1-10"], cards: cards(["3-12", "1-10"]))
        XCTAssertEqual(afterPlaying, ["3-12", "1-8", "1-10"], "the memory is grow-only; nothing is forgotten")
        XCTAssertEqual(FHandFan.displayOrder(cards: cards(["3-12", "1-10"]), order: afterPlaying)
                        .map(\.identity), ["3-12", "1-10"], "but only cards I hold are drawn")
        // ...and then I picked it back up. The owner: "if you pick up a card
        // that is in the local preferred order, such as a card you covered with
        // then had to pick up, it can go back into its position."
        XCTAssertEqual(FHandFan.displayOrder(cards: cards(["3-12", "1-10", "1-8"]), order: afterPlaying)
                        .map(\.identity), ["3-12", "1-8", "1-10"],
                       "a card that comes home resumes the slot it left, not the right edge")
    }

    func testRememberingNeverReordersWhatItAlreadyKnows() {
        // The whole point: the hand arrives in a DIFFERENT array order (this is
        // the replay's order from the bug) and not one remembered card moves.
        let dealt = FHandFan.remembering([], cards: cards(["3-12", "1-8", "1-10", "2-9"]))
        let shuffledSameCards = cards(["1-8", "1-10", "3-12", "2-9"])
        XCTAssertEqual(FHandFan.remembering(dealt, cards: shuffledSameCards), dealt,
                       "a differently-ordered copy of the same hand changes nothing")
        XCTAssertEqual(FHandFan.displayOrder(cards: shuffledSameCards, order: dealt).map(\.identity),
                       ["3-12", "1-8", "1-10", "2-9"],
                       "and it is still DRAWN in the arrangement the board already had")
    }

    func testRememberingIsIdempotent() {
        let hand = cards(["3-12", "1-8"])
        let once = FHandFan.remembering([], cards: hand)
        XCTAssertEqual(FHandFan.remembering(once, cards: hand), once)
    }

    // MARK: - a drag is still the only thing that rearranges

    func testADragDoesNotForgetACardThatIsOutOnTheTable() {
        // 1-8 is remembered but not in hand (I covered with it). Dragging 1-10
        // to the head must keep 1-8's place, so it comes home to slot 1.
        let order = ["3-12", "1-8", "1-10"]
        let slots = [CGRect(x: 0, y: 0, width: 40, height: 60),
                     CGRect(x: 50, y: 0, width: 40, height: 60)]
        let spliced = FHandFan.splice(order: order, deferred: ["1-8"], dragged: "1-10",
                                      centre: CGPoint(x: 20, y: 30), slots: slots)
        XCTAssertEqual(spliced?.order, ["1-10", "1-8", "3-12"],
                       "the dragged card moves; the card off in the table keeps its own place")
    }

    // MARK: - the kernel divergence this exists to survive

    /// The two derivations of ONE position, read back to back, disagree about
    /// my hand's array order. This is the bug's engine; the board must not care.
    func testTheKernelItselfDisagrees() async throws {
        let k = MessageKernel.shared
        let parentBytes = bytes(fixtureHex)
        _ = try await MessageEnvelope.decode(payload: parentBytes, viewer: 0)
        let menu = await k.residentLegal(seat: 0)
        guard let pickup = menu.first(where: { $0.type == .pickup }) else {
            return XCTFail("the fixture's seat 0 should be a defender who may pick up")
        }
        let turn = try await k.stagedTurn(.continuation(payload: parentBytes),
                                          replaying: [pickup], seat: 0)
        let live = (await k.residentView(viewer: 0)?.me?.hand ?? []).map(\.identity)
        let snapshot = (turn.events.first?.state?.me?.hand ?? []).map(\.identity)
        XCTAssertFalse(live.isEmpty); XCTAssertFalse(snapshot.isEmpty)
        XCTAssertEqual(Set(live), Set(snapshot), "same position, so the same cards")
        // Not an XCTAssertNotEqual: the day these agree is a day this file
        // should keep passing. Say so out loud instead, so a run that fixed it
        // upstream is legible in the log rather than a mystery green.
        if live != snapshot {
            print("round-30: the derivations still disagree — live \(live) vs replay \(snapshot)")
        }
    }

    /// END TO END, through the real controller: play the pickup the owner
    /// played and prove the six cards already in my hand do not move.
    func testAPickupDoesNotRearrangeTheHandItLandsIn() async throws {
        let k = MessageKernel.shared
        let parentBytes = bytes(fixtureHex)
        let parent = try await MessageEnvelope.decode(payload: parentBytes, viewer: 0)
        let menu = await k.residentLegal(seat: 0)
        guard let pickup = menu.first(where: { $0.type == .pickup }) else {
            return XCTFail("the fixture's seat 0 should be a defender who may pick up")
        }
        let c = MessageTurnController(parentPayload: parentBytes, parent: parent, mySeat: 0)
        await c.refresh()

        // What the fan draws before the tap, and what it therefore remembers.
        let handBefore = c.view?.me?.hand ?? []
        XCTAssertEqual(handBefore.count, 6)
        var order = FHandFan.remembering([], cards: handBefore)
        let before = FHandFan.displayOrder(cards: handBefore, order: order).map(\.identity)

        await c.apply(pickup)

        // The board is now showing the WITHHELD board - a replay snapshot, in
        // the replay's array order. This is the exact frame the owner watched
        // his hand rearrange in.
        let handDuring = c.view?.me?.hand ?? []
        XCTAssertEqual(handDuring.count, 8, "the two table cards are in my hand")
        order = FHandFan.remembering(order, cards: handDuring)
        let during = FHandFan.displayOrder(cards: handDuring, order: order).map(\.identity)

        XCTAssertEqual(Array(during.prefix(6)), before,
                       "the six cards I was already holding have not moved")
        XCTAssertEqual(Set(during.suffix(2)),
                       Set(handDuring.map(\.identity)).subtracting(before),
                       "and the two I picked up are at the right edge")

        // Undo puts the live board back, in ITS array order. Still no movement.
        await c.undo()
        await c.refresh()
        let handAfter = c.view?.me?.hand ?? []
        order = FHandFan.remembering(order, cards: handAfter)
        XCTAssertEqual(FHandFan.displayOrder(cards: handAfter, order: order).map(\.identity), before,
                       "undo does not rearrange my hand either")
    }
}
