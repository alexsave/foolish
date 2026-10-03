// WHICH WAY THE DRAWER IS - the BINDING half.
//
// The rule is the kernel's (c/src/msg_expand.c, `msg_style_*`), pinned and
// mutation-checked in c/tests/msg_expand_test.c with the flight log that found
// it in msg_expand.h. Not re-asserted here. What only this side can get wrong is
// the crossing: `note(did:expanded:)` maps two Bools onto two C codes each, and
// a swap of WILL and DID (or of the two styles) would invert the auto-collapse
// with every C test still green; and the two scalars must survive the inout
// round trip, or every call would look like the first one.
//
// WHAT NOTHING HERE CAN SEE: whether Messages runs the collapse that follows.
// That was filmed (first-run New game on the iPhone 17e simulator, 5 of 5 cold
// opens left expanded before, collapsed after).
import XCTest
@testable import FoolishKit

final class DrawerStyleTests: XCTestCase {

    /// First-run New game, as the simulator's flight log has it: the late
    /// install did leaves the drawer expanded.
    func testTheLateInstallDidCrossesIntact() {
        var d = GateWire.DrawerStyle(expanded: false)
        XCTAssertFalse(d.isExpanded)
        d.note(did: false, expanded: false)   // the stated style
        d.note(did: true, expanded: false)
        d.note(did: false, expanded: false)   // the install's will
        d.note(did: false, expanded: true)    // our ask lands
        d.note(did: true, expanded: true)
        XCTAssertTrue(d.isExpanded, "the expand arrived")
        d.note(did: true, expanded: false)    // the install's own did, late
        XCTAssertTrue(d.isExpanded, "the late install did must not read as a collapse")
        d.note(did: false, expanded: false)   // the auto-collapse runs
        d.note(did: true, expanded: false)
        XCTAssertFalse(d.isExpanded, "a real collapse is compact")
    }

    /// A drag let go: the only did after an unconfirmed will is where the
    /// drawer rests. A binding that dropped `confirmed` on the way back would
    /// read this as the late tail and stay expanded.
    func testADragLetGoCrossesIntact() {
        var d = GateWire.DrawerStyle(expanded: false)
        d.note(did: false, expanded: true)
        d.note(did: true, expanded: false)
        XCTAssertFalse(d.isExpanded)
        XCTAssertTrue(GateWire.DrawerStyle(expanded: true).isExpanded,
                      "an activation handed expanded starts expanded")
    }

    func testTheDrawerStyleFixShipsOn() {
        XCTAssertTrue(CollapseTween.readsDrawerFromCallbacksByDefault)
    }
}
