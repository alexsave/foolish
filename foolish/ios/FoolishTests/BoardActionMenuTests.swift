// BoardActionMenuTests - the board's pill menu, driven without a board.
//
// MOST OF THESE USED TO BE SOURCE-TEXT SCANS in UndoGateTests, matching the
// exact spelling of an expression inside `MessageTableView.actionBar` - down to
// the line break in the middle of Take's condition. That was not a style
// choice: the decision lived inside a `some View`, so there was nothing to
// call, and a scan was the only thing available.
//
// THE PLAY PILLS ARE THE KERNEL'S NOW, WHOLE (legal.h play_pills): the board,
// the selection and the host's PLAY_HOST_* bits go in, the PLAY_PILL_* bits
// come out. Every case this file used to hold against a fake probe and a
// hand-filled `Gates` lives in c/tests/tests.c
// test_play_pills_one_move_one_button, marked [Swift], with the same position
// and the same expected pills - the filmed bug each one guards is named there.
// What is left here is the Swift boundary: the bits become the five Bools, and
// the view crosses as its own fields, in the right order.
//
// WHAT IS NOT HERE, and deliberately stays a source scan in UndoGateTests: that
// the pill column is redrawn on a timer, that `play` marks itself in flight
// BEFORE it clears the selection, and that the pill's action is
// `undoPillTapped`. Those are facts about the view, and a value cannot see them.

import XCTest
@testable import FoolishKit

final class BoardActionMenuTests: XCTestCase {

    // MARK: - the kernel's bits, as the five Bools

    /// Each PLAY_PILL_* bit is its own Bool and no other, and no bits is
    /// `.none`.
    func testEachPillBitIsItsOwnButton() {
        XCTAssertEqual(BoardActionMenu(pills: 0), .none)
        let bits = [PLAY_PILL_ATTACK, PLAY_PILL_COVER, PLAY_PILL_PASS, PLAY_PILL_PICKUP, PLAY_PILL_GOOD]
        for (i, bit) in bits.enumerated() {
            let m = BoardActionMenu(pills: UInt32(bit))
            XCTAssertEqual([m.canAttack, m.canCover, m.canPass, m.canPickup, m.canDone],
                           (0..<bits.count).map { $0 == i }, "pill bit \(bit) drew the wrong button")
        }
    }

    // MARK: - the view crosses as itself

    /// Seat 1 defends a 7 of spades on an open table; the menu is empty, so
    /// the only pill the kernel can draw is Take (legal.h: Take is not the
    /// menu's). `viewer`, `status` and the viewer's own status are the fields
    /// the kernel reads, so a crossing that swapped two of them changes the
    /// answer.
    private func board(viewer: Int, defender: Int = 1, seatStatus: Int = PLAYER_STATUS_IN,
                       gameStatus: Int = GAME_STATUS_PLAYING) -> GameView {
        let players = (0..<2).map { s in
            PlayerView(seat: s, name: "p\(s)", status: s == viewer ? seatStatus : PLAYER_STATUS_IN,
                       handCount: 6, awaitingAttack: false, strategyKey: 0, hand: s == viewer ? [] : nil)
        }
        return GameView(status: gameStatus, numPlayers: 2, powerSuit: 3, deckCount: 10, discardCount: 0,
                        hasFlipped: false, firstAttacker: 0, defender: defender, viewer: viewer,
                        goodMask: 0, gameOver: -1, flipped: nil,
                        battles: [BattleView(attack: Card(s: 0, v: 7), defense: nil)],
                        eliminationOrder: [], players: players)
    }

    private func pills(_ view: GameView, _ host: PlayWire.Host = [],
                       selection: [Card] = []) -> BoardActionMenu {
        BoardActionMenu(pills: PlayWire.pills(menu: MoveWire.emptyMenu, view: view,
                                              selection: selection, host: host))
    }

    func testTheDefenderIsTheViewsOwnComparison() {
        XCTAssertTrue(pills(board(viewer: 1)).canPickup, "the defender of a non-empty table lost Take")
        XCTAssertEqual(pills(board(viewer: 0)), .none, "the attacker was offered Take")
        XCTAssertEqual(pills(board(viewer: -1)), .none, "a spectator was offered Take")
    }

    func testTheStatusesCrossWhereTheKernelReadsThem() {
        XCTAssertEqual(pills(board(viewer: 1, seatStatus: PLAYER_STATUS_OUT)), .none,
                       "a seat that is out was offered Take")
        XCTAssertEqual(pills(board(viewer: 1, gameStatus: GAME_STATUS_GAME_OVER)), .none,
                       "a finished game's table was offered to the defender")
    }

    func testTheSelectionAndTheHostCrossAsGiven() {
        XCTAssertEqual(pills(board(viewer: 1), selection: [Card(s: 1, v: 6)]), .none,
                       "Take survived a live selection")
        for host: PlayWire.Host in [.staged, .inFlight, .moving, .superseded, .pickupHeld] {
            XCTAssertEqual(pills(board(viewer: 1), host), .none, "Take survived host bit \(host.rawValue)")
        }
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
