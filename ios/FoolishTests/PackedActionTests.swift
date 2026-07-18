// PackedActionTests.swift — pins the awire byte layout (§16.D3). These assert
// the exact bytes awire.h / awire.c produce; when the TS-generated
// action_goldens.json lands (M-D), a second test will diff against those too so
// any wire change fails in two places by design.

import XCTest
@testable import FoolishKit
import FoolishNet

final class PackedActionTests: XCTestCase {

    func testCardByteEncoding() {
        // suit*13 + (value-1). Spades six = 0*13+5 = 5; Diamonds ace = 3*13+12 = 51.
        XCTAssertEqual(PackedAction.encodeCard(Card(s: 0, v: 6)), 5)
        XCTAssertEqual(PackedAction.encodeCard(Card(s: 3, v: 13)), 51)
        XCTAssertEqual(PackedAction.encodeCard(.hidden), 0xFE)
        // Round-trip.
        for s in 0..<4 { for v in 1...13 {
            XCTAssertEqual(PackedAction.decodeCard(PackedAction.encodeCard(Card(s: s, v: v))), Card(s: s, v: v))
        } }
    }

    func testAttackWire() throws {
        let bytes = try PackedAction.encode(Move(type: .attack, cards: [Card(s: 0, v: 6), Card(s: 1, v: 6)]))
        // [kind=0][n=2][5][18]  (hearts six = 1*13+5 = 18)
        XCTAssertEqual(bytes, [0, 2, 5, 18])
    }

    func testCoverWire() throws {
        let m = Move(type: .cover, cards: [Card(s: 1, v: 9)], attackCards: [Card(s: 0, v: 7)])
        let bytes = try PackedAction.encode(m)
        // [kind=1][n=1][cover=hearts9=21][attack=spades7=6]
        XCTAssertEqual(bytes, [1, 1, 21, 6])
    }

    func testPickupGoodAreTwoBytes() throws {
        XCTAssertEqual(try PackedAction.encode(.pickup), [3, 0])
        XCTAssertEqual(try PackedAction.encode(.good), [4, 0])
    }

    func testCoverMismatchRejected() {
        let m = Move(type: .cover, cards: [Card(s: 1, v: 9)], attackCards: [])
        XCTAssertThrowsError(try PackedAction.encode(m))
    }

    func testRequestEnvelopeV2() throws {
        let body = try PackedAction.requestBody(gameId: "g1", intentVersion: 7,
                                                move: Move(type: .attack, cards: [Card(s: 0, v: 6)]))
        // [2]['g'll... ] : fmt=2, gidLen=2, 'g','1', intentVersion LE 07 00 00 00, then wire [0,1,5]
        XCTAssertEqual([UInt8](body), [2, 2, 0x67, 0x31, 7, 0, 0, 0, 0, 1, 5])
    }

    func testResponseDecodeStaleRound() throws {
        // [fmt=1][status=rejected=1][rejectCode=100][version=42 LE]
        let data = Data([1, 1, 100, 42, 0, 0, 0])
        let r = try PackedAction.decodeResponse(data)
        XCTAssertEqual(r.status, .rejected)
        XCTAssertTrue(r.isStaleRound)
        XCTAssertEqual(r.version, 42)
    }
}
