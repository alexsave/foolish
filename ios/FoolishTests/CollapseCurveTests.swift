// CollapseCurveTests - the collapse follows the host drawer's own curve, and the
// curve in the code is the curve that was filmed.
//
// WHY THIS IS NOW A NUMERIC TEST. Round 10d shipped a bezier over 0.38s whose
// doc comment said "quartic-out over 0.45s", and the two drifted for months
// because the only check was a reader trusting a paragraph. The animation is
// no longer a `withAnimation` curve at all: `CollapseTween.height(from:to:at:)`
// is a pure function of time, so the thing that was only ever measurable off a
// film can be asserted directly against the film's own numbers.
//
// THE FIXTURE is the drawer's top edge averaged across seventeen filmed
// collapses (iPhone 14 Plus, real Messages, `msgrig.sh ruler` + `film`,
// passthrough frames), as a fraction of its 481pt travel. Single takes carry
// 13-16pt of frame-timing noise; the average fits a critically damped spring to
// 3.5pt. The film's first moving frame is ~6.5ms after the spring began, which
// is the `phase` below - the recorder's frame clock, not a knob.
import XCTest
@testable import FoolishKit

final class CollapseCurveTests: XCTestCase {

    /// (ms after the first moving frame, fraction of the travel done).
    private let filmedAverage: [(Double, Double)] = [
        (0, 0.0000), (10, 0.0458), (20, 0.0869), (30, 0.1505), (50, 0.2825),
        (70, 0.4198), (100, 0.5900), (150, 0.7854), (200, 0.8928), (300, 0.9773),
        (500, 0.9999),
    ]
    private let travel: Double = 481
    private let phase: Double = 0.0065

    /// The curve in the code is the curve that was filmed: within 5pt of a
    /// 481pt travel at every averaged sample. A response of 0.30 or 0.38 is
    /// 15-25pt out at 50ms, which is the opening the whole fit is about.
    func testTheCurveMatchesTheFilmedDrawer() {
        for (ms, frac) in filmedAverage {
            let got = CollapseTween.hostProgress(at: ms / 1000 + phase)
            XCTAssertEqual(got * travel, frac * travel, accuracy: 5,
                           "at \(Int(ms))ms the drawer had done \(frac * travel)pt, the curve says \(got * travel)")
        }
    }

    /// A spring that has not started has not moved. The evaluator is asked for
    /// negative times when `lead` is negative or a tick lands early, and a
    /// critically damped formula extended below zero does NOT return zero.
    func testNothingHappensBeforeTheStart() {
        for t in [-1.0, -0.05, -0.001, 0] {
            XCTAssertEqual(CollapseTween.hostProgress(at: t), 0, "t=\(t)")
        }
    }

    /// Monotonic and settled: the drawer never comes back up, and by half a
    /// second it is where it will rest.
    func testTheCurveOnlyEverDescendsAndSettles() {
        var last = -1.0
        for i in 0...200 {
            let p = CollapseTween.hostProgress(at: Double(i) * 0.005)
            XCTAssertGreaterThanOrEqual(p, last, "step \(i)")
            XCTAssertLessThanOrEqual(p, 1)
            last = p
        }
        XCTAssertGreaterThan(CollapseTween.hostProgress(at: 0.5), 0.999)
    }

    /// The box height rides the curve between its two ends and nowhere else.
    func testTheHeightRunsFromTheExpandedBoxToTheCompactOne() {
        XCTAssertEqual(CollapseTween.height(from: 815, to: 340, at: 0), 815)
        XCTAssertEqual(CollapseTween.height(from: 815, to: 340, at: 2), 340, accuracy: 0.01)
        // 0.59 of the way 100ms after the film's first moving frame, off the
        // fixture above (so the film's phase applies here too).
        let mid = CollapseTween.height(from: 815, to: 340, at: 0.1 + phase)
        XCTAssertLessThan(mid, 815); XCTAssertGreaterThan(mid, 340)
        XCTAssertEqual(mid, 815 - 475 * 0.59, accuracy: 5)
    }

    /// A retarget eases, it does not step: the ends are exact and the middle is
    /// strictly between them.
    func testARetargetEasesRatherThanSteps() {
        XCTAssertEqual(CollapseTween.retargetBlend(from: 360, to: 394, at: 0), 360)
        XCTAssertEqual(CollapseTween.retargetBlend(from: 360, to: 394, at: 1), 394)
        let mid = CollapseTween.retargetBlend(from: 360, to: 394,
                                              at: CollapseTween.retargetDuration / 2)
        XCTAssertGreaterThan(mid, 360); XCTAssertLessThan(mid, 394)
    }

    /// The number in the code is the number in the comment - the check that
    /// would have caught round 10d's 0.38-versus-0.45 drift.
    func testTheCodeAndTheCommentAgreeOnTheResponse() throws {
        let here = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
        let src = try String(contentsOf: here.deletingLastPathComponent()
            .appendingPathComponent("FoolishKit/Messages/CollapseTween.swift"), encoding: .utf8)
        let coded = String(format: "%.3f", CollapseTween.hostResponse)
        XCTAssertTrue(src.contains("response of \(coded)s"),
                      "the code runs response \(coded) but the file's note does not say so - "
                      + "say the number where the next reader will see it")
    }
}
