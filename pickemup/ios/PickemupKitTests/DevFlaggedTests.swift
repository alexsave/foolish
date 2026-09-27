// DevFlaggedTests.swift - the two pieces a Debug build switches on with a dev
// file (ANIMATION_DECISIONS A14 and A15) take their numbers and their word
// from the kernel. What each looks like inside Messages is not provable here;
// that judgement is owed on a phone.
//
// Each test names the mutation it must go red on (MUTATE:), listed with the
// rest in pickemup/ios/TESTS_MUTATED.md.

import CPickemup
import XCTest
@testable import PickemupKit

@MainActor
final class DevFlaggedTests: XCTestCase {

    // MUTATE: CollapseSlide.pickemup passes t in seconds to the kernel (no
    // `* 1000`) -> "the whole travel at the flip, nothing at the end".
    // MUTATE: CollapseSlide.pickemup takes its steps from PK_LAY_COLLAPSE_MS
    // -> "the kernel's keyframes".
    func testTheCollapseSlideRunsOnTheKernelsPush() {
        let s = CollapseSlide.pickemup()
        XCTAssertEqual(s.duration, Double(PK_LAY_COLLAPSE_MS) / 1000, "the kernel's length")
        XCTAssertEqual(s.steps, Int(PK_LAY_COLLAPSE_STEPS), "the kernel's keyframes")
        XCTAssertEqual(s.flip, CGFloat(PK_LAY_COLLAPSE_FLIP), "the kernel's flip")
        XCTAssertEqual(s.push(500, 0), 500, "the whole travel at the flip, nothing at the end")
        XCTAssertEqual(s.push(500, s.duration), 0, "the whole travel at the flip, nothing at the end")
        XCTAssertEqual(s.push(500, 0.169), CGFloat(pk_lay_collapse_push(500, 169)), accuracy: 0.001,
                       "the host's spring, as the kernel has it")
    }

    // MUTATE: PickemupRoot's SendHint captions with "BTN_SEND" -> nothing
    // here (a view); the planned red run is a screenshot under `dev.sendhint`.
    // MUTATE: the SEND_HINT key renamed in keys.h -> "the reminder's word".
    func testTheSendReminderSaysTheKernelsWordAfterItsFuse() {
        XCTAssertEqual(Pk.string("SEND_HINT"), "Send", "the reminder's word is the kernel's")
        XCTAssertEqual(PK_T_SEND_HINT, 3000, "the sister product's three seconds")
        let host = PickemupHost()
        XCTAssertFalse(host.hintStaged, "nothing is staged until the controller says so")
        XCTAssertFalse(host.hintVisible)
    }
}
