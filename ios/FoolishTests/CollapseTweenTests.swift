// CollapseTweenTests — the drawer collapse, replayed instead of re-filmed.
//
// The report (owner, 1.0(22)): "i saw that one time the lobby screen collapsed,
// and it collapsed to a height even shorter than the normal collapsed height.
// was super weird." Hard to reproduce on purpose, because whether it happens at
// all depends on the ORDER the host reports its transition heights in - and that
// order is Messages', not ours. So the sequence is the fixture: the one filmed
// in round 10d and written into `MessagesRootView.follow`'s own comment,
// 748 -> 315 -> 307 -> 778 -> 758 -> 253 -> 315, driven through the rule in both
// the harmless order and the one that produced the report.
//
// The invariant under test is a single sentence: while the box is holding its
// own height, that height is never less than the tallest compact height the host
// has reported. A box taller than the drawer is clipped and invisible; a box
// shorter exposes what is behind it, which is the bug.

import XCTest
@testable import FoolishKit

final class CollapseTweenTests: XCTestCase {

    /// The view's half of the loop, so a test can drive the same state machine
    /// `follow` drives without a SwiftUI view around it.
    private struct Box {
        var armed = false
        var armedFrom: CGFloat = 0
        var collapsing = false
        var target: CGFloat = 0
        /// nil = following the model box exactly.
        var height: CGFloat?

        mutating func report(_ h: CGFloat) {
            switch CollapseTween.step(height: h, armed: armed, armedFrom: armedFrom,
                                      collapsing: collapsing, target: target) {
            case .start(let from, let to):
                armed = false; collapsing = true; target = to
                _ = from            // the tween's start; irrelevant to where it rests
                height = to
            case .retarget(let to):
                target = to; height = to
            case .hold:
                break
            case .follow:
                height = nil
            }
        }
    }

    private let filmed: [CGFloat] = [748, 315, 307, 778, 758, 253, 315]

    /// THE BUG, in the order that produces it: a low noisy report arrives before
    /// the drawer's real rest height, so the tween aims at 253 - and every later
    /// report, including the 315 that says where the drawer actually stopped,
    /// used to be ignored for the whole 1.2s the box was held.
    func testALowNoisyReportDoesNotStrandTheBoxShort() {
        var box = Box(armed: true, armedFrom: 748)
        for h in [748, 253, 307, 315] as [CGFloat] { box.report(h) }
        XCTAssertEqual(box.height, 315,
                       "the box must end at the drawer's rest height, not at the noise it snapped through")
    }

    /// …and the order actually filmed, which never showed the bug, must be
    /// unchanged by the fix: the first snap is already the rest height, and the
    /// lower reports after it are ignored rather than chased.
    func testTheFilmedOrderStillRestsWhereItDid() {
        var box = Box(armed: true, armedFrom: 748)
        for h in filmed { box.report(h) }
        XCTAssertEqual(box.height, 315, "unchanged: 315 was the first snap and is the rest height")
    }

    /// The invariant itself, over every rotation of the filmed noise: whatever
    /// order the host reports in, a held box is never shorter than the tallest
    /// compact height it has been told about.
    func testNoArrivalOrderCanLeaveTheBoxShorterThanTheDrawer() {
        for rotation in 0..<filmed.count {
            let order = Array(filmed[rotation...] + filmed[..<rotation])
            var box = Box(armed: true, armedFrom: 748)
            var tallestCompact: CGFloat = 0
            for h in order {
                box.report(h)
                if h < CollapseTween.compactThreshold { tallestCompact = max(tallestCompact, h) }
                guard let held = box.height else { continue }
                XCTAssertGreaterThanOrEqual(held, tallestCompact,
                    "order \(order): held \(held) is shorter than the drawer's \(tallestCompact)")
            }
        }
    }

    /// Expanded-sized reports mid-collapse are noise about where the drawer WAS.
    /// Following one would balloon the box back to full height in the middle of
    /// the collapse - which is what `follow`'s comment always claimed it did not
    /// do, and now the claim is checked.
    func testExpandedNoiseDuringTheCollapseIsIgnored() {
        var box = Box(armed: true, armedFrom: 748)
        box.report(315)
        box.report(778)
        XCTAssertEqual(box.height, 315, "778 is the drawer it is leaving, not the drawer it is becoming")
    }

    /// A MANUAL grabber drag is never armed, so it must never start a tween -
    /// the box follows the model exactly, which is what makes a drag track the
    /// finger.
    func testAnUnarmedDragJustFollows() {
        var box = Box()
        for h in [748, 600, 420, 315] as [CGFloat] {
            box.report(h)
            XCTAssertNil(box.height, "an unarmed report must leave the box on the model")
        }
    }

