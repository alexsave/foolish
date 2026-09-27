// TableModelTests.swift - the view model against the real kernel: every touch
// is one kernel call, and what it stages (or does not) is the kernel's word.
//
// Each test names the mutation it must go red on (MUTATE:), listed with the
// rest in pickemup/ios/TESTS_MUTATED.md with the assertion each went red on.

import CPickemup
import XCTest
@testable import PickemupKit

@MainActor
final class TableModelTests: XCTestCase {

    private var stages: [TableModel.Stage] = []

    private func model() -> TableModel {
        let m = TableModel()
        stages = []
        m.onStage = { [weak self] in self?.stages.append($0) }
        return m
    }

    // MUTATE: stageIfSendable's canSend guard never refuses -> "a draw stages nothing".
    func testDrawOpensTheDraftAndStagesNothing() throws {
        XCTAssertNotNil(Phones.dmStartedByBo())
        let m = model()
        XCTAssertTrue(m.myTurn, "Bo moves first (D28)")
        let before = m.hand.count
        m.draw()
        XCTAssertEqual(m.hand.count, before + 1, "the drawn card is in the hand")
        XCTAssertEqual(m.hand.last, try XCTUnwrap(Pk.draftPlan().last { $0.kind == PK_EV_DRAW }).card,
                       "a drawn card goes on the right (D24)")
        XCTAssertEqual(m.table?.draft, 1, "a draft is open")
        XCTAssertEqual(m.table?.canSend, 0, "mid-turn: it cannot be sealed")
        XCTAssertTrue(stages.isEmpty, "a draw stages nothing")
        XCTAssertEqual(m.strip.draws, 1, "the strip counts the draw")
        XCTAssertEqual(m.pills.trailing, .draw, "Draw keeps the trailing slot (U9)")
        XCTAssertEqual(m.pills.leading, .pass, "and Pass stands beside it (D10)")
    }

    // MUTATE: TableModel.play stages with collapse: false -> "a play collapses".
    // MUTATE: stageIfSendable passes Pk.words(PK_API_W_HEADLINE) as the
    // caption -> "the caption is the kernel's staged caption".
    func testPlayStagesWithTheKernelsCaption() throws {
        var found = false
        for k in 0..<50 {
            Phones.dmStartedByBo(seed: k)
            if Phones.plainPlayable() != nil { found = true; break }
        }
        XCTAssertTrue(found, "a deal where the starter can play")
        let m = model()
        let pos = try XCTUnwrap(Phones.plainPlayable())
        let card = m.hand[pos]
        m.play(pos)
        XCTAssertEqual(stages.count, 1, "a play stages once")
        XCTAssertEqual(stages.first?.caption, Pk.words(PK_API_W_STAGED_CAPTION),
                       "the caption is the kernel's staged caption")
        XCTAssertFalse(stages.first?.caption.isEmpty ?? true)
        XCTAssertEqual(stages.first?.collapse, true, "a play collapses")
        XCTAssertEqual(m.view?.top, card, "the card is on the pile")
        XCTAssertEqual(m.strip.played, card, "the strip shows the played card")
    }

    // MUTATE: TableModel.play skips the Pk.isWild branch (plays with
    // PK_NO_SUIT) -> "nothing is staged before a suit".
    // MUTATE: TableModel.choose plays suit 0 whatever was tapped -> "the
    // chosen suit is live".
    func testAWildWaitsForItsSuit() throws {
        let pos = try XCTUnwrap(Phones.dmWithWild(), "a deal with a wild in the starter's hand")
        let m = model()
        m.play(pos)
        XCTAssertEqual(m.pickerFor, pos, "the picker is up")
        XCTAssertTrue(stages.isEmpty, "nothing is staged before a suit")
        XCTAssertEqual(m.table?.draft ?? 0, 0, "and nothing was applied (D17)")
        XCTAssertEqual(m.pills.trailing, .none, "no pills under the picker")
        XCTAssertEqual(m.headline, Pk.string("HEAD_PICK_SUIT"))
        m.choose(2)
        XCTAssertNil(m.pickerFor)
        XCTAssertEqual(stages.count, 1, "the choice stages the wild")
        XCTAssertEqual(m.view?.liveSuit, 2, "the chosen suit is live")
        XCTAssertEqual(m.strip.chosen, 2)
    }

    // MUTATE: cancelPicker plays the wild with suit 0 -> "cancel stages nothing".
    func testCancellingThePickerLeavesNothingStaged() throws {
        let pos = try XCTUnwrap(Phones.dmWithWild())
        let m = model()
        let hand = m.hand
        m.play(pos)
        m.cancelPicker()
        XCTAssertNil(m.pickerFor)
        XCTAssertTrue(stages.isEmpty, "cancel stages nothing")
        XCTAssertEqual(m.hand, hand, "the wild is home")
    }

