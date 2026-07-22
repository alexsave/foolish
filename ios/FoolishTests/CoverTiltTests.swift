// CoverTiltTests — "when I cover and the cover is replayed, the covered card
// very quickly goes from a rotated orientation back to centre, then rotates
// again once the cover is sent. It should start unrotated."
//
// The tilt is a pure function of three things (FBattleGrid.coverLanded), so the
// whole reported sequence can be asserted as a sequence instead of eyeballed on
// a simulator: the board's first paint, the flight, the landing.
import XCTest
@testable import FoolishKit

final class CoverTiltTests: XCTestCase {

    private let attack = Card(s: 0, v: 10)
    private let defense = Card(s: 0, v: 12)

    /// The bug, as the three states it passes through when a board opens on a
    /// table that is ALREADY covered. Only the first one was ever wrong.
    func testAReplayedCoverIsUprightUntilItLands() {
        // 1. first paint — the pre-hide has not run yet (it runs from an
        //    onChange, one paint later). Nothing is hidden, and the model
        //    already says covered: this is precisely where it used to tilt.
        XCTAssertFalse(FBattleGrid.coverLanded(defense: defense, hidden: [], coversSettled: false),
                       "a board that opens on a covered table must paint UPRIGHT")
        // 2. the pre-hide landed and the cover is in flight.
        XCTAssertFalse(FBattleGrid.coverLanded(defense: defense, hidden: [defense.identity],
                                               coversSettled: true),
                       "still upright while the cover flies in")
        // 3. the flight finished.
        XCTAssertTrue(FBattleGrid.coverLanded(defense: defense, hidden: [], coversSettled: true),
                      "tilted once the cover has landed")
    }

    /// An uncovered attack is upright in every state — nothing above may tilt a
    /// battle that has no defender.
    func testAnUncoveredAttackNeverTilts() {
        for settled in [false, true] {
            for hidden in [Set<String>(), [attack.identity], [defense.identity]] {
                XCTAssertFalse(FBattleGrid.coverLanded(defense: nil, hidden: hidden,
                                                       coversSettled: settled))
            }
        }
    }

    /// The suppression is for the first-paint window ONLY: a static render (a
    /// bubble snapshot, the gallery, the offline board) passes no `coversSettled`
    /// at all and must still show covers laid across. This is the assertion that
    /// fails if the default is ever flipped to false "for safety".
    func testAStaticRenderShowsCoversLandedByDefault() {
        let grid = FBattleGrid(battles: [], trumpSuit: nil)
        XCTAssertTrue(grid.coversSettled,
                      "a render with no replay behind it must not wait for one")
    }

    /// A cover that is hidden for a DIFFERENT reason than its own flight (some
    /// other card in the same step) must not tilt either — `hidden` is the
    /// authority once settled, not "is this the card we just played".
    func testAnyHiddenCoverIsUpright() {
        let other = Card(s: 1, v: 6)
        XCTAssertFalse(FBattleGrid.coverLanded(defense: defense,
                                               hidden: [other.identity, defense.identity],
                                               coversSettled: true))
        XCTAssertTrue(FBattleGrid.coverLanded(defense: defense, hidden: [other.identity],
                                              coversSettled: true))
    }
}
