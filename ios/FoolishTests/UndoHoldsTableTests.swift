import XCTest
@testable import FoolishKit

/// An undone card is on the table until its flight home is in the air - not
/// ~90ms less.
///
/// Owner, on the undo fix: "animation is better but the card still briefly
/// disappears... the animating flying veil needs to appear as soon as the
/// table card is gone, not 90ms after. look at how we do this for, idk,
/// literally every other animation". The grid trace had it:
///
///     viewChanged prior=3b new=2b undo=true
///     -> undoReturn [0-13]            (sweep = the three pairs)
///     grid sweeping=false pairs=2     <- the ace gone, a paint after the undo
///     flight START [0-13 ...]         86ms later
///
/// `flyUndoReturn` kept the pre-undo table as the sweep so the card could fly
/// FROM it, but the kernel's choice (`anim_shown_table`) put a non-empty live
/// table above any sweep. Every other flight hides its table copy in the turn
/// its flight starts; this one lost it the moment the undo published. Now the
/// kernel draws a sweep that holds every live card and more
/// (`anim_shown_table_rows`, hold_leaving), and the undo lets go of it in the
/// same turn it hands the animator the flight.
@MainActor
final class UndoHoldsTableTests: XCTestCase {

    // Real cards, in the kernel's numbering (v 1...13, ace 13 - the flight log's
    // "0-13" is the ace of spades). A card the kernel cannot name is never
    // accounted for, so a fixture with v: 14 tests the refusal, not the hold.
    private let two = [BattleView(attack: Card(s: 1, v: 13), defense: Card(s: 2, v: 11)),   // A/Q
                       BattleView(attack: Card(s: 3, v: 11), defense: Card(s: 2, v: 9))]    // Q/10
    private var three: [BattleView] { two + [BattleView(attack: Card(s: 0, v: 13), defense: nil)] }  // + A

    private func source(_ path: String) throws -> String {
        let here = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
        return try String(contentsOf: here.deletingLastPathComponent()
            .appendingPathComponent(path), encoding: .utf8)
    }

    /// MUTANTS: the Swift entry point ignoring `holdLeaving`; the kernel's rule
    /// removed (C mutants are in tests.c).
    func testALeavingCardIsStillDrawnWhereItSat() {
        let t = PreBoutTable.shownTable(live: two, sweep: three, pending: [], holdLeaving: true)
        XCTAssertEqual(t.shown, three, "the undone card vanished from the grid before its flight")
        XCTAssertTrue(t.sweeping, "drawn as a sweep, so the card's own flight hides it")
    }

    /// The flag's other state is build 72's grid.
    func testTheFlagOffPathDrawsTheLiveTable() {
        let t = PreBoutTable.shownTable(live: two, sweep: three, pending: [], holdLeaving: false)
        XCTAssertEqual(t.shown, two)
        XCTAssertFalse(t.sweeping)
    }

    /// Ships on; a debug build with no `dev.flags` runs what ships.
    /// MUTANT: `holdsLeavingByDefault = false`.
    func testTheHoldShipsOn() {
        XCTAssertTrue(UndoFlightSource.holdsLeavingByDefault)
        #if DEBUG || SOLO_TESTING
        XCTAssertEqual(UndoFlightSource.holdsLeaving, UndoFlightSource.holdsLeavingByDefault)
        #endif
    }

    /// The board asks for the hold, and the undo lets go of the table in the
    /// builder - the turn `playStep` hands the flight to the animator - rather
    /// than hiding the card before it starts polling.
    /// MUTANTS: gridRow not passing the flag; the release moved before
    /// `playStep`; the early hide left on the flag-on path.
    func testTheBoardHoldsTheTableUntilTheFlightStarts() throws {
        let board = try source("FoolishKit/Boards/MessageTableView.swift")
        XCTAssertTrue(board.contains("pending: pending, holdLeaving: UndoFlightSource.holdsLeaving)"),
                      "the grid does not ask the kernel to hold a leaving card")
        let start = try XCTUnwrap(board.range(of: "private func flyUndoReturn("))
        let body = String(board[start.upperBound...].prefix(9000))
        let step = try XCTUnwrap(body.range(of: "await playStep {"))
        let release = try XCTUnwrap(body.range(of: "if UndoFlightSource.holdsLeaving, !flights.isEmpty { self.dropSweep() }"),
                                    "the undo never lets go of the held table")
        XCTAssertGreaterThan(release.lowerBound, step.lowerBound,
                             "the table is let go before the flight exists")
        XCTAssertTrue(body.contains("if !UndoFlightSource.holdsLeaving { self.sweptFlownIds.formUnion(flyIds) }"),
                      "the card is still hidden before the flight is built")
    }

    /// THE TABLE IS HELD BEFORE THE UNDO PUBLISHES, not a paint after. Filmed
    /// undoing a FIRST attack: the 8 of hearts jumped from its slot to the
    /// bottom of the drawer for two frames (7.757, 7.810) before its flight
    /// appeared back on the table. The trace's last grid line before the undo
    /// was `cells=0`: the undo's view (an empty table) was painted before
    /// `flyUndoReturn` set the sweep, so the grid drew nothing, its collapse
    /// layer's host sized to nothing, and the sweep came back into a host a
    /// paint out of date. `play` captures the table synchronously before
    /// `apply` for the same reason ("the onChange that re-sets this fires a
    /// paint too late"); Undo now does the same before `controller.undo()`.
    /// MUTANTS: the sweep set after the undo; never set.
    func testUndoHoldsTheTableBeforeTheUndoPublishes() throws {
        let board = try source("FoolishKit/Boards/MessageTableView.swift")
        let start = try XCTUnwrap(board.range(of: "private func undoAction() {"))
        let body = String(board[start.upperBound...].prefix(2500))
        let hold = try XCTUnwrap(body.range(of: "if UndoFlightSource.holdsLeaving, let table = controller.view?.battles, !table.isEmpty { setSweep(table) }"),
                                 "Undo does not hold the table before it publishes")
        let undo = try XCTUnwrap(body.range(of: "await controller.undo()"))
        XCTAssertLessThan(hold.lowerBound, undo.lowerBound, "the table is held a paint too late")
    }
}