    // MUTATE: TableModel.undo ignores Pk.undo's refusal (no refusedBelowFloor)
    // -> "the refusal says drawn cards stay".
    func testUndoOfADrawIsRefusedAndSaysSo() {
        Phones.dmStartedByBo()
        let m = model()
        m.draw()
        let after = m.hand
        m.undo()
        XCTAssertEqual(m.hand, after, "a drawn card never comes back (D8)")
        XCTAssertTrue(m.drawnStay, "the refusal says drawn cards stay (U23)")
        XCTAssertEqual(m.subline, Pk.string("SUB_DRAWN_STAY"))
    }

    // MUTATE: Pk.undo always answers false -> "the play comes back".
    func testUndoOfAPlayBringsTheCardHome() throws {
        var found = false
        for k in 0..<50 {
            Phones.dmStartedByBo(seed: k)
            if Phones.plainPlayable() != nil { found = true; break }
        }
        XCTAssertTrue(found)
        let m = model()
        let hand = m.hand
        m.play(try XCTUnwrap(Phones.plainPlayable()))
        XCTAssertEqual(m.pills.trailing, .undo, "Undo alone takes the trailing slot (U9)")
        m.undo()
        XCTAssertEqual(m.hand, hand, "the play comes back")
        XCTAssertNil(m.strip.played)
    }

    // MUTATE: tapFan never un-calls (drop the `calling(seat)` branch) ->
    // "a second tap un-calls".
    func testATapOnAFanStagesTheCatchAndASecondTakesItBack() {
        Phones.dmStartedByBo()
        let m = model()
        guard m.mayCall(0) else { return XCTFail("every unstamped fan is tappable (D5c)") }
        m.tapFan(0)
        XCTAssertTrue(m.calling(0), "the call is staged on Alex's fan")
        XCTAssertEqual(m.strip.called, 0)
        m.tapFan(0)
        XCTAssertFalse(m.calling(0), "a second tap un-calls")
    }

    // MUTATE: Pk.read adopts nothing (answers 0 without reading) -> Alex is
    // not seated by the bubble ("XCTAssertEqual t.me 0").
    func testTheStagedLinkRoundTripsToTheOtherPhone() throws {
        var found = false
        for k in 0..<50 {
            Phones.dmStartedByBo(seed: k)
            if Phones.plainPlayable() != nil { found = true; break }
        }
        XCTAssertTrue(found)
        let m = model()
        let parent = try XCTUnwrap(Pk.text)
        m.play(try XCTUnwrap(Phones.plainPlayable()))
        let staged = try XCTUnwrap(Pk.text)
        XCTAssertEqual(Pk.check(staged), 0, "the staged link reads")
        XCTAssertLessThan(Pk.prefer(staged, over: parent), 0, "the child beats its parent (4.8)")
        XCTAssertTrue(Pk.commit(), "sent")
        Phones.be(0)
        Pk.sender(of: staged, isDM: true, iSent: false)
        XCTAssertEqual(Pk.read(staged), 0, "Alex reads Bo's bubble")
        let t = try XCTUnwrap(Pk.table())
        XCTAssertEqual(t.me, 0)
        XCTAssertEqual(t.bubbles, 1)
        XCTAssertEqual(Pk.view()?.myHand.count, 7, "Alex's own seven")
    }

    // MUTATE (C): pk_view fills reveal[] while the game is live -> "no count
    // while it is played". The struct has no other place for one (D22).
    func testTheMaskedViewNeverExposesAnotherSeatsCount() throws {
        Phones.dmStartedByBo()
        let v = try XCTUnwrap(Pk.view())
        XCTAssertTrue(v.reveal.allSatisfy { $0.card.isEmpty }, "no count while it is played")
        let labels = Mirror(reflecting: v).children.compactMap(\.label)
        XCTAssertFalse(labels.contains { $0.lowercased().contains("count") || $0 == "handN" },
                       "the view has no per-seat count field: \(labels)")
        XCTAssertEqual(Pk.ranks(), [], "a live game ranks nobody")
    }

    // MUTATE: TableModel.join always calls Pk.join (never joinStart) ->
    // "the second player's join starts the game".
    func testJoiningADMStartsIt() throws {
        Phones.reset()
        Phones.be(0)
        XCTAssertTrue(Pk.newGame(dm: true, seed: Phones.seed(3)))
        let invite = try XCTUnwrap(Pk.text)
        Phones.be(1)
        XCTAssertEqual(Pk.read(invite), 0)
        let m = model()
        XCTAssertEqual(m.table?.offered, PK_LOBBY_JOIN)
        m.join()
        XCTAssertEqual(m.phase, PK_PHASE_LIVE, "the second player's join starts the game")
        XCTAssertEqual(stages.first?.caption, Pk.words(PK_API_W_CAPTION, 0), "captioned as the deal")
    }
}
