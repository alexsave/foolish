// BridgeKernelTests - the seam's kernel maps real bridge states into the
// model the screens draw.
//
// Two phones on one kernel, the way cn_twophone_test.c plays them: each
// phone is its own defaults (seat records and nickname) and identity, and
// switching phone builds a BridgeKernel on that phone's store, which hands
// the kernel that phone's records. Every link is the kernel's own
// (cn_api_text), sent and adopted as Messages would carry it.
//
// THE ORACLE IS THIS FILE'S: the dice are read once per round through the
// tests-only CN_API_ALL view, and from them this file works out which dice
// count, who loses and what the outcome line says. The model must agree.

import CChuiniu
import XCTest
@testable import ChuiniuKit

@MainActor
final class BridgeKernelTests: XCTestCase {
    private var stores: [String: UserDefaults] = [:]

    private func phone(_ name: String) -> BridgeKernel {
        let suite = "chuiniu.tests.\(name).\(ObjectIdentifier(self).hashValue)"
        let store = stores[name] ?? {
            let d = UserDefaults(suiteName: suite)!
            d.removePersistentDomain(forName: suite)
            stores[name] = d
            return d
        }()
        let k = BridgeKernel(store: store, devPerson: false)
        k.me(Data(repeating: UInt8(name.utf8.first!), count: 16))
        k.nickname(name)
        k.sender(nil, isDM: false, iSent: false)
        return k
    }

    /// Every seat's dice this round, sorted, as the tests-only view has them.
    private func allDice(seats: Int) throws -> [[Int]] {
        let v = try readCnView(XCTUnwrap(cn_api_view(CN_API_ALL)))
        return (0..<seats).map { s in v.all[(s * CN_START_DICE)..<((s + 1) * CN_START_DICE)].filter { $0 != 0 } }
    }

    private static let seed: [UInt8] = (0..<32).map { UInt8(($0 * 7 + 11) & 0xFF) }

    func testTheLayoutOfTheReadersIsTheLibrarys() {
        XCTAssertTrue(BridgeKernel.layoutMatches, "cn_api_layout_hash against SG_LAYOUT_HASH")
    }

