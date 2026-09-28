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
        // the whole window, as the extension's GeometryReader sees its view
        AXTree.hosted(TableScreen(model: m, onRules: {}).ignoresSafeArea()
                        .onPreferenceChange(PkAnchorKey.self) { anchors = $0 },
                      size: CGSize(width: 390, height: h)) { _ in }
        XCTAssertNotNil(anchors["pill.pass"], "Pass beside Draw")
        return anchors
    }

    private func checkTheBand(viewHeight h: CGFloat, scale want: CGFloat) throws {
        let a = try anchorsAfterADraw(viewHeight: h)
        let inset = PkLayout.boardInset
        let board = CGSize(width: 390 - inset.leading - inset.trailing, height: h - inset.top - inset.bottom)
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
        for name in ["pill.draw", "pill.pass"] {
            let pill = try XCTUnwrap(a[name], name)
            XCTAssertFalse(pill.intersects(pile), "no pill over the pile (\(name) \(pill), pile \(pile))")
        }
        XCTAssertFalse(deckDrawn.intersects(status), "the deck is not over the status corner (\(deckDrawn), \(status))")
        let strip = try XCTUnwrap(a["strip"])
        XCTAssertLessThanOrEqual(status.maxY, strip.maxY + 1.5, "the drawer's status corner ends with its strip: no sub-line")
        XCTAssertLessThan(fan.maxY, pile.minY, "the pile clear of the fan across the table")
    }

    // MUTATE: TableScreen draws the pile and the deck at scale 1 (`let scale:
    // CGFloat = 1`) -> "the pile is drawn at the scale", "the pile clear of the
    // fan across the table".
    // MUTATE: the pills never stack (`let stacked = false`) -> "no pill over
    // the pile".
    // MUTATE: SeatBadge keeps its stamp slot in the VStack -> "the pile clear
    // of the fan across the table" (the fan is 30 below the ring point, not 9).
    // The iPhone 17e simulator's drawer, measured through dev.anchors: a 281pt
    // view (263 of board).
    func testTheIPhone17eDrawerKeepsThePileInTheBand() throws {
        try checkTheBand(viewHeight: 281, scale: 0.69968)
    }

    func testA299DrawerKeepsItToo() throws {
        try checkTheBand(viewHeight: 299, scale: 0.85970)
    }

    func testA340DrawerKeepsItToo() throws {
        try checkTheBand(viewHeight: 340, scale: 1)
    }
}
