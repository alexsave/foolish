// LiveArrivalRoleMarkTests - NOTE 2, the BOARD half: "Live arrival good take a
// while to show up?"
//
// A good that does not close the bout (three or more seats) emits no step, so
// the arrival's stream is legitimately empty and the only thing that changes
// is a role mark: the attacker's sword turns into the check.
// LiveArrival4pTests pins that the controller hands the board that change;
// these tests mount the REAL board on the controller, deliver the bubble the
// way GameSurface.seatOnBoard does, and read back what every seat's mark is
// drawing once the board is at rest.
//
// THE MECHANISM. The board's view-change router (`flyBoutEndToDiscard`) sends
// every arrival to the open-replay and reported "a sequence owns the roles",
// while the open-replay returned on an empty stream without starting one. So
// the `!sequenced` role sync in the board's `onChange` never ran, and a warm
// board kept drawing the marks of the bubble before (the ledger's roles
// outlive the view) until some later sequence happened to sync them.
// THE FIX makes the open-replay say whether it started a sequence and the
// router return that, so the one existing role sync for unsequenced view
// changes owns this case too (flag `arrival.emptyroles`, ships on).
//
// WARM, ON PURPOSE. A board that has never synced its roles draws the live
// view, so the defect only shows on a board that has already played a
// sequence - the cold open of the covered table below, whose cover is a real
// stream - which is every board a live arrival lands on in practice.
//
// The probe is `MessageTableView.drawnMarks` / `drawnRoles` (DEBUG), filled by
// the `traceMark` call every seat's mark already makes when it is drawn.

import XCTest
import SwiftUI
@testable import FoolishKit

@MainActor
final class LiveArrivalRoleMarkTests: XCTestCase {

    private var window: UIWindow?

    /// The probe is static, so one test's marks must not become the next one's
    /// history.
    override func setUp() async throws {
        try await super.setUp()
        MessageTableView.resetDrawnMarks()
    }

    /// Take the board down and let it go before the next test runs, so a
    /// mounted board's teardown cannot land inside some later test's
    /// measurement (MemoryProfileTests reads process-wide resident memory).
    override func tearDown() async throws {
        window?.isHidden = true
        window?.rootViewController = nil
        window = nil
        try? await Task.sleep(nanoseconds: 300_000_000)
        try await super.tearDown()
    }

    /// The board, mounted for real in the app test host, on a controller that
    /// has NOT begun - so the board's own `.task` begins it and the cold open
    /// runs through the same view change a tapped bubble does.
    private func mount(_ f: LiveArrivalFixture, seat: Int,
                       on bubble: ChainBubble) throws -> MessageTurnController {
        let scene = try XCTUnwrap(UIApplication.shared.connectedScenes
            .compactMap { $0 as? UIWindowScene }.first, "needs the app test host")
        let c = MessageTurnController(parentPayload: bubble.payload, parent: bubble.env,
                                      mySeat: seat)
        let w = UIWindow(windowScene: scene)
        w.rootViewController = UIHostingController(
            rootView: MessageTableView(controller: c, onSend: { _, _ in }))
        w.makeKeyAndVisible()
        window = w
        return c
    }

    /// Wait for the board to come to rest: no sequence running, for long
    /// enough that a role flip or a closing beat would have shown up.
    private func settle(_ what: String) async {
        let deadline = Date().addingTimeInterval(20)
        var quietSince: Date?
        while Date() < deadline {
            try? await Task.sleep(nanoseconds: 50_000_000)
            if BoardAnimator.isSequencing { quietSince = nil; continue }
            if quietSince == nil { quietSince = Date() }
            if Date().timeIntervalSince(quietSince!) > 1.0 { return }
        }
        XCTFail("\(what): the board never came to rest")
    }

    /// The marks the board is drawing must be the game's, at rest, and no seat
    /// may have drawn the same mark twice since `MessageTableView.resetDrawnMarks`
    /// (a mark that comes back is a mark that animated twice).
    private func assertMarksAtRest(_ c: MessageTurnController, _ what: String,
                                   file: StaticString = #filePath, line: UInt = #line) {
        guard let view = c.view else { return XCTFail("\(what): no view", file: file, line: line) }
        XCTAssertEqual(MessageTableView.drawnRoles, MessageTableView.RoleState(view),
                       "\(what): the board is drawing stale role marks at rest "
                       + "(marks drawn: \(MessageTableView.drawnMarks))", file: file, line: line)
        for (seat, history) in MessageTableView.drawnMarks {
            XCTAssertEqual(Set(history).count, history.count,
                           "\(what): seat \(seat)'s mark came back to one it had left: \(history)",
                           file: file, line: line)
        }
    }

