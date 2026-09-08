// THE DRAWER THAT OPENS ITSELF - the BINDING half.
//
// The decision moved to the kernel (c/src/msg_expand.c). Its header carries the
// eight-cold-open measurement that found the moment, and c/tests/msg_expand_test.c
// is where the policy is pinned and mutation-checked - the retry, the one spare,
// the two-second window, the clear-on-expanded. None of that is re-asserted here,
// because a second copy of a rule is how two copies drift apart.
//
// What IS only testable here is the crossing, and it is not nothing: the Swift
// `Event` maps three cases onto three C codes, and a swap of the two transition
// cases would be a silent, total inversion of the feature that no C test can
// see. Nor can C see whether the three scalars of state survive the round trip
// through the inout pointers - a binding that dropped the writes would make
// every call look like the first one.
//
// WHAT NOTHING HERE CAN SEE, said again because this feature has produced three
// false passes already: whether Messages honours a
// `requestPresentationStyle(.expanded)`. That happens in another process's
// sheet, no API reports a dropped request, and no unit test can stage an
// MSMessagesAppViewController inside a real host. The proof is a device.
import XCTest
@testable import FoolishKit

final class NameEntryExpandTests: XCTestCase {

    /// The filmed cold open, driven through the Swift type. It proves the three
    /// event codes are mapped the right way round (a swap of the two transition
    /// cases fails the retry below) and that state survives between calls.
    func testTheFilmedColdOpenCrossesIntact() {
        var e = GateWire.NameEntryExpand()

        // 0.029 - the host STATES the style it is about to present in, before
        // any name screen exists.
        XCTAssertFalse(e.note(.transition(toCompact: true), now: 0.029))
        XCTAssertFalse(e.note(.transition(toCompact: true), now: 0.029))

        // 0.170 - a name screen asks. Issued, and on a cold open discarded.
        XCTAssertTrue(e.note(.wanted, now: 0.170))
        XCTAssertTrue(e.isPending, "the state must survive the crossing")

        // 0.434 - the compact install, the one moment a request sticks. If
        // `.transition(toCompact: true)` were mapped to the expanded code this
        // would answer false, and the feature would be inert on a device again.
        XCTAssertTrue(e.note(.transition(toCompact: true), now: 0.434),
                      "toCompact must map to the kernel's COMPACT event")

        // 0.560 - it landed, and the ask is over.
        XCTAssertFalse(e.note(.transition(toCompact: false), now: 0.560))
        XCTAssertFalse(e.isPending, "toCompact:false must map to EXPANDED, which clears it")
    }

    /// Two instances must not share a budget - the kernel keeps no statics, and
    /// the binding must not introduce any.
    func testTwoHostsDoNotShareABudget() {
        var a = GateWire.NameEntryExpand()
        var b = GateWire.NameEntryExpand()
        XCTAssertTrue(a.note(.wanted, now: 1.0))
        XCTAssertTrue(b.note(.wanted, now: 1.0), "b's own first ask, not a's")
        XCTAssertTrue(a.isPending)
        XCTAssertTrue(b.isPending)
    }

    /// A value type, so a copy taken before an ask is not advanced by it. This
    /// is what lets the view controller hold it as a plain stored property.
    func testACopyKeepsItsOwnState() {
        var e = GateWire.NameEntryExpand()
        let before = e
        XCTAssertTrue(e.note(.wanted, now: 1.0))
        XCTAssertTrue(e.isPending)
        XCTAssertFalse(before.isPending, "the copy was taken before the ask")
    }
}
