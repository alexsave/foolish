// BidPickerTests - the picker lights exactly what the kernel's menu allows:
// Raise at or above the least quantity for the chosen face, never below it,
// and Call only when the menu says so.

import XCTest
@testable import ChuiniuKit

final class BidPickerTests: XCTestCase {
    /// A menu as the kernel might hand it after "four 3s": faces above 3 at
    /// four, faces at or below 3 at five. The numbers are this test's input,
    /// not a rule the picker knows.
    private let afterFour3s = Menu(minimumRaise: Bid(quantity: 4, face: 4),
                                   minQuantityByFace: [0, 0, 5, 5, 4, 4, 4],
                                   maxQuantity: 20, callAllowed: true)

    func testRaiseIsOffBelowTheInjectedMinimumAndOnAtIt() {
        let m = afterFour3s
        XCTAssertFalse(BidPicker.raiseEnabled(quantity: 4, face: 3, menu: m), "four 3s again is below the minimum")
        XCTAssertFalse(BidPicker.raiseEnabled(quantity: 4, face: 2, menu: m), "four 2s is below the minimum")
        XCTAssertTrue(BidPicker.raiseEnabled(quantity: 5, face: 3, menu: m), "five 3s is at the minimum")
        XCTAssertTrue(BidPicker.raiseEnabled(quantity: 4, face: 4, menu: m), "four 4s is at the minimum")
        XCTAssertFalse(BidPicker.raiseEnabled(quantity: 3, face: 6, menu: m), "three 6s is below the minimum")
        XCTAssertTrue(BidPicker.raiseEnabled(quantity: 20, face: 6, menu: m), "every die is still a bid")
        XCTAssertFalse(BidPicker.raiseEnabled(quantity: 21, face: 6, menu: m), "past every die is not")
    }

    func testAFaceTheKernelDidNotRankIsNeverRaisable() {
        XCTAssertFalse(BidPicker.raiseEnabled(quantity: 9, face: 1, menu: afterFour3s), "no bid on 1s")
        XCTAssertFalse(BidPicker.raiseEnabled(quantity: 9, face: 7, menu: afterFour3s), "no face 7")
    }

    func testCallFollowsTheMenu() {
        XCTAssertTrue(BidPicker.callEnabled(menu: afterFour3s), "a bid on the table may be called")
        var opening = afterFour3s
        opening.callAllowed = false
        XCTAssertFalse(BidPicker.callEnabled(menu: opening), "nothing to call before the opening bid")
    }

    func testTheStepperSpansTheLeastQuantityToEveryDie() {
        XCTAssertEqual(BidPicker.quantityRange(menu: afterFour3s), 4...20, "least of the faces to the table")
    }
}
