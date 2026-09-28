// NetTests.swift — pins the pure Net/ ports to the web's behavior (§16.D2, D4).

import XCTest
@testable import FoolishKit
import FoolishNet

final class NetTests: XCTestCase {

    func testNameToEmailMatchesWeb() {
        // Goldens: sha256("ALEX")[:16] and sha256("FOOL")[:16] (computed from the
        // same algorithm the web uses). Case-insensitive (uppercased first).
        XCTAssertEqual(Auth.nameToEmail("alex"), "01b7cc720b3af40a@foolish.cards")
        XCTAssertEqual(Auth.nameToEmail("ALEX"), "01b7cc720b3af40a@foolish.cards")
        XCTAssertEqual(Auth.nameToEmail("Fool"), "6d573648b9ef4e8f@foolish.cards")
    }

    func testReservedPrefixRejectedAnywhere() {
        XCTAssertTrue(Auth.usernameUsesReservedPrefix("%Cordite"))
        XCTAssertTrue(Auth.usernameUsesReservedPrefix("na%me"))   // .includes, not startsWith
        XCTAssertFalse(Auth.usernameUsesReservedPrefix("alex"))
        XCTAssertFalse(Auth.usernameUsesReservedPrefix(""))
    }

    func testVersionGate() {
        XCTAssertFalse(VersionGate.shouldDrop(lastApplied: nil, incoming: 5))   // first seq
        XCTAssertFalse(VersionGate.shouldDrop(lastApplied: 3, incoming: nil))   // replay (no version)
        XCTAssertTrue(VersionGate.shouldDrop(lastApplied: 5, incoming: 5))      // equal → stale
        XCTAssertTrue(VersionGate.shouldDrop(lastApplied: 5, incoming: 4))      // older → stale
        XCTAssertFalse(VersionGate.shouldDrop(lastApplied: 5, incoming: 6))     // newer → apply
    }
}
