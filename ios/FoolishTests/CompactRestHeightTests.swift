// THE HEIGHT THE COLLAPSED DRAWER COMES TO REST AT, and whose it is.
//
// Owner, 1.1(69): "Collapsed should be the same height as the keyboard height
// in iMessages, always." What was reported as the defect was a 16pt difference
// between two films of the SAME take on the SAME binary - a collapsed lobby
// settling at a drawer top of 584 in one and 568 in the other.
//
// MEASURED, on a 6.9" simulator (440x956pt) driving the real Messages app:
//
//   Messages' compose field holds no first responder -> drawer 388.7pt,
//                                                       top 567.3,
//                                                       our view handed 340
//   Messages' compose field IS first responder        -> drawer 372pt,
//                                                       top 584.0,
//                                                       our view handed 323
//
// The whole input stack drops 17pt with it, our surface fills exactly what it
// is handed, and NOTHING in this target chooses between the two. Create, leave,
// toggle and start all land on 340 from a base that left the thread and came
// back, and all four land on 323 from a base that tapped the compose field
// first - the ACTION is irrelevant and the host's first-responder state decides
// it. (`ios/Tools/rig/README.md` trap 10 has the full reading, including the
// two films whose FIRST frames already differ by the 16pt their takes were
// blamed for.)
//
// So there is no compact height for this code to get right or wrong: there is
// only the host's, and the job is to render it and nothing else. That is what
// this file pins, because it is the part a future change can silently break -
// one remembered compact height, one clamp, one `max(...)` against a constant,
// and the drawer stops being the host's.
//
// It does NOT test the tween. `CollapseTween`'s curve, lead and retarget rule
// are CollapseTweenTests / CollapseCurveTests / CollapseDriverTests, and the
// two assertions here that touch `step` are about what it does when NOTHING is
// collapsing - the rest state either side of an animation nobody is running.
//
// MUTATIONS RUN (2026-09-11, on this branch):
//   `height: boxHeight > 0 ? boxHeight : geo.size.height`
//     -> `height: boxHeight > 0 ? boxHeight : min(geo.size.height, 323)`
//        testTheRestingBoxIsTheHostsOwnHeight fails ("the resting frame no
//        longer follows the host's geometry unconditionally").
//   `case .follow: driver.stop(); boxHeight = 0`
//     -> `case .follow: driver.stop()`
//        testReleaseHandsTheBoxBackToTheHost fails ("the .follow case no longer
//        releases the box").
//   `flipDrop` 60 -> 10
//        testAHostCompactResizeIsNotMistakenForTheCollapseFlip fails
//        ("a 17pt host resize started the collapse tween").
import XCTest
import CoreGraphics
@testable import FoolishKit

final class CompactRestHeightTests: XCTestCase {

    /// The two compact heights Messages hands this extension on a 6.9" phone,
    /// as its own AnimLog reports them (`stage follow geo=830->340` / `->323`).
    /// Named rather than inlined so a reader of a failure knows these are
    /// measured numbers and not thresholds someone picked.
    private let hostCompactUnfocused: CGFloat = 340
    private let hostCompactFocused: CGFloat = 323

    private func source() throws -> [String] {
        let here = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
        let url = here.deletingLastPathComponent()
            .appendingPathComponent("FoolishKit/Messages/MessagesRootView.swift")
        return try String(contentsOf: url, encoding: .utf8).components(separatedBy: "\n")
    }

    private func code(_ lines: [String]) -> [String] {
        lines.filter { !$0.trimmingCharacters(in: .whitespaces).hasPrefix("//")
                       && !$0.trimmingCharacters(in: .whitespaces).hasPrefix("///") }
    }

    // MARK: the resting box is the host's, verbatim

    /// At rest the surface is framed at the GeometryReader's own height, with
    /// no constant anywhere near it. `boxHeight` is the tween's override and is
    /// zero at rest (below); everything else on that line must be the host.
    ///
    /// WHY A SOURCE SCAN. The height in question is delivered by another
    /// process, to a SwiftUI GeometryReader, inside a sheet Messages owns.
    /// Nothing in a unit test can stage that. What CAN be pinned is that the
    /// one expression which decides our resting size names the host's geometry
    /// and nothing else - which is exactly the invariant a "let's just clamp
    /// the compact height" change would break.
    func testTheRestingBoxIsTheHostsOwnHeight() throws {
        let src = code(try source())
        let framed = src.filter { $0.contains("boxHeight > 0 ? boxHeight : geo.size.height") }
        XCTAssertEqual(framed.count, 2,
                       "the resting frame no longer follows the host's geometry "
                       + "unconditionally - the surface's frame and the wool behind "
                       + "it are the two places that read it")
        for line in framed {
            XCTAssertFalse(line.contains("min(") || line.contains("max(")
                           || line.contains("clamp"),
                           "a clamp on the resting height makes it OURS: \(line)")
        }
    }

