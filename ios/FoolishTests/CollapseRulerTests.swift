// CollapseRulerTests - the measuring instrument, not the animation.
//
// The ruler exists so a filmed frame can be CLASSIFIED BY CHANNEL: the reader
// (ios/Tools/msgrig.sh's takes, measured off the PNGs) asks "is this pixel red
// / green / magenta / cyan / yellow" and nothing else, because that is the one
// test that survives h264 chroma subsampling and works in either appearance.
//
// That only holds while every band is a LITERAL sRGB colour. `Color.red` and
// friends are dynamic - 255,59,48 in light and 255,69,58 in dark - and swapping
// one in would not fail to compile, would not look different to a human, and
// would silently move every measured edge. So the palette is the thing under
// test here, and the animation is not: the tween's own rule is CollapseTweenTests.
//
// MUTATIONS RUN (2026-09-07, failure text pasted into the branch's report):
//   band 0 -> `.red`          testTheEdgeColoursAreLiteralNotSystem and
//                             testBandZeroMarksTheBoxTop both fail with
//                             `("red") is not equal to ("#FF0000FF")` - which is
//                             the whole point: the two are not the same colour
//                             and only a machine reading the film can tell.
//   `i % 10` -> `i % 5`       testEveryTenthBandIsTheHundredPointMark fails
//                             (`band 5 is not a 100pt mark`), and so does
//                             testBandsAlternateSoThePitchIsMeasurable.
//   magenta/cyan swapped      testBandsAlternateSoThePitchIsMeasurable fails,
//                             12 assertions, everything else stays green.

import XCTest
import SwiftUI
@testable import FoolishKit

final class CollapseRulerTests: XCTestCase {

    /// THE BUG THIS FILE EXISTS FOR. A system colour is appearance-dependent, so
    /// a ruler drawn in one would measure differently in the other - and the
    /// reader has no way to notice, because a slightly-off red is still red to
    /// a human looking at the film.
    func testTheEdgeColoursAreLiteralNotSystem() {
        XCTAssertEqual(CollapseRuler.colour(0), CollapseRuler.pure(1, 0, 0),
                       "band 0 must be literal sRGB red")
        XCTAssertNotEqual(CollapseRuler.pure(1, 0, 0), Color.red,
                          "…and literal sRGB red is NOT Color.red - if these ever "
                          + "compare equal this test has stopped saying anything")
        XCTAssertNotEqual(CollapseRuler.pure(0, 1, 0), Color.green)
        XCTAssertNotEqual(CollapseRuler.pure(1, 1, 0), Color.yellow)
    }

    /// Band 0 is the box's own top edge, which is the landmark every measured
    /// row is counted from.
    func testBandZeroMarksTheBoxTop() {
        XCTAssertEqual(CollapseRuler.colour(0), CollapseRuler.pure(1, 0, 0))
        XCTAssertNotEqual(CollapseRuler.colour(1), CollapseRuler.colour(0),
                          "the band under the top mark must not be the top mark")
    }

    /// The 100pt marks: with a 10pt band, every tenth one. They are what lets a
    /// reader put a number on a distance without counting forty bands.
    func testEveryTenthBandIsTheHundredPointMark() {
        for i in stride(from: 10, through: 80, by: 10) {
            XCTAssertEqual(CollapseRuler.colour(i), CollapseRuler.pure(1, 1, 0),
                           "band \(i) is at \(i * Int(CollapseRuler.band))pt and must be yellow")
        }
        for i in [1, 5, 9, 11, 15, 19] {
            XCTAssertNotEqual(CollapseRuler.colour(i), CollapseRuler.pure(1, 1, 0),
                              "band \(i) is not a 100pt mark")
        }
    }

    /// Between the marks the bands alternate, which is what makes the PITCH
    /// measurable: the reader takes the spacing of the magenta runs and divides
    /// by two, and a pitch that is not 10pt means the imagery was scaled on its
    /// way to the screen.
    func testBandsAlternateSoThePitchIsMeasurable() {
        let magenta = CollapseRuler.pure(1, 0, 1), cyan = CollapseRuler.pure(0, 1, 1)
        for i in [1, 3, 5, 7, 9, 11] {
            XCTAssertEqual(CollapseRuler.colour(i), magenta, "band \(i)")
        }
        for i in [2, 4, 6, 8, 12, 14] {
            XCTAssertEqual(CollapseRuler.colour(i), cyan, "band \(i)")
        }
    }

    /// The band is 10pt because the measurement is reported in whole points and
    /// a 3x device renders 10pt as 30 pixels - wide enough to survive a resize
    /// and narrow enough that the box top is located to a point.
    func testTheBandIsTenPoints() {
        XCTAssertEqual(CollapseRuler.band, 10)
    }
}
