// THE CARDS A SUPERSEDED SEQUENCE LEAVES BEHIND.
//
// Owner, testing 1.0(17) with the compact drawer left open while bubbles
// arrived: "it is very frequently the case that it DOES NOT match the actual
// state, and you need to close and retap a bubble to get to the correct game
// state... sometimes a deal animation gets just dropped and it looks like I
// have 5 cards even when the deck is full", and "a simple 1 card cover move
// shows the card moving from the other player hand to the card, even rotating,
// but then just vanishes and the attack card it covered rotates back to 0
// degrees".
//
// All three are ONE defect, and it lives in the veil's two sets rather than in
// any animation. `preHide` puts a card in BOTH `preHidden` and `hidden`;
// `openSlots` takes it out of `preHidden` only, so its hand slot lays out while
// it stays invisible for its flight. `clearPreHidden` - the blanket net every
// teardown runs - subtracts `preHidden` from `hidden`, so once a card has been
// opened that net can no longer reach it. The ONLY thing that reveals it is the
// teardown's own rescue, and that teardown is skipped whole when a newer
// sequence has claimed the animator - which is exactly what a bubble arriving
// mid-flight does.
//
// So the card stayed in `hidden` for the life of the board: laid out, opacity
// 0. A dealt card that never appears is a hand one card short against a full
// deck; a landed cover that never appears is a card that "vanishes"; and since
// FBattleGrid reads `hidden \ preHidden` as "in flight right now", the attack
// underneath it goes on rendering as uncovered - the tilt coming off is the
// pair collapsing, not an animation running backwards.
import XCTest
@testable import FoolishKit

final class SequenceTeardownTests: XCTestCase {

    private let a = "a", b = "b", c = "c"

    /// THE INVARIANT THE BUG RESTS ON, asked of the animator itself: a card that
    /// has been OPENED is beyond `clearPreHidden`'s reach. This is not a defect -
    /// it is what lets a later step's prediction survive an earlier step's
    /// teardown - but it is the reason an orphan cannot be mopped up by the
    /// blanket net and has to be carried explicitly.
    @MainActor
    func testAnOpenedCardIsBeyondTheBlanketNet() {
        let animator = BoardAnimator()
        animator.preHide([a, b])
        XCTAssertTrue(animator.isHidden(a) && animator.isHidden(b))

        animator.openSlots([a])          // a's slot lays out; a stays invisible
        animator.clearPreHidden()        // the net every teardown runs

        XCTAssertFalse(animator.isHidden(b), "the net did not hand back an unopened card")
        XCTAssertTrue(animator.isHidden(a),
                      "an opened card is reachable by clearPreHidden - the orphan cannot arise")
        // …and the targeted twin is what does reach it.
        animator.reveal([a])
        XCTAssertFalse(animator.isHidden(a))
    }

    /// THE FIX, as a rule. A superseded sequence must reveal nothing - the one
    /// that replaced it has cards pre-hidden for flights it has not made, and
    /// revealing those is the double animation the token guard exists to
    /// prevent - but it must hand its own opens ON.
    func testASupersededSequenceHandsItsOpensOnInsteadOfDroppingThem() {
        let owed = MessageTableView.sequenceTeardown(opened: [a, b], orphaned: [c],
                                                     isNewest: false)
        XCTAssertTrue(owed.reveal.isEmpty,
                      "a superseded sequence revealed cards the newer one may be about to fly")
        XCTAssertEqual(owed.carry, [a, b, c],
                       "the opens this sequence never flew were dropped, not carried")
    }

    /// …and the last sequence standing settles the whole debt: its own opens and
    /// every orphan handed to it. Nothing may still be carried afterwards -
    /// there is no later sequence to carry it to.
    func testTheLastSequenceStandingRevealsEverythingOwed() {
        let owed = MessageTableView.sequenceTeardown(opened: [a], orphaned: [b, c],
                                                     isNewest: true)
        XCTAssertEqual(owed.reveal, [a, b, c],
                       "the last sequence left a card hidden that nothing will ever fly")
        XCTAssertTrue(owed.carry.isEmpty, "the debt was carried past the last sequence")
    }

    /// The chain a real arrival makes: one sequence opens a deal and is cut off,
    /// a second opens its own and is cut off too, and the third finishes. Every
    /// card any of them opened must be visible at the end.
    func testAChainOfSupersedesLosesNothing() {
        var carried: Set<String> = []
        for opened in [Set([a]), Set([b])] {
            carried = MessageTableView.sequenceTeardown(opened: opened, orphaned: carried,
                                                        isNewest: false).carry
        }
        let final = MessageTableView.sequenceTeardown(opened: [c], orphaned: carried,
                                                      isNewest: true)
        XCTAssertEqual(final.reveal, [a, b, c],
                       "a card opened two sequences ago never came back")
        XCTAssertTrue(final.carry.isEmpty)
    }
}
