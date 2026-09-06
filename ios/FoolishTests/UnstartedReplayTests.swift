// THE WINDOW IN WHICH THE VEIL IS UP - round 43.
//
// A tapped bubble opens with a replay outstanding. Until that replay STARTS,
// the board must render the game as it was BEFORE the move it is about to
// animate: the cards the move touches are drawn as not-yet-there, and the
// counts read their pre-move values. That is the veil, and `unstartedReplay` is
// the window it is up for.
//
// Two conditions, and both matter in opposite directions:
//
//   * `replayPending` - the controller still has a replay outstanding. When it
//     goes false the sequence has taken over, and the veil must come DOWN or
//     every card the replay touched stays hidden for the life of the board.
//   * a non-empty stream - there is something to hide FOR. A bubble that
//     carries no move (a §10 undo-to-empty re-seal, which seals
//     MSG_NEW_NOTHING) must not veil anything, or it opens on a board with
//     cards missing and nothing coming to put them back.
//
// WHY THIS FILE EXISTS, and it is not tidiness. When the window was collapsed
// out of `pendingOpen` and `pendingRoles` - which each re-derived it, one of
// them as two separate guards - the collapse was mutation-checked by deleting
// the `replayPending` half. **That mutant passed all 539 tests.** The half of
// the condition that decides whether the veil ever lifts was covered by
// nothing. The rule is a pure function of two controller values, so it is
// extracted and exercised here rather than left to a source scan.
//
// MUTATION-CHECKED: dropping the `replayPending` guard fails
// `testAStartedReplayClosesTheWindow`; dropping the empty-stream guard fails
// `testABubbleThatCarriesNoMoveVeilsNothing`; inverting `replayPending` fails
// three of the four.

import XCTest
@testable import FoolishKit

final class UnstartedReplayTests: XCTestCase {

    /// One event, enough to make a stream non-empty. Its contents are not read
    /// by the rule under test - only whether the array has any.
    private var oneEvent: [GameEvent] {
        [GameEvent(type: EventType.attackPass.rawValue, seat: 0, msg: 0, from: 0, to: 0,
                   cards: [nil], target: nil, battle: nil, state: nil)]
    }

    /// The veil is up: a replay is outstanding and it has something to hide.
    func testAnOutstandingReplayWithAMoveOpensTheWindow() {
        XCTAssertEqual(
            Veil.unstartedReplay(replayPending: true, events: oneEvent)?.count, 1,
            "a tapped bubble with a move to replay must veil it until the sequence starts")
    }

    /// THE HALF NOTHING WAS CHECKING. Once the sequence has taken over the
    /// window shuts, and `animator.hidden` plus the count overrides become the
    /// whole truth. Leave it open and the handoff never happens: the veil keeps
    /// answering, so every card the replay touched is hidden for good and the
    /// badges never reach their real values.
    func testAStartedReplayClosesTheWindow() {
        XCTAssertNil(
            Veil.unstartedReplay(replayPending: false, events: oneEvent),
            "once the replay has started the veil must come down - the animator owns "
            + "the cards now, and a window that stays open hides them for the life "
            + "of the board")
    }

    /// A bubble that carries no move veils nothing. This is the §10
    /// undo-to-empty re-seal, which the kernel seals as MSG_NEW_NOTHING: there
    /// is no move to animate, so a veil would take cards off the board with
    /// nothing scheduled to bring them back.
    func testABubbleThatCarriesNoMoveVeilsNothing() {
        XCTAssertNil(
            Veil.unstartedReplay(replayPending: true, events: []),
            "an empty stream has nothing to hide for")
    }

    /// Both off is the ordinary settled board, and stays shut.
    func testASettledBoardHasNoWindow() {
        XCTAssertNil(Veil.unstartedReplay(replayPending: false, events: []))
    }
}
