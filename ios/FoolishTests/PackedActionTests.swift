// PackedActionTests.swift — the awire action frame, pinned to the KERNEL.
//
// These used to assert byte literals typed out by hand beside a Swift encoder
// that was also typed out by hand: Swift measured against Swift, which cannot
// fail for the reason that matters. The bytes now come from
// ios/Fixtures/action_goldens.bin, which c/ios/ios_action_goldens.c writes
// through fio_awire_encode and CI checks is fresh (.github/workflows/ios.yml),
// so a change to c/src/awire.c that Swift does not follow fails here.
//
// What is left to Swift is the MAPPING, and that is what these test: a Move's
// type onto the kernel's MOVE_* number, its cards onto the door's suit/value
// pairs in order, and a cover's attack cards onto the attacks they pair with.
// The fixture carries the REFUSALS too - the moves the kernel writes no frame
// for - because "this move has no bytes" is equally the kernel's answer.
//
// The request envelope at the bottom is NOT the kernel's: it is the `action`
// endpoint's protocol, read by table_request_decode (c/src/table.c) and written
// on the web by sdk/ts/wire/awire.ts. See the note in PackedAction.swift.

import XCTest
@testable import FoolishKit
import FoolishNet

final class PackedActionTests: XCTestCase {

    // MARK: the fixture
    //
    // Its layout is stated once, in c/ios/ios_action_goldens.c. This is the
    // reader; a test bundle is one of the hosts that may read a format the
    // kernel defines, it just may not WRITE one.

    private struct Vector {
        let name: String
        let type: Int
        let cards: [Card]
        let attacks: [Card]
        /// The frame, or nil when the kernel refused to write one.
        let wire: [UInt8]?
    }

    private struct Reader {
        let b: [UInt8]
        var at = 0
        mutating func u8() -> Int { defer { at += 1 }; return Int(b[at]) }
        mutating func i8() -> Int { defer { at += 1 }; return Int(Int8(bitPattern: b[at])) }
        mutating func bytes(_ n: Int) -> [UInt8] { defer { at += n }; return Array(b[at..<(at + n)]) }
        mutating func cards() -> [Card] {
            let n = u8()
            return (0..<n).map { _ in let s = i8(); return Card(s: s, v: i8()) }
        }
    }

    private func loadVectors() throws -> [Vector] {
        let url = try XCTUnwrap(
            Bundle(for: Self.self).url(forResource: "action_goldens", withExtension: "bin"),
            "action_goldens.bin missing from the test bundle — run `make ios-goldens`"
        )
        var r = Reader(b: [UInt8](try Data(contentsOf: url)))
        XCTAssertEqual(r.u8(), 1, "action_goldens.bin is not format 1")
        let n = r.u8()
        XCTAssertGreaterThan(n, 0, "the fixture carries no vectors")
        var out: [Vector] = []
        for _ in 0..<n {
            let nameLen = r.u8()
            let name = String(decoding: r.bytes(nameLen), as: UTF8.self)
            let type = r.u8()
            let cards = r.cards()
            let attacks = r.cards()
            let frameLen = r.u8()
            // 0 is the in-band refusal: no frame is ever shorter than 2 bytes.
            out.append(Vector(name: name, type: type, cards: cards, attacks: attacks,
                              wire: frameLen == 0 ? nil : r.bytes(frameLen)))
        }
        XCTAssertEqual(r.at, r.b.count, "the fixture did not read to its end")
        return out
    }

    /// MOVE_* order (c/src/legal.h), the numbering the fixture's `type` is in.
    /// Asserted against MoveWire.wireIndex below rather than trusted, so this
    /// table cannot drift away from the one production uses.
    private let byIndex: [MoveType] = [.attack, .cover, .pass, .pickup, .good, .wait]

    private func move(_ v: Vector) -> Move {
        // attackCards stays nil off a cover, which is how every producer in the
        // app builds one; a cover carries them even where the vector is a
        // deliberately unpaired refusal.
        Move(type: byIndex[v.type], cards: v.cards,
             attackCards: byIndex[v.type] == .cover ? v.attacks : nil)
    }

    private func hex(_ bytes: [UInt8]) -> String {
        bytes.map { String(format: "%02x", $0) }.joined()
    }

    // MARK: the tests

    func testMoveTypeNumberingMatchesTheKernel() {
        for (i, t) in byIndex.enumerated() {
            XCTAssertEqual(MoveWire.wireIndex(t), i, "\(t) is not MOVE_* \(i)")
        }
    }

    /// The SDK encoder (EngineC.apply, MessageEnvelope.apply) against C.
    func testMoveWireMatchesKernelGoldens() throws {
        let vectors = try loadVectors()
        XCTAssertGreaterThan(vectors.filter { $0.wire == nil }.count, 0, "no refusals pinned")
        for v in vectors {
            let bytes = MoveWire.encodeAction(move(v))
            if let wire = v.wire {
                XCTAssertEqual(hex(bytes), hex(wire), v.name)
            } else {
                XCTAssertTrue(bytes.isEmpty, "\(v.name) should have no frame, got \(hex(bytes))")
            }
        }
    }

    /// The online encoder (OnlineGame's POST body) against the same C.
    func testPackedActionMatchesKernelGoldens() throws {
        for v in try loadVectors() {
            if let wire = v.wire {
                XCTAssertEqual(hex(try PackedAction.encode(move(v))), hex(wire), v.name)
            } else {
                XCTAssertThrowsError(try PackedAction.encode(move(v)), v.name) { err in
                    XCTAssertEqual(err as? PackedAction.WireError, .unencodableMove, v.name)
                }
            }
        }
    }

    /// A move of more cards than a byte can count. The Swift encoder this
    /// replaced wrote `UInt8(move.cards.count)` and TRAPPED here; the kernel
    /// refuses it, well below its own 28-card cap. Not a fixture vector because
    /// a 300-card move is not worth 600 bytes of file to say the same thing.
    func testOverLongMoveIsRefusedNotTrapped() {
        let many = (0..<300).map { Card(s: $0 % 4, v: ($0 % 13) + 1) }
        XCTAssertTrue(MoveWire.encodeAction(Move(type: .attack, cards: many)).isEmpty)
        XCTAssertThrowsError(try PackedAction.encode(Move(type: .attack, cards: many)))
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
