// THE CHROME'S THREE ANIMATION OVERRIDES - one of them is a rule, two are not.
//
// `boardContent` animates on `.animation(FMotion.cardMotion, value:
// controller.view)`, and five pieces of chrome inside that scope must not move
// on it (round 7: "buttons should NEVER move / float"). The override that
// works is a NESTED same-trigger `.animation(nil, value: controller.view)`, and
// it was written out by hand at all five sites - the send hint, the promoted
// self role mark, the action column, the undo slot and the settings/help
// squares. One rule, five copies, and the why written down at only some of
// them; `doesNotRideTheBoardSpring` is the one place it lives now.
//
// WHY A SOURCE TEST. The thing being protected is a SwiftUI modifier chain on a
// view that needs a laid-out board, a `GameView` and a live controller before
// any of it means anything, and what actually went wrong here was never a
// value - it was five hand-spellings of one rule and a comment record that
// invited the wrong edit. Both are facts about the SOURCE. Same technique, and
// the same reason, as HandLandingTests and CountOwnershipTests next door.
//
// AND WHY IT ALSO GUARDS THE OTHER TWO. The trio reads like a band-aid on a
// band-aid on a band-aid and has been misread that way, so this pins that
// `actionBar` keeps its `.transaction { $0.animation = nil }` (vector 2: the
// INSERTION of the Undo pill, which nothing keyed on `controller.view` can
// see - round 10g filmed it arriving ~295pt above its slot) and that the
// `buttonLift` mirror keeps its explicit `FMotion.chrome` (vector 3: the
// chrome's own honest slide on a row-count change - round 36 asked for it).
// Neither is redundant with vector 1 and neither can be deleted quietly.
//
// COMMENT LINES ARE STRIPPED BEFORE SCANNING. These files carry more prose than
// code and every rule below is also NAMED in the prose around it - a whole-file
// scan reports each rule as a violation of itself, which is the trap
// HandLandingTests' own header records walking into.
//
// MUTATION-CHECKED, seven mutants, all red before the code went back:
//
//   1. the undo slot re-spelled by hand as `.animation(nil, value:
//      controller.view)` - red on `testTheChromeSpendsTheOneModifier` AND on
//      `testEverySitePassesTheAncestorsTrigger` (it drops the count to four)
//   2. the settings/help site handed `lift` instead of `controller.view`, i.e.
//      the `handHeight` mistake round 7 already made - red on
//      `testEverySitePassesTheAncestorsTrigger`
//   3. the modifier's body swapped for `transaction { $0.animation = nil }`,
//      the spelling that was tried and rejected - red on
//      `testTheModifierIsTheNestedSameTriggerOverride`
//   4. `actionBar`'s `.transaction` deleted - red on
//      `testTheActionColumnKeepsItsTransaction`
//   5. the mirror's `withAnimation(FMotion.chrome)` reduced to a bare
//      assignment, undoing round 36's slide - red on
//      `testTheChromeMirrorKeepsItsOwnAnimation`
//   6. the empty `Color.clear` placeholder put back in the ZStack - red on
//      `testNoEmptyRoleMarkSlotComesBack`
//   7. round 36 undone the other way: the `.transaction` hoisted back above
//      the 128x88 box, where it also nils the chrome's slide - red on
//      `testTheActionColumnKeepsItsTransaction`

import XCTest
@testable import FoolishKit

final class BoardSpringOverrideTests: XCTestCase {

    private func source() throws -> String {
        let here = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
        return try String(contentsOf: here.deletingLastPathComponent()
            .appendingPathComponent("FoolishKit/Boards/MessageTableView.swift"), encoding: .utf8)
    }

    /// Lines of `src` that are code, not comment.
    private func code(_ src: String) -> [String] {
        src.split(separator: "\n", omittingEmptySubsequences: false)
            .map(String.init)
            .filter { !$0.trimmingCharacters(in: .whitespaces).hasPrefix("//") }
    }

    /// The single place the rule is allowed to be spelled: the modifier's body.
    private func modifierBody(_ lines: [String]) throws -> [String] {
        let head = try XCTUnwrap(lines.firstIndex { $0.contains("func doesNotRideTheBoardSpring") },
                                 "no `doesNotRideTheBoardSpring` - the rule has no home")
        return Array(lines[head...].prefix(4))
    }

    /// The mechanism, not just the name: nilling the animation for the SAME
    /// trigger the ancestor animates on. A modifier that did anything else
    /// would leave five happy-looking call sites and a floating action bar.
    func testTheModifierIsTheNestedSameTriggerOverride() throws {
        let body = try modifierBody(code(try source())).joined(separator: "\n")
        XCTAssertTrue(body.contains("animation(nil, value: trigger)"),
                      "`doesNotRideTheBoardSpring` must nil the animation for the trigger it "
                      + "is handed. The override only works because the trigger is the SAME "
                      + "value the ancestor's scoped `.animation(cardMotion, value:)` uses - "
                      + "innermost wins. Anything else silently stops overriding.")
    }

