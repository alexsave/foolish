import XCTest
@testable import FoolishKit

/// An undone cover flies home FROM its tilt, turning upright on the way.
///
/// Owner, filming the Undo of a bout-ending cover: "when we undo, the cover card
/// that flies in/out IMMEDIATELY rotates to straight, then flies back. It
/// shouldn't. The attack card it was covering correctly gradually rotates back
/// to straight. But the cover card should rotate as it flies back."
@MainActor
final class UndoKeepsTiltTests: XCTestCase {

    private let attack = Card(s: 1, v: 9)
    private let cover = Card(s: 1, v: 12)
    private let lone = Card(s: 2, v: 6)
    private var table: [BattleView] {
        [BattleView(attack: attack, defense: cover), BattleView(attack: lone, defense: nil)]
    }

    /// MUTANTS: the cover answered upright; the attack's sign flipped.
    func testEachCardStartsAtTheTiltTheGridDrewItAt() {
        XCTAssertEqual(UndoFlightSource.tilt(for: cover, in: table), FBattleGrid.coverAngle,
                       "a cover lies across its attack")
        XCTAssertEqual(UndoFlightSource.tilt(for: attack, in: table), -FBattleGrid.coverAngle,
                       "the attack under a cover leans the other way")
        XCTAssertEqual(UndoFlightSource.tilt(for: lone, in: table), 0, "an uncovered attack stands upright")
        XCTAssertEqual(UndoFlightSource.tilt(for: Card(s: 0, v: 13), in: table), 0, "a card not on the table")
        XCTAssertNotEqual(FBattleGrid.coverAngle, 0)
    }

    /// Ships on, and the undo's flight home is built from it.
    /// MUTANT: the flight built without `fromAngle`.
    func testTheUndoFlightStartsFromTheTilt() throws {
        XCTAssertTrue(UndoFlightSource.keepsTiltByDefault)
        let board = try BoardSource.text()
        XCTAssertTrue(board.contains("fromAngle: UndoFlightSource.keepsTilt ? UndoFlightSource.tilt(for: c, in: old.battles) : 0,"),
                      "the undo's flight home no longer starts from the card's tilt")
    }
}
