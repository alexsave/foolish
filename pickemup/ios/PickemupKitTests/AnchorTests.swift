// AnchorTests.swift - the anchors a REAL TableScreen reports, which every
// flight aims at. BeatPlayerTests feed the player synthetic anchors, so they
// could not see the pile's anchor sitting at the board's origin: on the
// simulator every play flew to the top-left corner and snapped onto the pile
// (pickemup/docs/MOTION_REPORT.md, the filmed takes of 2026-09-27).

import CPickemup
import SwiftUI
import XCTest
@testable import PickemupKit

@MainActor
final class AnchorTests: XCTestCase {

    // MUTATE: TableScreen moves the stack anchor with `.offset` (in an overlay)
    // instead of laying it out with `.position` -> "the pile's anchor is the
    // pile".
    func testTheAnchorsAreWhereTheViewsAre() throws {
        XCTAssertNotNil(Phones.dmStartedByBo())
        let m = TableModel()
        var anchors: [String: CGRect] = [:]
        let outer = CGSize(width: 390, height: 700)
        AXTree.hosted(TableScreen(model: m, onRules: {}).onPreferenceChange(PkAnchorKey.self) { anchors = $0 },
                      size: outer) { _ in }
        let inset = PkLayout.boardInset
        let board = try XCTUnwrap(anchors["board"]?.size, "the board reports its frame")
        XCTAssertEqual(board.width, outer.width - inset.leading - inset.trailing, accuracy: 0.5)

        let pc = PkLayout.pileCentre(board: board, collapse: PkLayout.collapse(viewHeight: outer.height))
        let stack = try XCTUnwrap(anchors["stack"])
        XCTAssertEqual(stack.midX, pc.x, accuracy: 0.5, "the pile's anchor is the pile")
        XCTAssertEqual(stack.midY, pc.y, accuracy: 0.5, "the pile's anchor is the pile")
        XCTAssertEqual(stack.size, PkLayout.pileSize, "one pile card big")

        // the deck beside it, and every other anchor inside the board
        let deck = try XCTUnwrap(anchors["deck"])
        XCTAssertLessThan(deck.maxX, stack.minX, "the deck sits left of the pile (U3)")
        XCTAssertEqual(deck.midY, stack.midY, accuracy: 0.5, "on its line")
        for (name, r) in anchors where name != "board" {
            XCTAssertTrue(CGRect(origin: .zero, size: board).insetBy(dx: -1, dy: -1).contains(r), "\(name) \(r) is on the board")
        }
    }
}
