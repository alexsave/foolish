import XCTest
@testable import FoolishKit

/// The pass preview's empty slot holds through a crossing and a release.
///
/// Owner, on the pass: "the worst". Filmed with the rig's squares, yellow's pair
/// slid from x 251 to 216 as the slot opened, back to 241 as the finger crossed
/// the cyan pair, to 227, and on release snapped to 235 before sliding to its
/// final 215. The kernel now keeps the slot (`anim_pass_slot_shown`); the C
/// tests hold its rules, and these hold the board to asking it.
@MainActor
final class PassSlotTests: XCTestCase {

    private func source(_ path: String) throws -> String {
        let here = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
        return try String(contentsOf: here.deletingLastPathComponent()
            .appendingPathComponent(path), encoding: .utf8)
    }

    func testBothRulesShipOn() {
        XCTAssertTrue(PassSlot.holdByDefault)
        XCTAssertTrue(PassSlot.stickyByDefault)
    }

    /// MUTANT: the Swift wire dropping a rule bit.
    func testTheWireCarriesBothRulesToTheKernel() {
        XCTAssertTrue(PassSlotWire.shown(previewing: false, dragging: false, seenThisDrag: false,
                                         overDeadPair: false, heldAt: 2, battles: 2,
                                         hold: true, sticky: true),
                      "released pass, pair not landed: the slot stays")
        XCTAssertFalse(PassSlotWire.shown(previewing: false, dragging: false, seenThisDrag: false,
                                          overDeadPair: false, heldAt: 2, battles: 3,
                                          hold: true, sticky: true),
                       "the pair landed: the slot goes")
        XCTAssertTrue(PassSlotWire.shown(previewing: false, dragging: true, seenThisDrag: true,
                                         overDeadPair: true, heldAt: nil, battles: 2,
                                         hold: true, sticky: true),
                      "crossing a pair it cannot go on: the slot stays")
        XCTAssertFalse(PassSlotWire.shown(previewing: false, dragging: true, seenThisDrag: true,
                                          overDeadPair: true, heldAt: nil, battles: 2,
                                          hold: true, sticky: false),
                       "sticky off: the old close-on-crossing")
    }

    /// MUTANTS: the grid reading the bare preview again; the release not
    /// recording the held count; a new drag not resetting the crossing memory.
    func testTheBoardAsksTheKernelAndFeedsItTheDrag() throws {
        let board = try BoardSource.text()
        XCTAssertTrue(board.contains("let passPreview = sweeping ? false : passSlotShown(view)"),
                      "the table's slot must be the kernel's answer, not the bare preview")
        XCTAssertTrue(board.contains("if passing, target == .table { passHeldAt = view.battles.count }"),
                      "a released pass must hold its slot")
        XCTAssertTrue(board.contains("if dragCard == nil { passSeenThisDrag = false; passHeldAt = nil }"),
                      "a new drag starts with no memory of the last one")
        XCTAssertTrue(board.contains("playInFlight = false\n            passHeldAt = nil"),
                      "the hold ends with the play")
    }
}