    /// The flip needs a real drop, not any down-step: a small settle while armed
    /// (the host nudging the expanded height) is not the collapse.
    func testASmallDropIsNotTheFlip() {
        var box = Box(armed: true, armedFrom: 748)
        box.report(720)
        XCTAssertNil(box.height, "a 28pt settle is not a collapse")
        XCTAssertFalse(box.collapsing)
    }

    /// Jitter within the slack must not restart the animation - a retarget is a
    /// correction, not a per-frame follow.
    func testJitterAtTheTargetDoesNotRetarget() {
        XCTAssertEqual(CollapseTween.step(height: 316, armed: false, armedFrom: 748,
                                          collapsing: true, target: 315), .hold)
        XCTAssertEqual(CollapseTween.step(height: 320, armed: false, armedFrom: 748,
                                          collapsing: true, target: 315), .retarget(to: 320))
    }

    // MARK: 1.0(43) - the release, and why it may not land on the rest height
    //
    // MUTATIONS RUN: `remeasureNudge = 0` fails testTheReleaseNeverLandsOnThe…
    // (and leaves testTheNudgeIsSubPixel green, which is why both exist);
    // `remeasureNudge = 60` fails testTheNudgeIsSubPixel; putting
    // `boxHeight = 0` back into the tween's timer Task fails
    // testTheTweenReleasesThroughTheNudge.

    /// THE BUG, as a value. The tween ends on the height the box already rests
    /// at, so handing the override back is numerically no change - and SwiftUI
    /// had then published every landmark from the EXPANDED pass that opened the
    /// animation and had no later height change to publish from. Measured on the
    /// rig three seconds after an armed collapse: the board's own reader at 243,
    /// `handFrame` still at midY 623, the battle cards still at y 345, and the
    /// settlement sweep flying from 200pt below a 261pt drawer.
    ///
    /// So the release must go THROUGH a height that is not the rest height.
    ///
    /// MUTANT: `remeasureNudge = 0` (i.e. `handBack` returning `target`) fails
    /// this and `testTheNudgeIsSubPixel` passes, which is the point of having
    /// both - a nudge that publishes nothing is exactly the shipped defect.
    func testTheReleaseNeverLandsOnTheRestHeight() {
        for target in [261, 315, 243.5, 667] as [CGFloat] {
            XCTAssertNotEqual(CollapseTween.handBack(target: target), target,
                "a release equal to the rest height is no layout change at all, so "
                + "every published frame stays measured on the expanded board")
        }
    }

    /// …and it must be invisible. The nudge exists to make the layout move, not
    /// the picture: half a point is under one device pixel at every scale this
    /// app ships on, so nothing on screen shifts on the way back to the model.
    ///
    /// MUTANT: a whole-point nudge (or the 60pt one that would be "obviously
    /// safe") fails this while the test above still passes.
    func testTheNudgeIsSubPixel() {
        let target: CGFloat = 261
        XCTAssertLessThan(abs(CollapseTween.handBack(target: target) - target), 1,
                          "the release nudge must be sub-pixel - it is a re-measure, not a move")
    }

    /// The release is taken off the height the box is actually ON, so a retarget
    /// that moved the tween carries into it. A release computed from the height
    /// the tween STARTED at would put the box back at the expanded size for a
    /// frame, which is the flash this whole file exists to keep out of the
    /// collapse.
    func testTheReleaseIsTakenOffTheTargetNotTheStart() {
        XCTAssertEqual(CollapseTween.handBack(target: 315),
                       315 + CollapseTween.remeasureNudge)
        XCTAssertLessThan(CollapseTween.handBack(target: 315), CollapseTween.compactThreshold,
                          "the release must still be a compact height")
    }

    /// WHERE THE RULE HAS TO BE SPENT, and the half a value test cannot reach:
    /// the tween's own release. `MessagesRootView.follow` used to end its
    /// `.start` case by assigning `boxHeight = 0` from inside the timer Task,
    /// which is the assignment that publishes nothing.
    ///
    /// A source test for the reason HoldbackTests gives for its own two: the
    /// state this rule moves is `@State` on a SwiftUI view and has no seam.
    ///
    /// MUTANT: put `boxHeight = 0` back in the Task in place of the
    /// `handBackToModel()` call and this fails.
    func testTheTweenReleasesThroughTheNudge() throws {
        let here = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
        let src = try String(contentsOf: here.deletingLastPathComponent()
            .appendingPathComponent("FoolishKit/Messages/MessagesRootView.swift"),
                             encoding: .utf8)
        let head = try XCTUnwrap(src.range(of: "case .start(let from, let to):"),
                                 "the tween's start case")
        let body = String(src[head.lowerBound...].prefix(700))
        XCTAssertTrue(body.contains("handBackToModel()"),
                      "the collapse tween must release through the re-measure, not "
                      + "by assigning the rest height it is already on")
        XCTAssertTrue(src.contains("CollapseTween.handBack(target: boxHeight)"),
                      "and the height it releases through is the kernel of this file's rule, "
                      + "taken off the height the box is actually on")
    }
}
