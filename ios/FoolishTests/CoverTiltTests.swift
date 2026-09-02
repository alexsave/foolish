// CoverTiltTests — "when I cover and the cover is replayed, the covered card
// very quickly goes from a rotated orientation back to centre, then rotates
// again once the cover is sent. It should start unrotated." And round 4: "I see
// that the first cover rotates a bit as soon as we load."
//
// The tilt is a pure function of one input (FBattleGrid.coverLanded over the
// board's `hidden` set), so the whole reported sequence can be asserted as a
// sequence instead of eyeballed on a simulator: the board's first paint, the
// flight, the landing. What makes that ONE input enough is that the caller puts
// the not-yet-replayed cards into `hidden` too — see MessageTableView's
// `veiledCardIds`, and VeilTests for the derivation itself.
import XCTest
@testable import FoolishKit

final class CoverTiltTests: XCTestCase {

    private let attack = Card(s: 0, v: 10)
    private let defense = Card(s: 0, v: 12)

    /// The bug, as the three states it passes through when a board opens on a
    /// table that is ALREADY covered. Only the first one was ever wrong.
    func testAReplayedCoverIsUprightUntilItLands() {
        // 1. first paint. The board has not started the replay, but the veil
        //    already names the cover as not-yet-there, so this is upright —
        //    this is precisely where it used to flash tilted.
        XCTAssertFalse(FBattleGrid.coverLanded(defense: defense, hidden: [defense.identity]),
                       "a board that opens on a covered table must paint UPRIGHT")
        // 2. the animator has taken over and the cover is in flight. Same
        //    input, same answer — which is why the handoff is invisible.
        XCTAssertFalse(FBattleGrid.coverLanded(defense: defense, hidden: [defense.identity]),
                       "still upright while the cover flies in")
        // 3. the flight finished.
        XCTAssertTrue(FBattleGrid.coverLanded(defense: defense, hidden: []),
                      "tilted once the cover has landed")
    }

    /// Round-4 note 3, second half. A cover this open does NOT replay — one
    /// from an earlier bubble, already on the table before I looked — is never
    /// in `hidden`, so it is tilted from the very first paint and never
    /// animates its tilt. The old global "have we settled yet" gate got this
    /// wrong: it forced EVERY cover upright for a paint, so the untouched ones
    /// visibly rotated into place on load.
    func testACoverFromAnEarlierBubbleIsTiltedOnTheFirstPaint() {
        XCTAssertTrue(FBattleGrid.coverLanded(defense: defense, hidden: []),
                      "a cover that is not being replayed must not rotate on load")
    }

    /// An uncovered attack is upright in every state — nothing above may tilt a
    /// battle that has no defender.
    func testAnUncoveredAttackNeverTilts() {
        for hidden in [Set<String>(), [attack.identity], [defense.identity]] {
            XCTAssertFalse(FBattleGrid.coverLanded(defense: nil, hidden: hidden))
        }
    }

    /// A static render (a bubble snapshot, the gallery, the offline board)
    /// passes no `hidden` at all and must show covers laid across.
    func testAStaticRenderShowsCoversLanded() {
        let grid = FBattleGrid(battles: [], trumpSuit: nil)
        XCTAssertTrue(grid.hidden.isEmpty,
                      "a render with no replay behind it hides nothing, so nothing waits")
    }

    /// A cover that is hidden for a DIFFERENT reason than its own flight (some
    /// other card in the same step) must not tilt either — `hidden` is the
    /// authority, not "is this the card we just played".
    func testAnyHiddenCoverIsUpright() {
        let other = Card(s: 1, v: 6)
        XCTAssertFalse(FBattleGrid.coverLanded(defense: defense,
                                               hidden: [other.identity, defense.identity]))
        XCTAssertTrue(FBattleGrid.coverLanded(defense: defense, hidden: [other.identity]))
    }
}
