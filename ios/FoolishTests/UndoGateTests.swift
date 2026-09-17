import XCTest
@testable import FoolishKit

/// Undo cannot be pressed while the board is moving.
///
/// Owner, on build 72: "lets just disable the undo card until the animations
/// and the collapses are done. Cuz i dont want to get into a weird scenario
/// where we undo mid animation." Moving means any of: an animated sequence
/// holding `BoardAnimator`'s depth (a card flight, a sweep, a deal), the
/// collapse tween, or Messages still sliding the sheet.
///
/// The X on the staged bubble is NOT gated: by the time the board hears of it
/// Messages has already removed the bubble, and refusing the undo then would
/// leave a staged move with no bubble to send it in.
@MainActor
final class UndoGateTests: XCTestCase {

    private func source(_ path: String) throws -> String {
        let here = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
        return try String(contentsOf: here.deletingLastPathComponent()
            .appendingPathComponent(path), encoding: .utf8)
    }

    /// MUTANTS: any one of the three dropped from `accepts`.
    func testUndoIsRefusedWhileAnythingMoves() {
        XCTAssertTrue(UndoGate.accepts(waits: true, sequencing: false, tweening: false, presenting: false))
        XCTAssertFalse(UndoGate.accepts(waits: true, sequencing: true, tweening: false, presenting: false),
                       "a card flight / sweep / deal is running")
        XCTAssertFalse(UndoGate.accepts(waits: true, sequencing: false, tweening: true, presenting: false),
                       "the collapse is running")
        XCTAssertFalse(UndoGate.accepts(waits: true, sequencing: false, tweening: false, presenting: true),
                       "Messages is still moving the sheet")
    }

    /// The flag's other state is build 72's Undo, pressable at any time.
    /// MUTANT: `waits` ignored.
    func testTheFlagOffPathTakesEveryTap() {
        XCTAssertTrue(UndoGate.accepts(waits: false, sequencing: true, tweening: true, presenting: true))
    }

    /// The live answer reads the live state - not a copy of it.
    /// MUTANTS: `acceptsNow` not reading `BoardAnimator.isSequencing`; not
    /// reading `CollapseTween.isTweening`.
    func testTheLiveAnswerFollowsTheBoard() {
        XCTAssertTrue(UndoGate.waits, "precondition: ships on, and no dev.flags says otherwise")
        XCTAssertTrue(UndoGate.acceptsNow, "a still board takes the tap")

        let hold = BoardAnimator.holdSequence()
        XCTAssertFalse(UndoGate.acceptsNow, "refused while a sequence holds the board")
        hold.release()
        XCTAssertTrue(UndoGate.acceptsNow, "and taken again the moment it lets go")

        CollapseTween.isTweening = true
        defer { CollapseTween.isTweening = false }
        XCTAssertFalse(UndoGate.acceptsNow, "refused while the collapse runs")
    }

    /// Ships on; a debug build with no `dev.flags` runs what ships.
    /// MUTANT: `waitsByDefault = false`.
    func testTheGateShipsOn() {
        XCTAssertTrue(UndoGate.waitsByDefault)
        #if DEBUG || SOLO_TESTING
        XCTAssertEqual(UndoGate.waits, UndoGate.waitsByDefault)
        #endif
    }

    /// The pill asks the gate twice - to draw itself disabled, and again at the
    /// tap, because the drawing is refreshed on a timer and a tap can land
    /// between two refreshes. The X on the staged bubble must not ask at all.
    /// MUTANTS: the pill's `enabled:` without the gate; its action calling
    /// `undoAction` directly; `undoAction` itself asking the gate.
    func testThePillAsksAndTheBubbleXDoesNot() throws {
        let board = try source("FoolishKit/Boards/MessageTableView.swift")
        XCTAssertTrue(board.contains("enabled: !controller.conflictRetracting && UndoGate.acceptsNow"),
                      "the Undo pill does not draw itself disabled while the board moves")
        XCTAssertTrue(board.contains("action: undoPillTapped"),
                      "the Undo pill does not re-check the gate at the tap")
        XCTAssertTrue(board.contains("guard UndoGate.acceptsNow else"),
                      "undoPillTapped does not refuse a tap mid-animation")
        let undo = try XCTUnwrap(board.range(of: "private func undoAction()"))
        let body = board[undo.upperBound...].prefix(600)
        XCTAssertFalse(body.contains("UndoGate"),
                       "undoAction is shared with the bubble's X, which must never be refused")
    }
}
