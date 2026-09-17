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

    /// THE WHOLE STRETCH, NOT ITS PIECES. Owner: "i notice that you dim the undo
    /// button mulitple times. like the attack plays, the undo is dimmed, then
    /// the attack is done and it undims. then the collapse plays, the undo is
    /// dimmed... How about instead of dimming, you just dont show any action
    /// button between when the attack animation starts playing and the
    /// autocollapse finishes?" Between the flight landing and the collapse
    /// starting the extension deliberately rests (250ms + waitForSettle +
    /// 500ms), and nothing moving meant the gate opened for exactly that rest.
    /// MUTANT: `autoCollapsing` dropped from `accepts`.
    func testUndoIsRefusedForTheWholeAutoCollapse() {
        XCTAssertFalse(UndoGate.accepts(waits: true, sequencing: false, tweening: false,
                                        presenting: false, autoCollapsing: true),
                       "the rest between the flight and the collapse opened the gate")
    }

    /// The live answer reads the extension's auto-collapse too.
    /// MUTANT: `acceptsNow` not passing `CollapseTween.isAutoCollapsing`.
    func testTheLiveAnswerFollowsTheAutoCollapse() {
        CollapseTween.isAutoCollapsing = true
        defer { CollapseTween.isAutoCollapsing = false }
        XCTAssertFalse(UndoGate.acceptsNow())
    }

    /// FROM THE TAP, NOT FROM THE FLIGHT. Filmed at normal speed: Undo showed
    /// for a few frames right after Attack was tapped, before the card left the
    /// hand - the placement's sequence hold is taken in a Task a beat later, and
    /// `stage` has not marked the auto-collapse yet. The one thing already true
    /// in that turn is the veil: the played card is hidden before the apply.
    /// MUTANT: the board's veil not passed to the gate.
    func testTheVeiledCardOfATapThatJustLandedKeepsUndoAway() throws {
        XCTAssertFalse(UndoGate.accepts(waits: true, sequencing: false, tweening: false,
                                        presenting: false, autoCollapsing: false, cardsVeiled: true),
                       "Undo flashed up between the tap and the flight")
        let board = try source("FoolishKit/Boards/MessageTableView.swift")
        XCTAssertTrue(board.contains("UndoGate.acceptsNow(cardsVeiled: !animator.hidden.isEmpty)"),
                      "the board does not tell the gate about cards it is veiling")
        XCTAssertFalse(board.contains("UndoGate.acceptsNow {") || board.contains("UndoGate.acceptsNow else"),
                       "somewhere still asks the gate without the veil")
    }

    /// NOT A PLAY BUTTON EITHER. Filmed frame by frame at the tap: Attack,
    /// then ONE frame of "Good", then nothing. `play` clears the selection
    /// synchronously and applies in a Task, so for a paint the attacker had no
    /// selection and nothing staged - and the bar offered Good. Owner: "you just
    /// dont show any action button between when the attack animation starts
    /// playing and the autocollapse finishes".
    /// MUTANTS: `acting` not reading the in-flight play; the mark set after
    /// the selection is cleared; never cleared when the apply answers.
    func testNoPlayButtonBetweenTheTapAndTheStage() throws {
        let board = try source("FoolishKit/Boards/MessageTableView.swift")
        XCTAssertTrue(board.contains("let acting = controller.iCanAct && !controller.canSend && !playInFlight"),
                      "the play buttons do not stand down while a play is being applied")
        let start = try XCTUnwrap(board.range(of: "private func play(_ move: Move) {"))
        let body = String(board[start.upperBound...].prefix(4000))
        let mark = try XCTUnwrap(body.range(of: "playInFlight = ActionPillSlot.holdsWhilePlaying"),
                                 "play never marks itself in flight")
        let clear = try XCTUnwrap(body.range(of: "selection.removeAll()"))
        XCTAssertLessThan(mark.lowerBound, clear.lowerBound,
                          "the selection is cleared a paint before the bar knows a play is in flight")
        XCTAssertTrue(body.contains("playInFlight = false"), "a play in flight is never finished")
        XCTAssertTrue(ActionPillSlot.holdsWhilePlayingByDefault)
    }

    /// NOR WHILE AN UNDO FLIES. Filmed undoing a pickup: Pickup was back on the
    /// plank in the very frame the undo published, with the card still flying
    /// from the hand to the table. Every play button now waits for the same
    /// still board Undo does, redrawn on the same short timer (the sequence
    /// hold is let go after the animator's last publish, so nothing observed
    /// would redraw the column when it ends).
    /// MUTANTS: `acting` not reading `boardStill`; the column not redrawn on
    /// the timer; the flag shipping off.
    func testNoPlayButtonWhileAnUndoFlies() throws {
        XCTAssertTrue(ActionPillSlot.waitsForStillByDefault)
        let board = try source("FoolishKit/Boards/MessageTableView.swift")
        XCTAssertTrue(board.contains("let acting = controller.iCanAct && !controller.canSend && !playInFlight && boardStill"),
                      "a play button shows while the board is still moving")
        XCTAssertTrue(board.contains("TimelineView(.periodic(from: .now, by: 0.1)) { _ in\n                    actionBar(view)"),
                      "the play column is not redrawn when the board comes to rest")
        // Take is the one pill NOT gated on `acting` (it deliberately does not
        // read the kernel's menu), so the hold and the wait have to be spelled
        // out on it - filmed: Pickup back on the plank through a pickup undo's
        // whole flight, and (owner) "the label... changed for a single frame
        // after you hit pickup".
        XCTAssertTrue(board.contains("&& controller.pickupHold == 0 && !controller.superseded\n                && !playInFlight && boardStill,"),
                      "Take shows during a play in flight or an animation")
    }

    /// Hidden, not dimmed - and it ships that way.
    /// MUTANTS: `hidesByDefault = false`; the pill drawn whatever the gate says.
    func testThePillIsNotShownWhileRefused() throws {
        XCTAssertTrue(UndoGate.hidesByDefault)
        #if DEBUG || SOLO_TESTING
        XCTAssertEqual(UndoGate.hides, UndoGate.hidesByDefault)
        #endif
        // Owner: "basically I never want to see a dimmed undo button." That
        // covers the conflict retraction's disabled Undo too: shown enabled,
        // or not shown.
        let board = try source("FoolishKit/Boards/MessageTableView.swift")
        XCTAssertTrue(board.contains("if !controller.conflictRetracting && still {\n                            undoPill(enabled: true)"),
                      "the Undo pill is drawn (dimmed) while the gate or a retraction refuses")
    }

    /// The extension marks the WHOLE auto-collapse: from the moment `stage`
    /// takes the expanded path until the transition has settled, on every way
    /// out of it.
    /// MUTANTS: the mark set after the rest; the `defer` that clears it removed.
    func testTheExtensionMarksTheWholeAutoCollapse() throws {
        let vc = try source("FoolishMessages/MessagesViewController.swift")
        let set = try XCTUnwrap(vc.range(of: "CollapseTween.isAutoCollapsing = true"),
                                "the extension never marks its auto-collapse")
        let rest = try XCTUnwrap(vc.range(of: "try? await Task.sleep(nanoseconds: 250_000_000)"))
        XCTAssertLessThan(set.lowerBound, rest.lowerBound, "marked only after the rest began")
        XCTAssertTrue(vc.contains("defer { CollapseTween.isAutoCollapsing = false }"),
                      "an early return leaves Undo hidden for good")
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
        XCTAssertTrue(UndoGate.acceptsNow(), "a still board takes the tap")

        let hold = BoardAnimator.holdSequence()
        XCTAssertFalse(UndoGate.acceptsNow(), "refused while a sequence holds the board")
        hold.release()
        XCTAssertTrue(UndoGate.acceptsNow(), "and taken again the moment it lets go")

        CollapseTween.isTweening = true
        defer { CollapseTween.isTweening = false }
        XCTAssertFalse(UndoGate.acceptsNow(), "refused while the collapse runs")
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
        XCTAssertTrue(board.contains("undoPill(enabled: !controller.conflictRetracting && still)"),
                      "the Undo pill does not draw itself disabled while the board moves")
        XCTAssertTrue(board.contains("action: undoPillTapped"),
                      "the Undo pill does not re-check the gate at the tap")
        XCTAssertTrue(board.contains("guard UndoGate.acceptsNow(cardsVeiled: !animator.hidden.isEmpty) else"),
                      "undoPillTapped does not refuse a tap mid-animation")
        let undo = try XCTUnwrap(board.range(of: "private func undoAction()"))
        let body = board[undo.upperBound...].prefix(600)
        XCTAssertFalse(body.contains("UndoGate"),
                       "undoAction is shared with the bubble's X, which must never be refused")
    }
}
