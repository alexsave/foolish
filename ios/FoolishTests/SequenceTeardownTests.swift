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

    // REAL card identities, because the veil now speaks the kernel's dense ids
    // (anim_veil_teardown / anim_veil_handover over a u64 bitset) and a card
    // that is not a card has no bit. The labels were always arbitrary - what
    // these tests assert is the set relationship - so this is the same
    // expectation with cards that can actually be dealt.
    private let a = Card(s: 0, v: 6).identity
    private let b = Card(s: 1, v: 9).identity
    private let c = Card(s: 3, v: 13).identity

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
        // The net every teardown runs. `raisedBy: veilEpoch` is "everything
        // standing right now", i.e. exactly what the old blanket did - this
        // sequence is the only one that has veiled anything.
        animator.clearPreHidden(raisedBy: animator.veilEpoch)

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
        let owed = Veil.teardown(opened: [a, b], orphaned: [c],
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
        let owed = Veil.teardown(opened: [a], orphaned: [b, c],
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
            carried = Veil.teardown(opened: opened, orphaned: carried,
                                    isNewest: false).carry
        }
        let final = Veil.teardown(opened: [c], orphaned: carried,
                                  isNewest: true)
        XCTAssertEqual(final.reveal, [a, b, c],
                       "a card opened two sequences ago never came back")
        XCTAssertTrue(final.carry.isEmpty)
    }

    // MARK: - round 40 - a teardown may only take down its OWN veil

    /// THE THEFT, filmed in the rig on the first goodend run (a live play made
    /// while an open-replay was still winding down):
    ///
    ///     veil preHide [1-11]                      <- the tap
    ///     held ghost at source [1-11]
    ///     stream#1 end
    ///     veil clearPreHidden (pop IN) [1-11]      <- the theft
    ///     grid ... visible=1 hidden=0              <- the card PAINTS on the table
    ///     fan-rows 1 rows laid=5 hand=5 ... seq=0
    ///     flight START [1-11:47,623->187,338]
    ///     grid ... visible=0 hidden=1              <- …and vanishes again
    ///
    /// The token guard cannot catch this one: a LIVE PLAY veils its cards from
    /// `playAt` and never claims `animSequenceToken` (there is no sequence to
    /// claim it for - the flight does not exist until the kernel has published
    /// a table slot to fly to), so the winding-down stream still passes
    /// `mySeq == animSequenceToken`, takes the "newest" branch and runs the
    /// blanket net over a veil that was not its to take down.
    ///
    /// Owner: "still seeing the hand card rows twitch during the discard
    /// animation." The fan is CENTRED, so a card popping back into it and being
    /// hidden again a beat later re-lays out the whole row, twice.
    @MainActor
    func testATeardownCannotTakeDownAVeilRaisedAfterIt() {
        let animator = BoardAnimator()
        animator.preHide([a])                 // the running sequence's own veil
        let mine = animator.veilEpoch         // …the mark it captured when it claimed
        animator.preHide([b])                 // the player taps a card mid-sequence

        animator.clearPreHidden(raisedBy: mine)

        XCTAssertFalse(animator.isHidden(a), "the sequence did not hand back its own prediction")
        XCTAssertTrue(animator.isHidden(b),
                      "a sequence winding down stripped the veil off a card the player "
                      + "had just played - it pops into the fan, re-centres the row, and "
                      + "is hidden again a beat later when its flight starts")
    }

    /// …and the stamp must not become a way to STRAND a card. Everything a
    /// sequence veiled itself is still handed back by its own teardown,
    /// whatever went up afterwards - the guarantee `clearPreHidden` exists for.
    @MainActor
    func testASequenceStillHandsBackEverythingItVeiledItself() {
        let animator = BoardAnimator()
        animator.preHide([a, b])              // two predictions, one call
        animator.preHide([c])                 // …and a third, still this sequence's
        let mine = animator.veilEpoch

        animator.clearPreHidden(raisedBy: mine)

        XCTAssertFalse(animator.isHidden(a) || animator.isHidden(b) || animator.isHidden(c),
                       "a prediction this sequence made itself was left veiled for good")
    }

    /// The later veil is not stranded either: its own owner takes it down, and
    /// by then the mark has moved past it.
    @MainActor
    func testTheLaterVeilComesDownWithItsOwnTeardown() {
        let animator = BoardAnimator()
        animator.preHide([a])
        let first = animator.veilEpoch
        animator.preHide([b])
        let second = animator.veilEpoch

        animator.clearPreHidden(raisedBy: first)
        XCTAssertTrue(animator.isHidden(b))
        animator.clearPreHidden(raisedBy: second)
        XCTAssertFalse(animator.isHidden(b), "the later veil had no owner left to take it down")
    }

    // MARK: - round 40 - one ledger, one veil

    /// `pendingPlacement` is a ONE-SLOT ledger and the live-play veil is keyed
    /// off it, so a second play landing before the first's view change used to
    /// disown the first play's cards outright: nothing was left holding their
    /// ids, and they stayed in `preHidden` for the life of the board - laid out
    /// nowhere, excluded from a centred fan. `apply` is awaited, so the whole
    /// kernel round trip is a window a second tap fits inside.
    func testASecondPlayHandsBackTheVeilItDisowns() {
        let first = Card(s: 2, v: 4).identity, second = Card(s: 3, v: 2).identity
        let owed = Veil.handover(standing: [first], placing: [second])
        XCTAssertEqual(owed.reveal, [first],
                       "the play that lost the ledger kept its veil - its card is laid out nowhere")
        XCTAssertEqual(owed.veil, [second])
    }

    /// …and a card BOTH plays name (a re-play of the same selection) is not
    /// flashed back into the fan on the way past: it is veiled either way.
    func testACardCarriedIntoTheNewPlayIsNotRevealedOnTheWay() {
        let owed = Veil.handover(standing: [a, b], placing: [b, c])
        XCTAssertEqual(owed.reveal, [a], "only the disowned card comes back")
        XCTAssertFalse(owed.reveal.contains(b), "a card the new play also veils must not flash in")
        XCTAssertEqual(owed.veil, [b, c])
    }
}
