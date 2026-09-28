// CollapseDriverTests - the clock that moves the box, driven for real.
//
// THE BUG THIS FILE KEEPS OUT is the one every filmed variant of the collapse
// shared, the shipped bezier included: `withAnimation { boxHeight = to }`
// renders its START value on its first frame, so the first picture after the
// style flip was the still-expanded box under a drawer whose top had already
// moved 20-45pt - the hand pushed off the bottom of the screen for a frame,
// and the one off-screen frame in the shipped take. The driver's first height
// must therefore be on the curve, never `from`.

import XCTest
@testable import FoolishKit

@MainActor
final class CollapseDriverTests: XCTestCase {

    /// The first height is handed over synchronously, in the caller's own
    /// runloop turn, and it is already on the curve at `lead` - not `from`.
    ///
    /// MUTANT: emitting `from` first (or deferring the first tick to the
    /// timer) fails this.
    func testTheFirstHeightIsOnTheCurveNotTheExpandedBox() {
        let d = CollapseDriver()
        var seen: [CGFloat] = []
        d.start(from: 815, to: 340, lead: 0.020, hz: 120, tick: { seen.append($0) }, onDone: {})
        defer { d.stop() }
        XCTAssertEqual(seen.count, 1, "the first height is delivered before start returns")
        XCTAssertEqual(seen[0], CollapseTween.height(from: 815, to: 340, at: 0.020), accuracy: 0.5)
        XCTAssertLessThan(seen[0], 815 - 20, "20ms into the host's spring the box is already shorter")
    }

    /// Run to completion: heights only ever descend, end within a point of
    /// the target, and `onDone` fires exactly once.
    func testTheRunDescendsToTheTargetAndFinishesOnce() {
        let d = CollapseDriver()
        var seen: [CGFloat] = []
        var done = 0
        let finished = expectation(description: "onDone")
        // 0.6s: the spring is at 99.99% by then, so the run is settled.
        d.start(from: 815, to: 340, lead: 0, hz: 240, duration: 0.6,
                tick: { seen.append($0) },
                onDone: { done += 1; finished.fulfill() })
        wait(for: [finished], timeout: 3)
        XCTAssertEqual(done, 1)
        XCTAssertGreaterThan(seen.count, 10, "a 0.6s run at 240Hz ticks many times")
        for (a, b) in zip(seen, seen.dropFirst()) {
            XCTAssertLessThanOrEqual(b, a, "the box never grows during a collapse")
        }
        XCTAssertEqual(seen.last!, 340, accuracy: 1)
        XCTAssertFalse(d.isRunning)
    }

    /// A manual drag mid-collapse stops the clock: no more ticks, no `onDone`.
    func testStopEndsTheRunWithoutFinishing() {
        let d = CollapseDriver()
        var ticks = 0
        var done = false
        d.start(from: 815, to: 340, lead: 0, hz: 240, duration: 0.2,
                tick: { _ in ticks += 1 }, onDone: { done = true })
        d.stop()
        let after = ticks
        let quiet = expectation(description: "quiet")
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) { quiet.fulfill() }
        wait(for: [quiet], timeout: 2)
        XCTAssertEqual(ticks, after, "no ticks after stop")
        XCTAssertFalse(done, "a stopped run does not release the box a second time")
        XCTAssertFalse(d.isRunning)
    }

    /// A retarget eases the end of the curve rather than stepping the height:
    /// the tick right after it moves by a fraction of the retarget, not all of
    /// it.
    func testARetargetDoesNotStepTheHeight() {
        let d = CollapseDriver()
        var seen: [CGFloat] = []
        let some = expectation(description: "ticks")
        some.assertForOverFulfill = false
        d.start(from: 815, to: 340, lead: 0.25, hz: 240, duration: 0.5,
                tick: { seen.append($0); if seen.count == 6 { some.fulfill() } },
                onDone: {})
        wait(for: [some], timeout: 2)
        // 250ms in, the spring is at ~0.94; a 34pt retarget stepped in full
        // would move the height ~32pt at once.
        let before = seen.last!
        d.retarget(to: 374)
        let more = expectation(description: "more ticks")
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.02) { more.fulfill() }
        wait(for: [more], timeout: 2)
        d.stop()
        let after = seen.last!
        XCTAssertGreaterThan(after, before - 2, "the box keeps descending, it does not jump up or down")
        XCTAssertLessThan(after - before, 20, "20ms into a 180ms ease, well short of the whole 34pt")
    }
}
