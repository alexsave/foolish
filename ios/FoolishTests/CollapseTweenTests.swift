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
}
