import XCTest
@testable import FoolishKit

/// Undoing a PICKUP: the card stays in the hand until its flight to the table
/// exists - the hand-side twin of UndoHoldsTableTests.
///
/// Filmed at normal speed (lastmove-live pickup, expanded, collapse, Undo):
///     5.007  J in the hand, Undo on the plank
///     5.023  J gone from the hand          <- the undo published the new hand
///     5.077  the J's flight (orange square) appears
/// 54ms with the card nowhere. The trace agreed: `-> undoRelease`, then
/// `flight START` 53ms later. The fix is the fan's own holdback, which the
/// kernel draws even while the card is veiled (`anim_veil_fan` is
/// `veiled & ~holdback`): arm it with the leaving cards when the undo starts,
/// let it go in the builder - the turn `playStep` hands the animator the flight.
@MainActor
final class UndoReleaseHandHoldTests: XCTestCase {

    private func source(_ path: String) throws -> String {
        let here = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
        return try String(contentsOf: here.deletingLastPathComponent()
            .appendingPathComponent(path), encoding: .utf8)
    }

    /// The mechanism: a held-back card is drawn in the fan though it is veiled
    /// for the table it is flying to.
    func testAHeldBackCardIsDrawnInTheFanWhileVeiled() {
        let j = Card(s: 3, v: 10)
        XCTAssertFalse(Veil.fan(veiled: [j.identity], holdback: [j]).contains(j.identity))
        XCTAssertTrue(Veil.fan(veiled: [j.identity], holdback: []).contains(j.identity))
    }

    /// MUTANTS: the holdback armed inside the Task (after the undo has painted);
    /// never armed; released before `playStep` rather than in its builder.
    func testTheUndoHoldsTheLeavingCardsInTheHandUntilTheirFlight() throws {
        let board = try source("FoolishKit/Boards/MessageTableView.swift")
        let start = try XCTUnwrap(board.range(of: "private func flyUndoRelease("))
        let body = String(board[start.upperBound...].prefix(7000))
        let arm = try XCTUnwrap(body.range(of: "if UndoFlightSource.holdsLeaving { handHoldback = targets.map(\\.0); handHoldbackAt = veiledAt }"),
                                "the leaving cards are not held in the fan")
        let task = try XCTUnwrap(body.range(of: "Task {"))
        XCTAssertLessThan(arm.lowerBound, task.lowerBound, "held only after the undo has painted")
        let step = try XCTUnwrap(body.range(of: "await playStep {"))
        let release = try XCTUnwrap(body.range(of: "if UndoFlightSource.holdsLeaving, !flights.isEmpty { self.handHoldback = [] }"),
                                    "the hold is never let go as the flight starts")
        XCTAssertGreaterThan(release.lowerBound, step.lowerBound, "let go before the flight exists")
    }
}