    func testARaiseACallAndTheRevealMapIntoTheModel() throws {
        // Alex makes a DM lobby
        var alex = phone("Alex")
        XCTAssertTrue(alex.newGame(dm: true, seed: Self.seed))
        var m = alex.table
        XCTAssertEqual(m.phase, .lobby)
        XCTAssertEqual(m.seats.map(\.lobbyRow), ["1. Alex (You)"], "the lobby row is the kernel's")
        XCTAssertEqual(m.seats.map(\.name), ["Alex"], "the name a bubble shows never says You")
        XCTAssertEqual(m.me, 0)
        XCTAssertEqual(m.offered, .waiting, "seated alone, and the newest bubble is mine")
        XCTAssertEqual(m.bubbleCaption, "Alex wants a game of Chui Niu. Tap to join")
        let lobby = try XCTUnwrap(alex.stagedURL())

        // Bo taps it and joins, which fills a DM table and starts it
        var bo = phone("Bo")
        XCTAssertEqual(bo.adoptBubble(lobby), 0)
        m = bo.table
        XCTAssertEqual(m.offered, .join)
        XCTAssertNil(m.me, "a spectator until the join")
        XCTAssertTrue(bo.join(name: "Bo"))
        m = bo.table
        XCTAssertEqual(m.phase, .bidding, "the join started it")
        XCTAssertEqual(m.bubbleCaption, "Dice rolled. Alex bids first")
        let start = try XCTUnwrap(bo.stagedURL())

        // Alex opens the start: seat 0, five dice, the opening menu
        alex = phone("Alex")
        XCTAssertEqual(alex.adoptBubble(start), 0)
        let round1 = try allDice(seats: 2)
        m = alex.table
        XCTAssertEqual(m.me, 0)
        XCTAssertEqual(m.seats.map(\.name), ["Alex", "Bo"])
        XCTAssertEqual(m.seats.map(\.dice), [5, 5])
        XCTAssertEqual(m.seats.map(\.isTurn), [true, false])
        XCTAssertEqual(m.myDice, round1[0], "my dice are the kernel's seat 0")
        XCTAssertEqual(m.myDice, [2, 2, 4, 4, 5], "the seed's dice (the recipe is pinned in cn_dice_test)")
        XCTAssertNil(m.bid)
        XCTAssertEqual(m.caption, "Your turn: open the bidding")
        var menu = try XCTUnwrap(m.menu)
        XCTAssertFalse(menu.callAllowed, "no call on nothing")
        XCTAssertEqual(menu.minimumRaise, Bid(quantity: 1, face: 2))
        XCTAssertEqual(menu.maxQuantity, 10, "every die on the table")
        XCTAssertEqual(Array(menu.minQuantityByFace[2...6]), [1, 1, 1, 1, 1], "any face from one")

        // Alex stages two 4s: the committed table does not move, the bubble does
        XCTAssertTrue(alex.raise(quantity: 2, face: 4))
        m = alex.table
        XCTAssertNil(m.bid, "a staged move never changes the committed table")
        XCTAssertEqual(m.stagedBid, Bid(quantity: 2, face: 4))
        XCTAssertEqual(m.stagedBidText, "two 4s")
        XCTAssertEqual(m.caption, "Send to bid two 4s")
        XCTAssertEqual(m.bubbleCaption, "Alex bid two 4s")
        let raise = try XCTUnwrap(alex.stagedURL())
        XCTAssertTrue(alex.keepsStaged(raise))
        alex.sent(raise)
        XCTAssertFalse(alex.keepsStaged(raise), "committed")
        XCTAssertEqual(alex.table.bid, Bid(quantity: 2, face: 4))

        // Bo: the bid to beat, and the per-face table with its no-raise faces
        bo = phone("Bo")
        XCTAssertEqual(bo.adoptBubble(raise), 0)
        m = bo.table
        XCTAssertEqual(m.me, 1)
        XCTAssertEqual(m.myDice, round1[1], "Bo's own dice, nobody else's")
        XCTAssertEqual(m.bid, Bid(quantity: 2, face: 4))
        XCTAssertEqual(m.bidText, "two 4s")
        XCTAssertEqual(m.bidder, 0)
        menu = try XCTUnwrap(m.menu)
        XCTAssertTrue(menu.callAllowed)
        XCTAssertEqual(Array(menu.minQuantityByFace[2...6]), [3, 3, 3, 2, 2], "above two 4s")
        XCTAssertTrue(alex.isNewer(raise, than: start) && !alex.isNewer(start, than: raise), "the kernel's ranking")
        XCTAssertTrue(alex.sameGame(raise, start))

        // Bo calls: nothing lifts while staged (K8)
        XCTAssertTrue(bo.call())
        m = bo.table
        XCTAssertNil(m.reveal, "a staged call reveals nothing")
        XCTAssertEqual(m.caption, "Send to call two 4s")
        XCTAssertEqual(m.bubbleCaption, "Bo calls two 4s")
        let call = try XCTUnwrap(bo.stagedURL())
        bo.sent(call)
        m = bo.table
        let r = try XCTUnwrap(m.reveal, "every cup lifts once it is sent")
        XCTAssertEqual(m.phase, .revealed)
        XCTAssertEqual(r.dice, round1, "the reveal shows the round's dice")
        XCTAssertEqual(r.counts, round1.map { $0.map { $0 == 4 || $0 == 1 } }, "the 4s and the wild 1s count")
        let count = round1.joined().filter { $0 == 4 || $0 == 1 }.count
        let loser = count >= 2 ? 1 : 0
        XCTAssertEqual(r.loser, loser)
        XCTAssertEqual(r.bid, Bid(quantity: 2, face: 4))
        XCTAssertEqual(r.outcome, "Bo calls. Two 4s was \(loser == 1 ? "true" : "false"), \(loser == 1 ? "Bo" : "Alex") loses a die")
        XCTAssertNotNil(bo.revealMotion(atMs: 0), "the call's reveal plays on the caller's phone")
        XCTAssertEqual(bo.revealMotion(atMs: 0)?.cupsUp, false, "the cups are down when it starts")
        XCTAssertEqual(bo.revealMotion(atMs: 60_000)?.done, true)

        // the loser drops a die, and the next round opens with the loser
        XCTAssertEqual(m.seats[loser].dice, 4)
        XCTAssertEqual(m.seats[loser].isTurn, true, "the loser opens")
        XCTAssertEqual(r.nextAllowed, true)
        XCTAssertFalse(bo.nextRound(), "a look stages nothing")
        m = bo.table
        XCTAssertEqual(m.phase, .bidding, "the next round's table")
        let round2 = try allDice(seats: 2)
        XCTAssertEqual(m.myDice, round2[1])
        XCTAssertEqual(m.menu != nil, loser == 1, "the menu is the opener's")
    }

    /// The top of the table: after ten 5s on ten dice only ten 6s is left,
    /// and the faces the kernel gives no raise read as above maxQuantity.
    func testAFaceWithNoRaiseLeftIsAboveTheStepper() throws {
        let alex = phone("Ace")
        XCTAssertTrue(alex.newGame(dm: true, seed: Self.seed.reversed()))
        let lobby = try XCTUnwrap(alex.stagedURL())
        let bo = phone("Bee")
        XCTAssertEqual(bo.adoptBubble(lobby), 0)
        XCTAssertTrue(bo.join(name: "Bee"))
        let start = try XCTUnwrap(bo.stagedURL())
        let ace = phone("Ace")
        XCTAssertEqual(ace.adoptBubble(start), 0)
        XCTAssertTrue(ace.raise(quantity: 10, face: 5))
        let top = try XCTUnwrap(ace.stagedURL())
        ace.sent(top)
        let bee = phone("Bee")
        XCTAssertEqual(bee.adoptBubble(top), 0)
        let menu = try XCTUnwrap(bee.table.menu)
        XCTAssertEqual(menu.maxQuantity, 10)
        XCTAssertEqual(Array(menu.minQuantityByFace[2...6]), [11, 11, 11, 11, 10], "only ten 6s is left")
        XCTAssertFalse(BidPicker.raiseEnabled(quantity: 10, face: 4, menu: menu), "ten 4s is no raise")
        XCTAssertTrue(BidPicker.raiseEnabled(quantity: 10, face: 6, menu: menu), "ten 6s is")
        XCTAssertTrue(menu.callAllowed)
    }
}
