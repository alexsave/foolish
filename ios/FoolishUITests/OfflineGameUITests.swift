// OfflineGameUITests.swift — drives the real UI to prove the core interactions
// work end to end (§6). The headline test is drag-to-play: a card dragged up
// onto the table attacks, which is the interaction the redesign added.

import XCTest

final class OfflineGameUITests: XCTestCase {

    override func setUp() { continueAfterFailure = false }

    /// Face-up cards on the centre of the table (the battles band). Empty at the
    /// start of a round; a played attack card lands here.
    private func tableCardCount(_ app: XCUIApplication) -> Int {
        let h = app.windows.firstMatch.frame.height
        let pred = NSPredicate(format: "label CONTAINS[c] ' of '")
        return app.otherElements.matching(pred).allElementsBoundByIndex
            .filter { $0.frame.midY > h * 0.30 && $0.frame.midY < h * 0.62 }.count
    }

    func testDragACardUpToAttack() {
        let app = XCUIApplication()
        app.launchArguments = ["-offlinePlayers", "2"]

        // Re-deal until the human (seat 0) is the first attacker, i.e. there's no
        // "Take" button waiting (that would mean we're defending) and the table
        // is still empty.
        var ready = false
        for _ in 0..<8 {
            app.launch()
            _ = app.otherElements.firstMatch.waitForExistence(timeout: 5)
            Thread.sleep(forTimeInterval: 1.5)   // deal settles
            if !app.buttons["Take"].exists && tableCardCount(app) == 0 {
                ready = true; break
            }
            app.terminate()
        }
        XCTAssertTrue(ready, "human never started as first attacker across 8 deals")

        // Drag the middle of the hand fan up onto the open table → attack.
        let from = app.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.93))
        let to = app.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.42))
        from.press(forDuration: 0.12, thenDragTo: to)

        // The attacked card now sits on the table.
        var landed = 0
        for _ in 0..<12 {
            Thread.sleep(forTimeInterval: 0.3)
            landed = tableCardCount(app)
            if landed > 0 { break }
        }
        XCTAssertGreaterThan(landed, 0, "dragging a card up did not put it on the table (attack didn't register)")
    }
}
