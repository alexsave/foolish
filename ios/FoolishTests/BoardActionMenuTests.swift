// BoardActionMenuTests - the board's pill menu, driven without a board.
//
// MOST OF THESE USED TO BE SOURCE-TEXT SCANS in UndoGateTests, matching the
// exact spelling of an expression inside `MessageTableView.actionBar` - down to
// the line break in the middle of Take's condition. That was not a style
// choice: the decision lived inside a `some View`, so there was nothing to
// call, and a scan was the only thing available. It is `BoardActionMenu` now,
// and every one of those scans is a value here. The filmed bug each one guards
// is named on the test that guards it, unchanged.
//
// WHAT IS NOT HERE, and deliberately stays a source scan in UndoGateTests: that
// the pill column is redrawn on a timer, that `play` marks itself in flight
// BEFORE it clears the selection, and that the pill's action is
// `undoPillTapped`. Those are facts about the view, and a value cannot see them.

import XCTest
@testable import FoolishKit

final class BoardActionMenuTests: XCTestCase {

    /// A kernel answer that says yes to everything it is asked, so a `false`
    /// below can only have come from the board's own gates.
    private let kernelSaysYes = PlayProbe(move: nil, coverable: [0], bestCover: 0,
                                          canAttack: true, canPass: true, canSayGood: true)
    /// …and one that lists nothing, which is what a settled or a stood-down
    /// seat is handed.
    private let kernelSaysNo = PlayProbe(move: nil, coverable: [], bestCover: nil,
                                         canAttack: false, canPass: false, canSayGood: false)

    /// A board that is ready to act: I have a menu, nothing is staged, my last
    /// tap has landed, the board is at rest, I am an attacker with an empty
    /// selection over a table with cards on it.
    private func gates(iCanAct: Bool = true, canSend: Bool = false,
                       playInFlight: Bool = false, boardStill: Bool = true,
                       superseded: Bool = false, pickupHeld: Bool = false,
                       isDefender: Bool = false, isOut: Bool = false,
                       tableIsEmpty: Bool = false,
                       selectionIsEmpty: Bool = true) -> BoardActionMenu.Gates {
        .init(iCanAct: iCanAct, canSend: canSend, playInFlight: playInFlight,
              boardStill: boardStill, superseded: superseded, pickupHeld: pickupHeld,
              isDefender: isDefender, isOut: isOut, tableIsEmpty: tableIsEmpty,
              selectionIsEmpty: selectionIsEmpty)
    }

    // MARK: - every play pill is the kernel's answer (§17.16)

    /// The whole rule in one assertion: with the kernel listing nothing, an
    /// otherwise perfect board offers no play. Take is the one exception and it
    /// has its own test below, so an attacker is asked here.
    func testAPillIsNeverOfferedOverAKernelThatListsNothing() {
        let m = BoardActionMenu.resolve(kernelSaysNo, gates())
        XCTAssertEqual(m, .none,
                       "a play pill was offered without the kernel listing the move")
    }

    /// And the mirror: the gates alone never conjure one either.
    func testTheAttackersPillsAndTheDefendersDoNotMix() {
        let attacker = BoardActionMenu.resolve(kernelSaysYes, gates(isDefender: false))
        XCTAssertTrue(attacker.canAttack)
        XCTAssertFalse(attacker.canCover, "an attacker was offered Cover")
        XCTAssertFalse(attacker.canPass, "an attacker was offered Pass")

        let defender = BoardActionMenu.resolve(kernelSaysYes, gates(isDefender: true))
        XCTAssertFalse(defender.canAttack, "the defender was offered Attack")
        XCTAssertTrue(defender.canCover)
        XCTAssertTrue(defender.canPass)
    }

    /// Good is selection-aware: a stray tap on it mid-selection would abandon
    /// the cards you had picked (web parity TODO).
    func testGoodIsGoneWhileCardsAreSelected() {
        XCTAssertTrue(BoardActionMenu.resolve(kernelSaysYes, gates()).canDone)
        XCTAssertFalse(BoardActionMenu.resolve(kernelSaysYes,
                                               gates(selectionIsEmpty: false)).canDone,
                       "Good over a selection: tapping it throws the selection away")
    }

    // MARK: - the board's own gates

    /// NOT BETWEEN THE TAP AND THE STAGE. Filmed frame by frame at the tap:
    /// Attack, then ONE frame of "Good", then nothing. `play` clears the
    /// selection synchronously and applies in a Task, so for a paint the
    /// attacker had no selection and nothing staged - and the bar offered Good.
    /// Owner: "you just dont show any action button between when the attack
    /// animation starts playing and the autocollapse finishes".
    func testNoPillBetweenTheTapAndTheStage() {
        XCTAssertEqual(BoardActionMenu.resolve(kernelSaysYes,
                                               gates(playInFlight: true, isDefender: true)),
                       .none,
                       "a pill - Take included - showed while a play was being applied")
        XCTAssertTrue(ActionPillSlot.holdsWhilePlayingByDefault)
    }

    /// NOR WHILE AN UNDO FLIES. Filmed undoing a pickup: Pickup was back on the
    /// plank in the very frame the undo published, with the card still flying
    /// from the hand to the table.
    func testNoPillWhileTheBoardIsStillMoving() {
        XCTAssertEqual(BoardActionMenu.resolve(kernelSaysYes,
                                               gates(boardStill: false, isDefender: true)),
                       .none,
                       "a pill - Take included - showed while the board was moving")
        XCTAssertTrue(ActionPillSlot.waitsForStillByDefault)
    }

