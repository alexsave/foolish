// DiceTableLayoutTests - every seat count from 2 to 6, for me in any seat or
// a spectator, on the boards the table is drawn at: no two seats overlap,
// none overlaps the bid plate, and all stay on the board.

import XCTest
@testable import ChuiniuKit

final class DiceTableLayoutTests: XCTestCase {
    /// An expanded drawer's table above the picker on a small and a large
    /// phone.
    private let boards = [CGSize(width: 358, height: 380), CGSize(width: 398, height: 460)]

    func testSeatsNeverOverlapEachOtherOrThePlate() {
        for board in boards {
            for n in 2...6 {
                for me in [nil] + (0..<n).map({ Optional($0) }) {
                    let frames = DiceTableLayout.seatFrames(count: n, me: me, board: board)
                    let plate = DiceTableLayout.plateFrame(board: board)
                    let tag = "\(n) seats, me \(me.map(String.init) ?? "none"), board \(board)"
                    XCTAssertEqual(frames.count, n, "\(tag): a frame per seat")
                    let bounds = CGRect(origin: .zero, size: board)
                    for i in 0..<n {
                        XCTAssertFalse(frames[i].isEmpty, "\(tag): seat \(i) is placed")
                        XCTAssertTrue(bounds.contains(frames[i]), "\(tag): seat \(i) \(frames[i]) is on the board")
                        XCTAssertFalse(frames[i].intersects(plate), "\(tag): seat \(i) clears the bid plate")
                        for j in (i + 1)..<n {
                            XCTAssertFalse(frames[i].intersects(frames[j]),
                                           "\(tag): seats \(i) \(frames[i]) and \(j) \(frames[j]) do not overlap")
                        }
                    }
                }
            }
        }
    }

    func testMySeatIsTheBottomBand() {
        let board = boards[0]
        let frames = DiceTableLayout.seatFrames(count: 4, me: 2, board: board)
        XCTAssertEqual(frames[2].maxY, board.height, accuracy: 0.5, "my seat sits on the bottom edge")
        for s in [0, 1, 3] {
            XCTAssertLessThan(frames[s].maxY, frames[2].minY, "seat \(s) is above mine")
        }
        XCTAssertLessThan(frames[3].midX, frames[0].midX, "the seat after mine is the leftmost")
    }
}
