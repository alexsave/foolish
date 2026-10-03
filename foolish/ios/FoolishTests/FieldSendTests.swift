// THE INPUT FIELD IS STILL SENDING (FieldSend, StagedBubbleRouting.swift).
//
// What these pin is the rule `stage` waits on: a pressed bubble holds the
// field until Messages starts sending it, and never longer than the window.
// What they cannot see is Messages drawing the zero-height entry; that was
// filmed (r2b/sendmove_0_* before, r2b/sendmove_fix_* after).
import XCTest
@testable import FoolishKit

final class FieldSendTests: XCTestCase {
    private let a = Data([1, 2, 3])
    private let b = Data([4, 5, 6])

    func testAPressedBubbleHoldsTheFieldUntilItStartsSending() {
        var f = FieldSend()
        XCTAssertFalse(f.isBusy(now: 10), "nothing pressed, nothing held")
        f.pressed(a, at: 10)
        XCTAssertTrue(f.isBusy(now: 10.6), "the second after a Send press is the sending one")
        f.started(b)
        XCTAssertTrue(f.isBusy(now: 10.7), "another bubble starting says nothing about this one")
        f.started(a)
        XCTAssertFalse(f.isBusy(now: 10.8), "it has left the field")
    }

    func testASendThatNeverStartsHoldsTheFieldOnlyForTheWindow() {
        var f = FieldSend()
        f.pressed(a, at: 10)
        XCTAssertTrue(f.isBusy(now: 10 + FieldSend.window - 0.01))
        XCTAssertFalse(f.isBusy(now: 10 + FieldSend.window), "bounded")
        f.started(nil)
        XCTAssertFalse(f.isBusy(now: 20))
    }

    func testTheFieldSendWaitShipsOn() {
        XCTAssertTrue(FieldSend.waitsForOwnSendByDefault)
    }
}
