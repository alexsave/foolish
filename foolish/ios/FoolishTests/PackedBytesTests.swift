// PackedBytesTests.swift - the shared byte reader's "whole field or nothing".
//
// shared/swift/PackedBytes.swift promises that a read which returns nil moves
// nothing. A length-prefixed read used to consume its prefix before finding the
// body short, so a caller that took nil as "field absent" and read on was one
// or two bytes out (SECURITY_REVIEW_SHARED.md, PackedBytes). And a start
// outside the buffer or a huge count trapped instead of reading nothing.
import XCTest
@testable import FoolishKit

final class PackedBytesTests: XCTestCase {

    func testShortBlobLeavesTheCursorAlone() {
        // a u16 prefix saying 5 bytes, then only 2 of them
        var r = PackedReader([0x05, 0x00, 0xAA, 0xBB])
        XCTAssertNil(r.blob())
        XCTAssertEqual(r.at, 0)
        XCTAssertNil(r.text())
        XCTAssertEqual(r.at, 0)
        XCTAssertEqual(r.u16(), 5, "the prefix is still there to read")
    }

    func testShortBlob8LeavesTheCursorAlone() {
        var r = PackedReader([0x01, 0x03, 0xAA])
        XCTAssertEqual(r.u8(), 1)
        XCTAssertNil(r.blob8())
        XCTAssertEqual(r.at, 1)
        XCTAssertEqual(r.u8(), 3)
    }

    func testWholeBlobsStillRead() {
        var w = PackedWriter()
        w.text("hi"); w.blob8([7, 8]); w.u32(0xDEADBEEF)
        var r = PackedReader(w.bytes)
        XCTAssertEqual(r.text(), "hi")
        XCTAssertEqual(r.blob8(), [7, 8])
        XCTAssertEqual(r.u32(), 0xDEADBEEF)
        XCTAssertTrue(r.isAtEnd)
    }

    func testHugeCountReadsNothing() {
        var r = PackedReader([1, 2, 3], at: 1)
        XCTAssertNil(r.u8s(Int.max))
        XCTAssertNil(r.u8s(-1))
        XCTAssertEqual(r.at, 1)
        XCTAssertEqual(r.u8s(2), [2, 3])
    }

    func testStartOutsideTheBufferIsTheEnd() {
        var neg = PackedReader([1, 2, 3], at: -1)
        XCTAssertTrue(neg.isAtEnd)
        XCTAssertNil(neg.u8())
        var past = PackedReader(Data([1, 2, 3]), at: 9)
        XCTAssertTrue(past.isAtEnd)
        XCTAssertNil(past.blob())
        var mid = PackedReader([1, 2, 3], at: 2)
        XCTAssertEqual(mid.u8(), 3)
    }
}