    /// Nobody re-spells it by hand. Five copies of one rule is what this
    /// replaced, and a sixth site written out longhand is a site whose reason
    /// is not written down anywhere.
    func testTheChromeSpendsTheOneModifier() throws {
        let lines = code(try source())
        let mine = try modifierBody(lines)
        let inlined = lines.filter { $0.contains(".animation(nil, value:") && !mine.contains($0) }
        XCTAssertTrue(inlined.isEmpty,
                      "a chrome site re-spells the board-spring override by hand: "
                      + "\(inlined.map { $0.trimmingCharacters(in: .whitespaces) }). "
                      + "Use `.doesNotRideTheBoardSpring(controller.view)` - the reason this "
                      + "override exists, the two spellings that were tried and rejected, and "
                      + "the fact that it is one of THREE different vectors all live in that "
                      + "modifier's doc, and only there.")
    }

    /// The five sites, and every one of them handing over the ancestor's own
    /// trigger. `handHeight` was tried and is the wrong value - the change
    /// arrives through `controller.view`.
    func testEverySitePassesTheAncestorsTrigger() throws {
        let lines = code(try source())
        let calls = lines.filter { $0.contains(".doesNotRideTheBoardSpring(") }
        XCTAssertEqual(calls.count, 5,
                       "expected the five chrome sites (send hint, self role mark, action "
                       + "column, undo slot, settings/help squares) to spend the modifier")
        for c in calls {
            XCTAssertTrue(c.contains(".doesNotRideTheBoardSpring(controller.view)"),
                          "the trigger must be `controller.view`, the value the board's card "
                          + "spring itself animates on: \(c.trimmingCharacters(in: .whitespaces))")
        }
    }

    /// VECTOR 2, and it is not the same thing as vector 1. It covers the Undo
    /// pill being INSERTED into the action column, which is not driven by
    /// `controller.view` at all, so no `.animation(nil, value:)` can reach it.
    /// It must stay on the fixed-size container: round 36 narrowed it there
    /// because outermost it also nulled the chrome's own slide (vector 3).
    func testTheActionColumnKeepsItsTransaction() throws {
        let lines = code(try source())
        let bar = try XCTUnwrap(lines.firstIndex { $0.contains("actionBar(view)") },
                                "no `actionBar(view)` placement")
        let block = lines[bar...].prefix(12)
        let box = try XCTUnwrap(block.firstIndex { $0.contains(".frame(width: 128, height: 88") },
                                "the action column's fixed-size container is gone - the "
                                + "transaction below is scoped to exactly it")
        let txn = try XCTUnwrap(block.firstIndex { $0.contains(".transaction { $0.animation = nil }") },
                                "`actionBar`'s `.transaction` is gone. It is NOT redundant with "
                                + "`.doesNotRideTheBoardSpring`: it covers the INSERTION of the "
                                + "Undo pill into the column, a change `controller.view` does not "
                                + "drive. Round 10g filmed Undo arriving ~295pt above its slot.")
        XCTAssertTrue(box < txn,
                      "the transaction must wrap the 128x88 container, not the placement. "
                      + "Outermost it also nils the board-level padding that carries the chrome "
                      + "up when the hand grows a second row - which is round 36's slide.")
    }

    /// VECTOR 3. The mirror is the chrome's OWN animation for the one change
    /// that genuinely moves it. Explicit on purpose: an ambient animation here
    /// would be the card spring again, and `.onChange`'s own transaction
    /// carries none, which is why the value used to snap.
    func testTheChromeMirrorKeepsItsOwnAnimation() throws {
        let src = code(try source()).joined(separator: "\n")
        XCTAssertTrue(src.contains("withAnimation(FMotion.chrome) { buttonLift = h }"),
                      "the `buttonLift` mirror must animate its own row-count change with "
                      + "`FMotion.chrome`. Round 36: \"at least make it slide smoothly instead "
                      + "of jumping\". It is deliberately NOT driven by `controller.view`, so "
                      + "the `.doesNotRideTheBoardSpring` sites cannot null it.")
    }

    /// Round 41 moved the self role mark out of the ZStack and into an overlay
    /// above the flight layer; the empty `Color.clear` slot it left behind, at
    /// `lift + 6`, drew nothing and is gone. A transparent non-hit-testing
    /// child has no effect but the z-slot it occupies, and dropping a child
    /// does not reorder the ones after it - all four siblings that followed it
    /// fill and align themselves.
    func testNoEmptyRoleMarkSlotComesBack() throws {
        let lines = code(try source())
        XCTAssertFalse(lines.contains { $0.contains("lift + 6") },
                       "something is back at the board ZStack's old role-mark line. The mark "
                       + "is drawn from the `selfRoleIndicator` overlay (round 41) and reads "
                       + "`statusMarkLift + 6` there; the `lift + 6` slot in the ZStack was an "
                       + "empty `Color.clear` that drew nothing, and a transparent "
                       + "non-hit-testing child holds nothing open.")
    }
}
