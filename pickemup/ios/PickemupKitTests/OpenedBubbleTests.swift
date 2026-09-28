// OpenedBubbleTests.swift - an opened bubble's plan starts on the first board
// frame anybody sees, not at the adopt (MOTION_REPORT Take B, IOS_DECISIONS
// I46). The extension adopts the tapped bubble while its view is still hidden
// behind a white drawer; with the clock started there, the first draws were
// already in the air when the board appeared.

import CPickemup
import SwiftUI
import XCTest
@testable import PickemupKit

@MainActor
final class OpenedBubbleTests: XCTestCase {

    /// Bo draws three and passes; Alex opens that bubble with the board still
    /// hidden (the controller's `onScreen` false), the player's clock held at `t`.
    private func openedWhileHidden(clock t: @escaping () -> CFTimeInterval) throws -> PickemupHost {
        XCTAssertNotNil(Phones.dmStartedByBo())
        let startLink = try XCTUnwrap(Pk.text, "the start bubble")
        for _ in 0..<3 { XCTAssertTrue(Pk.draw()) }
        XCTAssertTrue(Pk.pass())
        let bo = try XCTUnwrap(Pk.text, "Bo's bubble")
        Phones.be(0)
        XCTAssertEqual(Pk.read(startLink), 0)
        let host = PickemupHost()
        host.showResident()
        host.model.player.now = t
        host.onScreen = false
        XCTAssertEqual(host.adopt(bo, arrival: false), 0)
        XCTAssertNotNil(host.model.player.plan, "the opened bubble plays")
        return host
    }

    // MUTATE: BeatPlayer.play starts the clock whether or not the board is on
    // screen (`if onScreen { start() }` -> `start()`) -> "nothing has played
    // behind the white drawer", "the clock starts on the first frame on screen".
    // MUTATE: BeatPlayer.ms never starts a waiting clock (`if began == nil,
    // onScreen { start() }` removed) -> "the clock starts on the first frame
    // on screen".
    func testTheClockStartsOnTheFirstBoardFrameOnScreen() throws {
        var t: CFTimeInterval = 1000
        let host = try openedWhileHidden(clock: { t })
        let p = host.model.player
        let plan = try XCTUnwrap(p.plan)

        // the drawer stays white for over a second: frames drawn behind it
        // (the body still runs) stand at the plan's first frame
        t += 1.2
        XCTAssertEqual(p.ms(), 0, "nothing has played behind the white drawer")
        XCTAssertNil(p.began, "no clock yet")
        XCTAssertTrue(p.animating, "the plan is still to play")

        // the REAL board, hosted while hidden: its body samples the player and
        // must not start the clock
        AXTree.hosted(TableScreen(model: host.model, onRules: {}), size: CGSize(width: 390, height: 700)) { _ in }
        XCTAssertNil(p.began, "a body drawn off screen starts nothing")

        // the controller shows the board; the first body after that starts it
        t += 0.3
        host.onScreen = true
        let shown = t
        AXTree.hosted(TableScreen(model: host.model, onRules: {}), size: CGSize(width: 390, height: 700)) { _ in }
        let began = try XCTUnwrap(p.began, "the clock starts on the first frame on screen")
        XCTAssertGreaterThanOrEqual(began, shown, "at or after the first board frame, never at the adopt")
        t = began + 0.25
        XCTAssertEqual(p.ms(), 250, "and runs from there")
        XCTAssertEqual(p.plan?.serial, plan.serial, "the same plan, whole")
    }

    // MUTATE: BeatPlayer.play schedules the plan's end at the adopt (`start()`
    // unconditionally) -> "a plan never shown is not ended behind the drawer".
    func testAPlanWaitingForTheBoardIsNotEndedBehindIt() throws {
        let host = try openedWhileHidden(clock: { CACurrentMediaTime() })
        let plan = try XCTUnwrap(host.model.player.plan)
        let past = expectation(description: "longer than the whole plan")
        DispatchQueue.main.asyncAfter(deadline: .now() + Double(plan.totalMs) / 1000 + 0.2) { past.fulfill() }
        wait(for: [past], timeout: 10)
        XCTAssertEqual(host.model.player.plan?.serial, plan.serial, "a plan never shown is not ended behind the drawer")
        XCTAssertTrue(host.model.player.animating)
    }
}