    /// …and nobody remembers a compact height to lay out against. Round 22
    /// already deleted `lastCompactHeight` and `extentHold` for being written
    /// every layout pass and read by nothing; this keeps them deleted, because
    /// a remembered compact height is precisely how the drawer would stop being
    /// the host's.
    func testNothingRemembersACompactHeight() throws {
        let src = code(try source())
        for banned in ["lastCompactHeight", "extentHold", "stageHeight"] {
            XCTAssertFalse(src.contains(where: { $0.contains(banned) }),
                           "`\(banned)` is back - a remembered compact height is not "
                           + "the host's compact height")
        }
    }

    /// The tween's release puts the box back to zero, which is what hands the
    /// frame above back to `geo.size.height`. Without it the surface would rest
    /// at whatever the last collapse happened to land on, forever.
    func testReleaseHandsTheBoxBackToTheHost() throws {
        let src = code(try source())
        guard let i = src.firstIndex(where: { $0.contains("case .follow:") }) else {
            return XCTFail("`follow`'s .follow case is gone - this test needs rewriting")
        }
        let body = src[i...min(i + 3, src.count - 1)].joined(separator: "\n")
        XCTAssertTrue(body.contains("boxHeight = 0"),
                      "the .follow case no longer releases the box to the host")
        XCTAssertTrue(src.contains(where: { $0.contains("boxHeight = 0") && !$0.contains("==") }),
                      "nothing sets boxHeight back to 0 at all")
    }

    // MARK: a host resize at rest is FOLLOWED, not held

    /// Messages can resize its own compact drawer under a surface that is
    /// already at rest - it does exactly that, by 17pt, the moment its compose
    /// field takes or loses first responder. Both of the heights it gives must
    /// come back as `.follow`, i.e. "the box is the model box", or the surface
    /// would sit at the wrong one of the two until something else moved.
    func testBothHostCompactHeightsAreFollowedAtRest() {
        for h in [hostCompactUnfocused, hostCompactFocused] {
            XCTAssertEqual(CollapseTween.step(height: h, armed: false, armedFrom: 0,
                                              collapsing: false, target: 0),
                           .follow,
                           "a resting report of \(h) must release the box to the host")
        }
    }

    /// …and the 17pt between them must not read as the collapse flip. The flip
    /// is a drop of hundreds of points (830 -> 340 on this device); `flipDrop`
    /// is what separates the two, and a host resize inside it would start a
    /// tween against a drawer that is not moving.
    func testAHostCompactResizeIsNotMistakenForTheCollapseFlip() {
        let step = CollapseTween.step(height: hostCompactFocused, armed: true,
                                      armedFrom: hostCompactUnfocused,
                                      collapsing: false, target: 0)
        XCTAssertEqual(step, .follow, "a 17pt host resize started the collapse tween")
        // The real flip, for contrast: the same call with the expanded height
        // the rig measures (830) DOES start it, so the guard above is not
        // passing by refusing everything.
        guard case .start = CollapseTween.step(height: hostCompactUnfocused, armed: true,
                                               armedFrom: 830, collapsing: false,
                                               target: 0) else {
            return XCTFail("830 -> 340 is the collapse flip and must still start the tween")
        }
    }

    /// Both host compact heights are on the compact side of every threshold the
    /// surface reasons with, so nothing downstream can tell them apart either -
    /// a send hint or an autofocus gate that flipped between 340 and 323 would
    /// be a second, invisible copy of this same bug.
    func testTheTwoHostHeightsAreIndistinguishableToEveryGate() {
        for h in [hostCompactUnfocused, hostCompactFocused] {
            XCTAssertEqual(MessageTableView.collapseFraction(height: h), 1,
                           "\(h)pt must read as fully collapsed")
            XCTAssertLessThan(h, CollapseTween.compactThreshold,
                              "\(h)pt must be under the compact threshold")
        }
    }
}
