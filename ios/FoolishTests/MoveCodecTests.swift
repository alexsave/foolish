// MoveCodecTests.swift — pure-Swift checks on the Move/Card JSON contract with
// the C bridge (no engine needed). If these drift, fio_apply_json stops
// understanding the app's moves.

import XCTest
@testable import FoolishKit

final class MoveCodecTests: XCTestCase {

    func testAttackEncodesCardsShape() throws {
        let m = Move(type: .attack, cards: [Card(s: 0, v: 7)])
        let json = m.jsonString()
        // The bridge parser (find_key) needs "type", "cards", and {s,v} cards.
        XCTAssertTrue(json.contains("\"type\":\"attack\""))
        XCTAssertTrue(json.contains("\"s\":0"))
        XCTAssertTrue(json.contains("\"v\":7"))
        XCTAssertFalse(json.contains("attackCards"), "nil optionals must be omitted")
        XCTAssertFalse(json.contains("\"seat\""))
    }

    func testCoverIncludesAttackCards() throws {
        let m = Move(type: .cover, cards: [Card(s: 1, v: 9)], attackCards: [Card(s: 0, v: 7)])
        let json = m.jsonString()
        XCTAssertTrue(json.contains("\"type\":\"cover\""))
        XCTAssertTrue(json.contains("attackCards"))
    }

    func testPickupAndGoodAreZeroCard() throws {
        XCTAssertTrue(Move.pickup.jsonString().contains("\"type\":\"pickup\""))
        XCTAssertTrue(Move.good.jsonString().contains("\"type\":\"good\""))
    }

    func testDecodeBotStepShape() throws {
        // Exactly what fio_bot_step_json emits.
        let data = Data(#"{"seat":2,"type":"cover","cards":[{"s":1,"v":9}],"attackCards":[{"s":0,"v":7}]}"#.utf8)
        let m = try JSONDecoder().decode(Move.self, from: data)
        XCTAssertEqual(m.seat, 2)
        XCTAssertEqual(m.type, .cover)
        XCTAssertEqual(m.cards, [Card(s: 1, v: 9)])
        XCTAssertEqual(m.attackCards, [Card(s: 0, v: 7)])
    }

    func testCardIdentityStable() {
        XCTAssertEqual(Card(s: 0, v: 7).identity, "0-7")
        XCTAssertTrue(Card.hidden.isHidden)
    }
}
