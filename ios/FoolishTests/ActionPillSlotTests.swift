import XCTest
@testable import FoolishKit

/// The action pill sits in one place whatever word is on it, and mirrors the
/// settings gear across the board.
///
/// Measured before the fix, on the simulator: Pickup 24.0pt from the right
/// edge, Undo 28.0pt, the gear 24.0pt from the left. Round 10g hoisted Undo
/// into its own slot with a hardcoded trailing 20 where the column it left sits
/// at 4 + FSpace.m = 16. See `ActionPillSlot`.
///
/// Every assertion here was mutation-checked with a compiling mutant; each one
/// is named on the test that caught it.
final class ActionPillSlotTests: XCTestCase {

    private func source(_ path: String) throws -> String {
        let here = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
        return try String(contentsOf: here.deletingLastPathComponent()
            .appendingPathComponent(path), encoding: .utf8)
    }

    /// MUTANT: `undoTrailing` returning `legacyUndoTrailing` on the aligned path.
    func testUndoSitsWhereEveryOtherPillSits() {
        XCTAssertEqual(ActionPillSlot.undoTrailing(aligned: true), ActionPillSlot.pillTrailing,
                       "Undo must use the column's own inset, or it lands 4pt off Pickup")
        XCTAssertEqual(ActionPillSlot.pillTrailing,
                       ActionPillSlot.outerInset + FActionBar.innerInset)
    }

    /// The flag's OTHER state is round 10g's inset, kept for comparison.
    /// MUTANT: `legacyUndoTrailing = 16`.
    func testTheFlagOffPathKeepsRound10gsInset() {
        XCTAssertEqual(ActionPillSlot.undoTrailing(aligned: false), 20)
        XCTAssertNotEqual(ActionPillSlot.undoTrailing(aligned: false),
                          ActionPillSlot.undoTrailing(aligned: true),
                          "a flag whose two states are the same path is not a flag")
    }

    /// Ships ON, and a debug build with no `dev.flags` file runs what ships.
    /// MUTANTS: `alignedByDefault = false`; `flag(_:shipping:)` returning a
    /// literal `false`.
    func testTheAlignedSlotShipsAndDebugAgrees() {
        XCTAssertTrue(ActionPillSlot.alignedByDefault)
        #if DEBUG || SOLO_TESTING
        XCTAssertEqual(ActionPillSlot.aligned, ActionPillSlot.alignedByDefault)
        let unset = "round47.never.set.\(UUID().uuidString)"
        XCTAssertTrue(MessageDevBoard.flag(unset, shipping: true))
        XCTAssertFalse(MessageDevBoard.flag(unset, shipping: false))
        #endif
    }

    /// The drift that happened was a number typed at the placement. All three
    /// placements - the pill column, the Undo slot and the settings squares -
    /// must read the shared insets, and Undo must be the column's width.
    /// MUTANTS: the Undo placement back to `.padding(.trailing, 20)`; the
    /// squares' leading back to a literal `4`; Undo's width back to a literal.
    func testNoPlacementTypesItsOwnInset() throws {
        let board = try source("FoolishKit/Boards/MessageTableView.swift")
        XCTAssertTrue(board.contains(".padding(.trailing, ActionPillSlot.undoTrailing(aligned: ActionPillSlot.aligned))"),
                      "the Undo slot no longer reads ActionPillSlot")
        XCTAssertTrue(board.contains(".padding(.trailing, ActionPillSlot.outerInset)"),
                      "the pill column no longer reads ActionPillSlot")
        XCTAssertTrue(board.contains(".padding(.leading, ActionPillSlot.outerInset)"),
                      "the settings squares no longer mirror the pill column")
        XCTAssertFalse(board.contains("fixedWidth: 96"),
                       "a pill width is typed as a number again")
        let squares = try source("FoolishKit/DesignSystem/FSquareButton.swift")
        XCTAssertTrue(squares.contains(".padding(.horizontal, FActionBar.innerInset)"),
                      "the settings squares' inner inset no longer matches the pills'")
    }
}
