// CardIdentityTests.swift — the two Card facts nothing else pins.
//
// This file used to be MoveCodecTests: four checks on the exact JSON shape
// Move.jsonString() produced for the C parser to read back. Both ends of that
// contract are gone - fio_apply_json and fio_bot_step_json were deleted when
// moves moved to the awire frame, and jsonString() went with them, so the tests
// were asserting that a dead encoder still spoke to a dead parser.
//
// A Move's real round-trip is covered where it happens: PackedActionTests over
// PackedAction/awire, and HarnessTests/PlayRulesTests over MoveWire.

import XCTest
@testable import FoolishKit

final class CardIdentityTests: XCTestCase {

    func testCardIdentityStable() {
        XCTAssertEqual(Card(s: 0, v: 7).identity, "0-7")
        XCTAssertTrue(Card.hidden.isHidden)
    }
}
