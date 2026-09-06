// CoverTiltTests — "when I cover and the cover is replayed, the covered card
// very quickly goes from a rotated orientation back to centre, then rotates
// again once the cover is sent. It should start unrotated." Round 4: "I see
// that the first cover rotates a bit as soon as we load." And 1.0(47), the
// same sentence a third time, on the bout-ending cover: "I briefly saw the
// uncovered cards animate from rotated back to straight, then rotate back as
// my cover cards flew to cover."
//
// THE THIRD ONE GOT THROUGH BECAUSE THIS FILE TESTED THE WRONG FUNCTION. It
// asserted `FBattleGrid.coverLanded`, which round 7 replaced with `coverTilted`
// (a cover tilts in lockstep with its flight now, not once it lands) and left
// with no caller at all. Every test below passed against a rule the board had
// stopped asking. So they ask `coverTilted` now, and the sweep case — where
// the hole actually was — asks it through the same two derivations the board
// composes: `MessageTableView.pendingSweepUnplaced` and `Veil.grid`.
import XCTest
@testable import FoolishKit

final class CoverTiltTests: XCTestCase {

    private let attack = Card(s: 0, v: 10)
    private let defense = Card(s: 0, v: 12)

    /// The bug, as the states a pair passes through when a board opens on a
    /// table that is ALREADY covered. Only the first was ever wrong.
    func testAReplayedCoverIsUprightUntilItFlies() {
        // 1. first paint. The replay has not started, but the veil already
        //    names the cover as not-yet-there, so the pair is upright — this is
        //    precisely where it used to flash tilted.
        XCTAssertFalse(FBattleGrid.coverTilted(defense: defense,
                                               hidden: [defense.identity], flyingNow: []),
                       "a board that opens on a covered table must paint UPRIGHT")
        // 2. the cover's flight has STARTED: it tilts with the card coming down,
        //    not after it (round 7).
        XCTAssertTrue(FBattleGrid.coverTilted(defense: defense,
                                              hidden: [defense.identity],
                                              flyingNow: [defense.identity]),
                      "the attack rotates in lockstep with the cover's flight")
        // 3. landed.
        XCTAssertTrue(FBattleGrid.coverTilted(defense: defense, hidden: [], flyingNow: []),
                      "tilted once the cover has landed")
    }

    /// THE BOUT-ENDING COVER (1.0(47)), through the board's own composition.
    ///
    /// The final view has no table at all — it was swept — so the replay's
    /// table IS the pre-bout grid, and that grid comes out of the kernel with
    /// the cover already lying on it. `battlesArea` renders it synchronously so
    /// the board does not blink empty, and the veil for it has to be derived on
    /// that same paint: `sweepUnplaced` is written by the `onChange` that starts
    /// the sequence, one paint later.
    func testABoutEndingCoverOpensUprightOnTheSweepGrid() {
        let battles = [BattleView(attack: attack, defense: defense)]
        // What the replay PLACES: the cover, and the attack it lands on is
        // already lying there from an earlier bubble.
        let unplaced = MessageTableView.pendingSweepUnplaced(placed: [defense.identity],
                                                            table: battles)
        XCTAssertEqual(unplaced, [defense.identity],
                       "the pre-bout grid is waiting for the cover, not the attack under it")

        let grid = Veil.grid(sweeping: true, veiled: [], sweptFlown: [],
                             sweepUnplaced: unplaced, sweepArriving: [], flying: [])
        XCTAssertFalse(FBattleGrid.coverTilted(defense: defense,
                                               hidden: grid.hidden, flyingNow: grid.flyingNow),
                       "the first paint of a bout-ending cover replay must be UPRIGHT")

        // …and once that cover is in the air, the attack rotates with it.
        let flying = Veil.grid(sweeping: true, veiled: [], sweptFlown: [],
                               sweepUnplaced: unplaced,
                               sweepArriving: [defense.identity], flying: [])
        XCTAssertTrue(FBattleGrid.coverTilted(defense: defense,
                                              hidden: flying.hidden, flyingNow: flying.flyingNow),
                      "the sweep grid tilts the attack as its cover comes down")
    }

    /// A placement the pre-bout grid holds no slot for is somewhere else — a
    /// pickup's cards go to a hand. Veiling those would carry off a table with
    /// holes in it.
    func testOnlyWhatTheGridHoldsASlotForIsWaitedFor() {
        let battles = [BattleView(attack: attack, defense: defense)]
        let elsewhere = Card(s: 3, v: 13)
        XCTAssertEqual(MessageTableView.pendingSweepUnplaced(placed: [elsewhere.identity],
                                                             table: battles), [])
        XCTAssertEqual(MessageTableView.pendingSweepUnplaced(placed: [defense.identity],
                                                             table: []), [],
                       "no pending table, nothing to wait on")
    }

    /// Round-4 note 3, second half. A cover this open does NOT replay — one from
    /// an earlier bubble, already on the table before I looked — is never in
    /// `hidden`, so it is tilted from the very first paint and never animates
    /// its tilt. The old global "have we settled yet" gate got this wrong: it
    /// forced EVERY cover upright for a paint, so untouched ones visibly rotated
    /// into place on load.
    func testACoverFromAnEarlierBubbleIsTiltedOnTheFirstPaint() {
        XCTAssertTrue(FBattleGrid.coverTilted(defense: defense, hidden: [], flyingNow: []),
                      "a cover that is not being replayed must not rotate on load")
    }

    /// An uncovered attack is upright in every state — nothing above may tilt a
    /// battle that has no defender.
    func testAnUncoveredAttackNeverTilts() {
        for hidden in [Set<String>(), [attack.identity], [defense.identity]] {
            XCTAssertFalse(FBattleGrid.coverTilted(defense: nil, hidden: hidden, flyingNow: []))
            XCTAssertFalse(FBattleGrid.coverTilted(defense: nil, hidden: hidden,
                                                   flyingNow: [defense.identity]))
        }
    }

    /// A static render (a bubble snapshot, the gallery, the offline board)
    /// passes no `hidden` at all and must show covers laid across.
    func testAStaticRenderShowsCoversLanded() {
        let grid = FBattleGrid(battles: [], trumpSuit: nil)
        XCTAssertTrue(grid.hidden.isEmpty,
                      "a render with no replay behind it hides nothing, so nothing waits")
    }

    /// A cover hidden for a DIFFERENT reason than its own flight (some other
    /// card in the same step) must not tilt either — `hidden` is the authority,
    /// not "is this the card we just played".
    func testAnyHiddenCoverIsUpright() {
        let other = Card(s: 1, v: 6)
        XCTAssertFalse(FBattleGrid.coverTilted(defense: defense,
                                               hidden: [other.identity, defense.identity],
                                               flyingNow: []))
        XCTAssertTrue(FBattleGrid.coverTilted(defense: defense,
                                              hidden: [other.identity], flyingNow: []))
    }
}