    /// Once a move is staged the only control is Undo: the extension has
    /// dropped the human at Messages' Send.
    func testAStagedMoveLeavesNoPlayPill() {
        XCTAssertEqual(BoardActionMenu.resolve(kernelSaysYes,
                                               gates(canSend: true, isDefender: true)),
                       .none,
                       "a play pill survived the staged bubble")
    }

    /// A seat with no published menu is a spectator on someone else's staged
    /// bubble - read-only, whatever the probe happens to say.
    func testASeatWithNoMenuIsOfferedNoPlay() {
        let m = BoardActionMenu.resolve(kernelSaysYes, gates(iCanAct: false))
        XCTAssertFalse(m.canAttack || m.canCover || m.canPass || m.canDone,
                       "a stood-down seat was offered a play")
    }

    // MARK: - Take, the one pill that is not the kernel's menu

    /// THE EXCEPTION, STATED AS A TEST. The kernel stops LISTING pickup once
    /// every attack is covered while still ACCEPTING it, so Take reads the
    /// web's condition (defending, cards on the table) and not the menu. Asked
    /// here against a probe that lists NOTHING, which is exactly the covered
    /// table the menu goes quiet on.
    func testTakeIsOfferedOnATableTheKernelHasStoppedListing() {
        XCTAssertTrue(BoardActionMenu.resolve(kernelSaysNo, gates(isDefender: true)).canPickup,
                      "the defender of a fully covered table can no longer take it")
    }

    /// …and it is still only ever the defender's, over a table with cards on it.
    func testTakeBelongsToTheDefenderOfANonEmptyTable() {
        XCTAssertFalse(BoardActionMenu.resolve(kernelSaysYes, gates(isDefender: false)).canPickup,
                       "an attacker was offered Take")
        XCTAssertFalse(BoardActionMenu.resolve(kernelSaysYes,
                                               gates(isDefender: true, tableIsEmpty: true)).canPickup,
                       "Take was offered over an empty table")
        XCTAssertFalse(BoardActionMenu.resolve(kernelSaysYes,
                                               gates(isDefender: true, isOut: true)).canPickup,
                       "a seat that is out was offered Take")
    }

    /// Everything that stands Take down, one at a time - each of them a filmed
    /// report. `superseded` is called out in `pickup`'s own doc: because Take
    /// does not read the menu, standing `iCanAct` down does not reach it, so a
    /// read-only board would keep offering it.
    func testTakeStandsDownForTheHoldTheSelectionAndTheSupersede() {
        for (name, g) in [("the 15s throw-in hold", gates(pickupHeld: true, isDefender: true)),
                          ("a newer chain", gates(superseded: true, pickupHeld: false, isDefender: true)),
                          ("a staged move", gates(canSend: true, isDefender: true)),
                          ("a live selection", gates(isDefender: true, selectionIsEmpty: false))] {
            XCTAssertFalse(BoardActionMenu.resolve(kernelSaysNo, g).canPickup,
                           "Take survived \(name)")
        }
    }

    /// The stood-down board is a read-only board, and `.none` is what it is.
    func testASupersededSeatIsOfferedNothingAtAll() {
        XCTAssertEqual(BoardActionMenu.resolve(kernelSaysYes,
                                               gates(iCanAct: false, superseded: true,
                                                     isDefender: true)),
                       .none)
    }

    // MARK: - the Undo pill

    /// Nothing staged, nothing to take back.
    func testNoUndoPillWithNothingStaged() {
        for hides in [true, false] {
            XCTAssertEqual(BoardActionMenu.undoPill(canSend: false, retracting: false,
                                                    still: true, hides: hides),
                           .absent)
        }
    }

    /// Owner: "basically I never want to see a dimmed undo button." With
    /// `hides` on - which is what ships (`UndoGate.hidesByDefault`) - the pill
    /// is shown enabled or not shown, and `.dimmed` is unreachable.
    func testWithHidesOnThePillIsNeverDimmed() {
        XCTAssertTrue(UndoGate.hidesByDefault, "precondition: hiding is what ships")
        for retracting in [true, false] {
            for still in [true, false] {
                let pill = BoardActionMenu.undoPill(canSend: true, retracting: retracting,
                                                    still: still, hides: true)
                XCTAssertNotEqual(pill, .dimmed, "a dimmed Undo pill reached the screen")
                XCTAssertEqual(pill, (!retracting && still) ? .enabled : .absent)
            }
        }
    }

    /// The other build (`UndoGate.hides` off) draws it dimmed instead - which
    /// is the state the enum exists to keep reachable only there.
    func testWithHidesOffARefusedPillIsDimmedNotGone() {
        XCTAssertEqual(BoardActionMenu.undoPill(canSend: true, retracting: false,
                                                still: false, hides: false),
                       .dimmed)
        XCTAssertEqual(BoardActionMenu.undoPill(canSend: true, retracting: false,
                                                still: true, hides: false),
                       .enabled)
    }

    /// NOT WHILE A RETRACTION IS IN FLIGHT (the audit's U8). A tap during the
    /// conflict peek ran `undo`, found nothing to take back, and RE-STAGED the
    /// very chain being retracted. Owner: "let's disable the undo button during
    /// that then."
    func testARetractionInFlightRefusesTheUndoPill() {
        XCTAssertNotEqual(BoardActionMenu.undoPill(canSend: true, retracting: true,
                                                   still: true, hides: true),
                          .enabled,
                          "Undo was pressable while a conflict retraction flew")
        XCTAssertNotEqual(BoardActionMenu.undoPill(canSend: true, retracting: true,
                                                   still: true, hides: false),
                          .enabled)
    }
}