    // MARK: - the non-closing good (note 2)

    func testANonClosingGoodArrivingOnAWarmBoardTurnsTheSwordAtThreeSeats() async throws {
        try await nonClosingGoodsTurnTheirSwords(players: 3, goods: 1)
    }

    /// Four seats: two goods in a row, each arriving on the board the last one
    /// left - the good-run of the owner's 4p session.
    func testNonClosingGoodsArrivingOnAWarmBoardTurnTheSwordsAtFourSeats() async throws {
        try await nonClosingGoodsTurnTheirSwords(players: 4, goods: 2)
    }

    private func nonClosingGoodsTurnTheirSwords(players: Int, goods: Int) async throws {
        let f = try await LiveArrivalFixture.coveredTable(players: players)
        let c = try mount(f, seat: f.defender, on: f.root)
        await settle("\(players)p cold open")
        XCTAssertFalse(c.openReplayEvents.isEmpty,
                       "\(players)p precondition: the cold open is a real sequence, so the board is warm")
        assertMarksAtRest(c, "\(players)p cold open of the covered table")

        var parent = f.root
        for seat in f.attackers.prefix(goods) {
            let x = try await f.good(seat, after: parent)
            MessageTableView.resetDrawnMarks()
            await f.arrive(x, at: c)
            XCTAssertEqual(c.basePayload, x.payload)
            XCTAssertTrue(c.openReplayEvents.isEmpty,
                          "\(players)p: a non-closing good has no step [\(c.openReplayEvents.kindNames)]")
            await settle("\(players)p good by seat \(seat)")
            XCTAssertEqual(MessageTableView.drawnMarks[seat]?.last, "check",
                           "\(players)p: seat \(seat) said good and the board still draws "
                           + "\(MessageTableView.drawnMarks[seat] ?? [])")
            assertMarksAtRest(c, "\(players)p after seat \(seat)'s good arrived")
            parent = x
        }
    }

    func testTheFixShipsOn() {
        XCTAssertTrue(MessageTableView.opensEmptyWithRoleSyncByDefault)
    }

    // MARK: - controls

    /// THE 2P CONTROL: the closing good is a full sequence (sweep, refill, role
    /// hand-off) and ends with the marks right, each turned once.
    func testTwoSeatsTheClosingGoodSweepsAndEndsWithTheRightMarks() async throws {
        let f = try await LiveArrivalFixture.coveredTable(players: 2)
        let c = try mount(f, seat: f.defender, on: f.root)
        await settle("2p cold open")
        let x1 = try await f.good(f.attackers[0], after: f.root)
        MessageTableView.resetDrawnMarks()
        await f.arrive(x1, at: c)
        XCTAssertTrue(c.openReplayEvents.sweepsTheTable,
                      "2p closing good [\(c.openReplayEvents.kindNames)]")
        await settle("2p closing good")
        assertMarksAtRest(c, "2p after the closing good")
    }

    /// A COLD OPEN of a non-closing good keeps what it did: its stream is empty,
    /// so it draws the bubble's own roles from the first paint and turns
    /// nothing.
    func testAColdOpenOfANonClosingGoodDrawsItsRolesAndTurnsNothing() async throws {
        let f = try await LiveArrivalFixture.coveredTable(players: 3)
        let a = f.attackers[0]
        let x1 = try await f.good(a, after: f.root)
        MessageTableView.resetDrawnMarks()
        let c = try mount(f, seat: f.defender, on: x1)
        await settle("3p cold open of a good")
        XCTAssertTrue(c.openReplayEvents.isEmpty, "[\(c.openReplayEvents.kindNames)]")
        XCTAssertEqual(MessageTableView.drawnMarks[a], ["check"],
                       "a cold open of a good draws the check and nothing else")
        assertMarksAtRest(c, "3p cold open of a good")
    }
}
