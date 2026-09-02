// ROUND 30 — A CARD LIFTS OFF THE TABLE FROM WHERE IT IS DRAWN.
//
// The owner, on 1.0(33), with two consecutive frames: "the layout of the cards
// on the table, for example two covers on two attacks, does not match the start
// of animation position for those same cards as they fly to discard. The cards
// very slightly jump, cover card to the right and attack card to the left."
//
// One line owns this, and round 12 and round 30 are OPPOSITE mistakes about it.
// `FBattleGrid` stacks a battle's two cards bottom-aligned and separates them
// with `.rotationEffect(anchor: .bottom)` alone - a render transform, which
// moves no layout frame - so both cards publish the SAME rect through
// `BattleCardFramesKey`. Three ways to build a flight from that:
//
//   rotate NEVER  - both ghosts start on the same spot, and every covered pair
//                   collapses into a stack (round 12's "rearranges into grid")
//   rotate TWICE  - the source rect is pre-swung AND the ghost swings it again,
//                   so the pair springs ~14pt apart as it lifts (this bug)
//   rotate ONCE   - the ghost alone rotates, and it lands exactly on the card
//
// Round 12 fixed the first by pre-swinging the source. `Flight.fromAngle` later
// gave the ghost its own rotation, which made that correction the second half
// of a double - and nothing failed, because both bugs look like "the cards are
// roughly right".
import XCTest
@testable import FoolishKit

final class SweepSourceTests: XCTestCase {

    private func card(_ id: String) -> Card {
        let p = id.split(separator: "-")
        return Card(s: Int(p[0])!, v: Int(p[1])!)
    }

    /// The grid's own geometry: one slot, two cards, bottom-aligned, same rect.
    private let slot = CGRect(x: 100, y: 200, width: 50, height: 70)

    private func table() -> (battles: [BattleView], frames: [String: CGRect]) {
        let attack = card("0-7"), defense = card("1-9"), lone = card("2-6")
        return ([BattleView(attack: attack, defense: defense),
                 BattleView(attack: lone, defense: nil)],
                [attack.identity: slot,
                 defense.identity: slot,
                 lone.identity: CGRect(x: 200, y: 200, width: 50, height: 70)])
    }

    /// THE FIX, stated as the thing that was wrong: both halves of a pair start
    /// from the SAME rect, and the tilts beside them are what separates the
    /// ghosts. A source that pre-swings returns two different rects and fails
    /// here; that is exactly the mutation.
    func testACoveredPairStartsFromOneRectAndIsSeparatedOnlyByItsTilt() {
        let (battles, frames) = table()
        let a = MessageTableView.tableSource(card("0-7"), battles: battles, frames: frames)
        let d = MessageTableView.tableSource(card("1-9"), battles: battles, frames: frames)
        XCTAssertEqual(a?.rect, slot, "the attack must lift from the rect it was laid out in")
        XCTAssertEqual(d?.rect, slot, "and so must the cover - the grid gives them the same one")
        XCTAssertEqual(a?.rect, d?.rect,
                       "a pre-swung source is the 1.0(33) jump: the pair springs apart as it lifts")
        XCTAssertEqual(a?.tilt, -FBattleGrid.coverAngle, "the attack under a cover lies back")
        XCTAssertEqual(d?.tilt, FBattleGrid.coverAngle, "the cover lies across")
    }

    /// …and the separation the ghost then produces is the separation the grid
    /// draws, because both rotate the same rect about the same pivot. Asserted
    /// as the arithmetic rather than trusted: this is the number that was
    /// doubled.
    func testTheGhostsRotationReproducesTheGridsSeparation() {
        let (battles, frames) = table()
        let a = MessageTableView.tableSource(card("0-7"), battles: battles, frames: frames)!
        let d = MessageTableView.tableSource(card("1-9"), battles: battles, frames: frames)!
        // Rotating about the bottom edge moves a card's centre sideways by
        // sin(angle) * halfHeight - the same thing FBattleGrid's rotationEffect
        // does to the drawn card, and FlyingCardsLayer's to the ghost.
        func swing(_ tilt: Double) -> Double { sin(tilt * .pi / 180) * (slot.height / 2) }
        let drawnApart = swing(d.tilt) - swing(a.tilt)
        XCTAssertEqual(drawnApart, 2 * swing(FBattleGrid.coverAngle), accuracy: 1e-9)
        XCTAssertEqual(drawnApart, 13.66, accuracy: 0.02,
                       "~14pt, which is also exactly how far the pair used to jump")
    }

    func testAnUncoveredAttackStandsUpright() {
        let (battles, frames) = table()
        let lone = MessageTableView.tableSource(card("2-6"), battles: battles, frames: frames)
        XCTAssertEqual(lone?.tilt, 0, "nothing is lying on it, so it does not lie back")
        XCTAssertEqual(lone?.rect, frames[card("2-6").identity])
    }

    /// A card the table never drew has no source here - the caller falls through
    /// to its own reconstruction rather than being handed a guess.
    func testACardTheTableNeverDrewHasNoSource() {
        let (battles, _) = table()
        XCTAssertNil(MessageTableView.tableSource(card("0-7"), battles: battles, frames: [:]))
    }

    /// A card on the table that belongs to no battle (a stale frame outliving its
    /// battle) is upright rather than nil - it still has a rect to fly from.
    func testACardWithNoBattleIsUprightRatherThanMissing() {
        let stray = card("3-13")
        let src = MessageTableView.tableSource(stray, battles: table().battles,
                                               frames: [stray.identity: slot])
        XCTAssertEqual(src?.tilt, 0)
        XCTAssertEqual(src?.rect, slot)
    }
}

// MARK: - ROUND 30: the replay waits for the sheet to arrive
//
// The wait itself is a private static on a SwiftUI view and cannot be driven
// from here; what CAN be pinned is the two things it rests on - a beat that is
// actually long enough to read, and a flag that defaults to "not moving" so a
// build where nothing ever sets it plays immediately instead of hanging.
@MainActor
final class SheetSettleTests: XCTestCase {

    func testTheBeatIsLongEnoughToNoticeAndShortEnoughToNotWaitOn() {
        XCTAssertGreaterThan(sheetSettleBeat, 0.1, "a beat under ~6 frames is not a beat")
        XCTAssertLessThan(sheetSettleBeat, 0.35, "any longer and the tap feels unresponsive")
    }

    /// Scales with `flightTime` like every other duration, so a filmed replay
    /// keeps its proportions under HARNESS_SLOWMO.
    func testTheBeatIsAFractionOfAFlightRatherThanAConstant() {
        XCTAssertEqual(sheetSettleBeat, flightTime * 0.4, accuracy: 1e-9)
    }

    /// THE FAIL-SAFE. `isPresenting` is a static set by a view controller the
    /// board cannot see; a host that never clears it, or a target that never
    /// sets it (the harness fakes presentation), must get today's behaviour and
    /// not a board that waits forever. Default false is half of that; the wait's
    /// own 600ms ceiling is the other half.
    func testNothingIsPresentingUntilAHostSaysSo() {
        XCTAssertFalse(CollapseTween.isPresenting,
                       "a board with no host must not believe a sheet is sliding")
    }
}
