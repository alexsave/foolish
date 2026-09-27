import XCTest
@testable import FoolishKit

/// An undone throw-in flies home from WHERE IT SAT, never from another slot.
///
/// Owner, on build 72: "the throw in works fine. animates, then collapses. Then
/// if you hit UNDO, the card that you just threw in jumps from its position on
/// the table to the center of the table, then animates back to your hand."
///
/// Reproduced on the Pro Max sim with the rig's squares: the thrown card's slot
/// at x=276.8 in the drawer, its flight starting at x=172.8 - slot 0 of the
/// TWO-pair row. The flight log says why. The undo publishes the new board, and
/// its re-laid-out table (two pairs, no thrown card) reached
/// `lastBattleCardFrames` BEFORE `flyUndoReturn` read it, 16ms and a Task later;
/// the card's own frame was gone, and the fallback was
/// `lastBattleFrames.values.first` - whichever slot a Dictionary lists first,
/// which changes from launch to launch (build 71's table gave slot 1, 35pt off;
/// 72's gave slot 0, 104pt off). So: snapshot the frames when the undo starts,
/// and fall back to the card's OWN slot or not at all.
@MainActor
final class UndoFlightSourceTests: XCTestCase {

    private let thrown = Card(s: 0, v: 14)
    private var battles: [BattleView] {
        [BattleView(attack: Card(s: 1, v: 14), defense: Card(s: 2, v: 12)),
         BattleView(attack: Card(s: 3, v: 12), defense: Card(s: 2, v: 10)),
         BattleView(attack: thrown, defense: nil)]
    }
    private let slot0 = CGRect(x: 109, y: 0, width: 62, height: 84)
    private let slot1 = CGRect(x: 181, y: 0, width: 62, height: 84)
    private let slot2 = CGRect(x: 253, y: 0, width: 62, height: 84)

    private func source(_ path: String) throws -> String {
        let here = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
        return try String(contentsOf: here.deletingLastPathComponent()
            .appendingPathComponent(path), encoding: .utf8)
    }

    /// MUTANT: the card's own frame ignored.
    func testTheCardsOwnFrameWins() {
        let own = CGRect(x: 259, y: 14, width: 50, height: 70)
        XCTAssertEqual(UndoFlightSource.rect(for: thrown, in: battles,
                                             cardFrames: [thrown.identity: own],
                                             slotFrames: [0: slot0, 1: slot1, 2: slot2],
                                             ownSlotOnly: true), own)
    }

    /// The report, exactly: the re-laid-out table has only slots 0 and 1, and
    /// neither is where the thrown card was. No flight beats a wrong one.
    /// MUTANT: falling back to `slotFrames.values.first`.
    func testACardWhoseSlotIsGoneDoesNotFlyFromAnotherOne() {
        XCTAssertNil(UndoFlightSource.rect(for: thrown, in: battles, cardFrames: [:],
                                           slotFrames: [0: slot0, 1: slot1], ownSlotOnly: true),
                     "flew from another pair's slot")
    }

    /// MUTANT: the slot looked up by anything but the card's own battle index.
    func testAMissingCardFrameFallsBackToItsOwnSlot() {
        for _ in 0..<20 {   // a Dictionary's order is per launch; ask enough to see it
            XCTAssertEqual(UndoFlightSource.rect(for: thrown, in: battles, cardFrames: [:],
                                                 slotFrames: [0: slot0, 1: slot1, 2: slot2],
                                                 ownSlotOnly: true),
                           CGRect(x: slot2.midX - 25, y: slot2.maxY - 70, width: 50, height: 70),
                           "the card as the grid lays it in its OWN slot: 50x70, bottom-centred")
        }
    }

    /// The flag's other state is the old lookup, kept for comparison.
    /// MUTANT: `ownSlotOnly` ignored.
    func testTheFlagOffPathKeepsTheOldLookup() {
        let r = UndoFlightSource.rect(for: thrown, in: battles, cardFrames: [:],
                                      slotFrames: [0: slot0, 1: slot1], ownSlotOnly: false)
        XCTAssertTrue(r == slot0 || r == slot1)
    }

    /// Ships on; a debug build with no `dev.flags` runs what ships.
    /// MUTANT: `ownSlotByDefault = false`.
    func testTheFixShipsOn() {
        XCTAssertTrue(UndoFlightSource.ownSlotByDefault)
        #if DEBUG || SOLO_TESTING
        XCTAssertEqual(UndoFlightSource.ownSlot, UndoFlightSource.ownSlotByDefault)
        #endif
    }

    /// The frames are taken when the undo STARTS - before its Task, before the
    /// 16ms beat - and the flight asks this function, not an inline lookup.
    /// MUTANTS: the snapshot moved inside the Task; the inline lookup restored;
    /// the flight reading `self.lastBattleCardFrames` instead of the snapshot.
    func testTheUndoSnapshotsTheTableBeforeItWaits() throws {
        let board = try BoardSource.text()
        let start = try XCTUnwrap(board.range(of: "func flyUndoReturn("))
        let body = String(board[start.upperBound...].prefix(9000))
        let snap = try XCTUnwrap(body.range(of: "let fromCardFrames = lastBattleCardFrames"),
                                 "flyUndoReturn does not snapshot the card frames")
        let task = try XCTUnwrap(body.range(of: "Task {"))
        XCTAssertLessThan(snap.lowerBound, task.lowerBound, "the snapshot is taken after the wait")
        XCTAssertTrue(body.contains("UndoFlightSource.rect(for: c, in: old.battles"),
                      "the flight does not ask UndoFlightSource")
        XCTAssertTrue(body.contains("cardFrames: ownSlot ? fromCardFrames")
                      && body.contains("slotFrames: ownSlot ? fromSlotFrames"),
                      "the flight reads the live frames, not the snapshot")
        XCTAssertFalse(body.contains("lastBattleFrames.values.first"),
                       "the any-slot fallback is still there")
    }
}
