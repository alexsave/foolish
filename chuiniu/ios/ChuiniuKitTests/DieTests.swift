// DieTests - the pip table: every face shows as many pips as its value, all
// on the face, none touching, and a blank for anything that is not 1 to 6.

import XCTest
@testable import ChuiniuKit

final class DieTests: XCTestCase {
    func testEveryFaceHasAsManyPipsAsItsValue() {
        for face in 1...6 {
            XCTAssertEqual(Die.pips(face).count, face, "face \(face) shows \(face) pips")
        }
    }

    func testPipsSitOnTheFaceAndNeverTouch() {
        for face in 1...6 {
            let pips = Die.pips(face)
            let d = Die.pipDiameter(face)
            for p in pips {
                XCTAssertTrue(p.x - d / 2 >= 0 && p.x + d / 2 <= 1 && p.y - d / 2 >= 0 && p.y + d / 2 <= 1,
                              "face \(face): a pip at \(p) stays on the face")
            }
            for i in pips.indices {
                for j in pips.indices where j > i {
                    XCTAssertGreaterThan(hypot(pips[i].x - pips[j].x, pips[i].y - pips[j].y), d,
                                         "face \(face): pips \(i) and \(j) do not touch")
                }
            }
        }
    }

    func testAnUnknownFaceIsBlank() {
        XCTAssertEqual(Die.pips(0).count, 0, "0 is blank")
        XCTAssertEqual(Die.pips(7).count, 0, "7 is blank")
    }
}
