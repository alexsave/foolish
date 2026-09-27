// CompactTests.swift - O10 on a REAL TableScreen: in the iPhone 17e's 299pt
// drawer (and a 340pt one) no pill is over the pile, the deck is not over the
// status corner, and the pile is clear of the fan across the table. The
// numbers are the kernel's (pk_lay_table_scale, pk_lay_pile); this reads back
// the frames the views actually laid out, which is where the collision was
// seen (pickemup/docs/shots/compact_collision.png).

import CPickemup
import SwiftUI
import XCTest
@testable import PickemupKit

@MainActor
final class CompactTests: XCTestCase {

    /// Bo's turn in a DM, one card drawn: the strip is up and Pass stands
    /// beside Draw, the exact state of compact_collision.png.
    private func anchorsAfterADraw(viewHeight h: CGFloat) throws -> [String: CGRect] {
        XCTAssertNotNil(Phones.dmStartedByBo())
        let m = TableModel()
        m.draw()
        XCTAssertFalse(m.strip.isEmpty, "the strip is up")
        var anchors: [String: CGRect] = [:]
        AXTree.hosted(TableScreen(model: m, onRules: {}).onPreferenceChange(PkAnchorKey.self) { anchors = $0 },
                      size: CGSize(width: 390, height: h)) { _ in }
        XCTAssertNotNil(anchors["pill.pass"], "Pass beside Draw")
        return anchors
    }

    private func checkTheBand(viewHeight h: CGFloat, scale want: CGFloat) throws {
        let a = try anchorsAfterADraw(viewHeight: h)
        let board = try XCTUnwrap(a["board"]?.size)
        let s = PkLayout.tableScale(board: board, collapse: PkLayout.collapse(viewHeight: h))
        XCTAssertEqual(s, want, accuracy: 0.001, "the kernel's scale for a \(h)pt drawer")
        let stack = try XCTUnwrap(a["stack"]), deck = try XCTUnwrap(a["deck"])
        let status = try XCTUnwrap(a["status"]), fan = try XCTUnwrap(a["fan.0"])
        XCTAssertEqual(stack.width, PkLayout.pileSize.width * s, accuracy: 0.5, "the pile is drawn at the scale")
        XCTAssertEqual(deck.width, PkLayout.deckSize.width * s, accuracy: 0.5, "the deck is drawn at the scale")
        XCTAssertEqual(deck.midY, stack.midY, accuracy: 0.5, "the deck on the pile's line (U3)")
        // the pile's card with its -3 degree lean, and the deck's eight layers
        // leaning 1 left and 2 up each (DeckStack.leanX, leanY)
        let pile = stack.insetBy(dx: -3 * s, dy: -3 * s)
        let lean = CGFloat(PkLayout.deckLayers(Pk.view()?.deckN ?? 0) - 1)
        let deckDrawn = deck.union(deck.offsetBy(dx: -lean * DeckStack.leanX * s, dy: -lean * DeckStack.leanY * s))
        for name in ["pill.draw", "pill.pass", "pills"] {
            let pill = try XCTUnwrap(a[name], name)
            XCTAssertFalse(pill.intersects(pile), "no pill over the pile (\(name) \(pill), pile \(pile))")
        }
        XCTAssertFalse(deckDrawn.intersects(status), "the deck is not over the status corner (\(deckDrawn), \(status))")
        XCTAssertLessThan(fan.maxY, pile.minY, "the pile clear of the fan across the table")
    }

    // MUTATE: TableScreen draws the pile and the deck at scale 1 (`let scale:
    // CGFloat = 1`) -> "no pill over the pile", "the pile is drawn at the scale".
    // MUTATE: StatusCorner always gets the sub-line -> "the deck is not over
    // the status corner".
    func testTheIPhone17eDrawerKeepsThePileInTheBand() throws {
        try checkTheBand(viewHeight: 299, scale: 0.80977)
    }

    func testA340DrawerKeepsItToo() throws {
        try checkTheBand(viewHeight: 340, scale: 1)
    }
}
