// CollapseCurveTests - the collapse's animation is a SPRING, and the number in
// the code is the number in the comment.
//
// WHY A SOURCE TEST. The value under test is empirical: it was fitted to the
// host's own drawer, measured optically at 60fps off a filmed run, and there is
// no way to assert it from inside a unit test - `withAnimation` has no readable
// curve, and the thing it has to match belongs to Messages, not to us.
//
// WHAT MAKES IT WORTH HAVING is the failure it prevents, which already
// happened. Round 10d fitted a quartic-out over 0.45s, wrote that in the doc
// comment, and shipped `.timingCurve(..., duration: 0.38)`. The two then sat
// out of step for months while every later reader trusted the paragraph. At
// 30fps nobody could see the cost; at 60fps it is 107pt of deviation from the
// curve the host actually runs, and it is what put the box's bottom edge 33pt
// away from where it belongs for the first ~130ms of every collapse.
//
// So this asserts the two agree. A future edit that retunes the animation and
// leaves the comment behind fails here, which is the only place that could have
// caught the original.
//
// THE MEASUREMENT behind the value, for whoever reads this next (iPhone 14 Plus,
// real Messages, two independent takes, `msgrig.sh ruler` + `film` + `sheet`):
//
//   what the code ran, .timingCurve(0.165,0.84,0.44,1) @0.38s   ~105pt max error
//   quartic-out @0.45s, what the comment claimed                  ~60pt
//   cubic-out @0.42s, the best analytic easing                    ~34pt
//   critically-damped spring, response 0.33s                      ~16pt
//
// The host is running a SPRING, which is why no bezier ever fit. Fitting both
// takes at once lands on response 0.39 / damping 0.88 / initial velocity 1.8 at
// ~13pt; `withAnimation(.spring(response:dampingFraction:))` cannot express an
// initial velocity, so the shipped value is the best critically-damped fit.
//
// AND THE LIMIT OF ALL OF IT, which is why the residual is not worth chasing
// further: a per-frame clock drawn beside the ruler (CollapseClock) showed that
// 11 of the 26 frames in a collapse are frames THIS APP NEVER DREW - Messages
// composites the transition from snapshots of our view. No curve can correct a
// frame we did not render. The spring wins because it is closer more of the
// time, not because it can ever be exact.
import XCTest

final class CollapseCurveTests: XCTestCase {

    private func rootViewSource() throws -> String {
        // #filePath is this file; the surface sits one directory over.
        let here = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
        let url = here.deletingLastPathComponent()
            .appendingPathComponent("FoolishKit/Messages/MessagesRootView.swift")
        return try String(contentsOf: url, encoding: .utf8)
    }

    /// The collapse animates on a spring, not a bezier. A `timingCurve` here is
    /// the regression: the host's drawer is a spring, and a bezier cannot
    /// follow one through its opening 70ms, which is where the whole error is.
    func testTheCollapseAnimatesOnASpring() throws {
        let src = try rootViewSource()
        guard let line = src.components(separatedBy: "\n")
            .first(where: { $0.contains("withAnimation(") && $0.contains("boxHeight") == false
                            && $0.contains("spring") || ($0.contains("withAnimation(") && $0.contains("timingCurve")) })
        else { return XCTFail("no collapse animation found in MessagesRootView") }
        XCTAssertFalse(line.contains("timingCurve"),
                       "the collapse is back on a bezier - the host runs a spring, "
                       + "and a bezier misses its opening 70ms by ~105pt")
        XCTAssertTrue(line.contains("spring("),
                      "the collapse animation is neither a spring nor a timingCurve: \(line)")
    }

    /// The response in the code and the response in the comment are the same
    /// number. This is the assertion that would have caught round 10d's
    /// 0.38-versus-0.45 drift.
    func testTheCodeAndTheCommentAgreeOnTheResponse() throws {
        let src = try rootViewSource()
        guard let m = src.range(of: #"response:\s*([0-9.]+)"#, options: .regularExpression)
        else { return XCTFail("no `response:` in MessagesRootView - has the spring gone?") }
        let coded = String(src[m]).components(separatedBy: ":")[1]
            .trimmingCharacters(in: .whitespaces)
        XCTAssertTrue(src.contains("response \(coded)s") || src.contains("response \(coded)"),
                      "the code animates with response \(coded) but no comment in this file "
                      + "states that number. Round 10d shipped 0.38 while its comment said "
                      + "0.45 and the two drifted for months - say the number where the "
                      + "next reader will see it.")
    }
}
