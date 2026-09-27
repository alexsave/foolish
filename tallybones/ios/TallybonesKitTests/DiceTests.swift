// DiceTests.swift - the die's face, the tray's keep toggling and T11's blanks
// over the real kernel (BridgeKernel, tb_api.h), the kernel's settle on the
// tray, the scorecard's rows, the bubble picture, and a receiver's dice. Each
// test's mutation, and the assertion it went red on, is in ../TESTS_MUTATED.md.

import SwiftUI
import XCTest
@testable import TallybonesKit

@MainActor
final class DiceFaceTests: XCTestCase {

    func testEveryValueHasThatManyPipsAndUnknownHasNone() {
        XCTAssertEqual((0...6).map { DiceFace.pips($0).count }, [0, 1, 2, 3, 4, 5, 6], "pips per value 0...6")
        XCTAssertTrue(DiceFace.pips(7).isEmpty, "no face past six")
        XCTAssertTrue(DiceFace.pips(-1).isEmpty, "no face below one")
        for v in 1...6 {
            let p = DiceFace.pips(v)
            XCTAssertEqual(Set(p.map { "\($0.x),\($0.y)" }).count, p.count, "no two pips of \(v) coincide")
            XCTAssertTrue(p.allSatisfy { (0.2...0.8).contains($0.x) && (0.2...0.8).contains($0.y) },
                          "every pip of \(v) sits inside the face")
        }
        XCTAssertEqual(DiceFace.pips(1), [CGPoint(x: 0.5, y: 0.5)], "one is the centre pip")
    }
}

@MainActor
final class TrayTests: XCTestCase {

    /// Alex's phone in hand, seat 0, roll 1 of the first turn.
    private func table() throws -> TallyTable {
        try XCTUnwrap(Phones.dmStarted(), "a DM started")
        return TallyTable(kernel: BridgeKernel())
    }

    func testATapTogglesTheKeepMarkAndASecondTapClearsIt() throws {
        let t = try table()
        XCTAssertTrue(t.tray.canKeep, "roll 1 of my turn may be kept")
        XCTAssertEqual(t.tray.roll, 1)
        XCTAssertTrue(t.tray.allKnown, "roll 1's five dice are derived when the turn begins (T6)")
        XCTAssertEqual(t.tray.kept, [false, false, false, false, false], "nothing kept at roll 1")
        t.toggle(2)
        XCTAssertEqual(t.tray.kept, [false, false, true, false, false], "a tap keeps die 2")
        t.toggle(0)
        t.toggle(2)
        XCTAssertEqual(t.tray.kept, [true, false, false, false, false], "a second tap on die 2 clears it")
        t.toggle(9)
        XCTAssertEqual(t.tray.kept, [true, false, false, false, false], "a die that is not there changes nothing")
    }

    func testAStagedKeepShowsBlanksUntilItIsSentThenTheRerollSettlesIn() throws {
        let t = try table()
        let roll1 = t.tray.dice
        var stages: [TallyStage] = []
        t.onStage = { stages.append($0) }
        for i in [0, 1, 4] { t.toggle(i) }
        t.roll()
        XCTAssertEqual(stages.count, 1, "Roll stages one KEEP")
        XCTAssertEqual(t.view.draft, .keep)
        XCTAssertEqual(t.tray.dice, [roll1[0], roll1[1], 0, 0, roll1[4]], "T11: the rerolling dice are blank while staged")
        XCTAssertTrue(stages[0].caption.hasSuffix("rerolls two"), "the KEEP's caption: \(stages[0].caption)")
        XCTAssertFalse(t.tray.canKeep, "no toggling under a staged KEEP")
        XCTAssertFalse(t.tray.canScore, "no scoring blank dice")
        XCTAssertEqual(t.preview, Array(repeating: nil, count: Category.count), "no preview of blank dice")
        t.toggle(2)
        XCTAssertEqual(t.tray.kept, [true, true, false, false, true], "a refused tap changes no mark")
        t.cancelStaged()                                    // Messages' X
        XCTAssertEqual(t.tray.kept, [true, true, false, false, true], "the refused tap left no mark behind")
        XCTAssertEqual(t.tray.dice, roll1, "a cancelled KEEP rerolls nothing")
        t.roll()

        t.kernel.commit()                                   // the send echo: tb_api_mark_sent
        t.refresh(animate: true)
        XCTAssertTrue(t.tray.allKnown, "the reroll exists once the KEEP is sent")
        XCTAssertEqual(t.tray.roll, 2)
        XCTAssertEqual([t.tray.dice[0], t.tray.dice[1], t.tray.dice[4]], [roll1[0], roll1[1], roll1[4]],
                       "the kept dice held")
        XCTAssertEqual(t.player.rolled, 0b01100, "exactly the rerolled dice settle")
        XCTAssertTrue(t.player.animating)
    }

    func testTheKernelsSettleBlanksTumblesAndLandsOnTheView() throws {
        let t = try table()
        for i in [0, 1, 4] { t.toggle(i) }
        t.roll()
        t.kernel.commit()
        t.refresh(animate: true)
        let plan = try XCTUnwrap(t.player.plan, "the send laid out a plan")
        let settle = try XCTUnwrap(plan.beat.first { $0.kind == TB_BK_SETTLE }, "one settle")
        XCTAssertNil(t.player.frame(die: 0, ms: settle.startMs + 10), "a kept die never moves")
        XCTAssertEqual(t.player.frame(die: 2, ms: 0)?.face, 0, "before its start the die is blank, in the cup")
        let mid = try XCTUnwrap(t.player.frame(die: 2, ms: settle.startMs + 20), "die 2 in the air")
        XCTAssertNotEqual(mid.rotation, 0, "turning")
        XCTAssertLessThan(mid.dy, 0, "dropping from above the tray")
        let end = try XCTUnwrap(t.player.frame(die: 3, ms: plan.totalMs), "die 3 at the end")
        XCTAssertEqual(end.face, t.tray.dice[3], "it lands on the value the view shows")
        XCTAssertEqual(end.rotation, 0, "at rest")
    }
}

@MainActor
final class ScorecardTests: XCTestCase {

    func testRowsAreThirteenInTwoHalvesWithPreviewsOnlyWhereOpen() {
        var card = CardModel(seat: 0, name: "Alex")
        card.slots[Category.threes.rawValue] = 9
        card.slots[Category.fullHouse.rawValue] = 25
        let preview: [Int?] = [0, 0, 9, 0, 10, 0, 19, 0, 25, 0, 0, 0, 19]
        let rows = ScoreRow.rows(card: card, preview: preview, draft: .none, canScore: true, name: { "\($0)" })
        XCTAssertEqual(rows.count, 13)
        XCTAssertEqual(rows.filter { $0.category.isNumbers }.count, 6, "the numbers half is Ones to Sixes")
        XCTAssertEqual(rows.map(\.category), Category.allCases, "card order")
        XCTAssertEqual(rows[Category.threes.rawValue].state, .filled(9), "a filled row shows its score")
        XCTAssertFalse(rows[Category.threes.rawValue].tappable, "a filled row cannot be scored again")
        XCTAssertEqual(rows[Category.fives.rawValue].state, .open(preview: 10), "an open row shows the preview")
        XCTAssertTrue(rows[Category.fives.rawValue].tappable)
        XCTAssertEqual(rows[Category.fullHouse.rawValue].state, .filled(25), "the preview never covers a filled row")

        let theirs = ScoreRow.rows(card: card, preview: preview, draft: .none, canScore: false, name: { "\($0)" })
        XCTAssertEqual(theirs[Category.fives.rawValue].state, .open(preview: nil), "a read-only card shows no preview")
        XCTAssertFalse(theirs.contains { $0.tappable }, "a read-only card has no taps")
    }

    func testAScoreTapStagesTheScoreAndTheRowShowsItStaged() throws {
        try XCTUnwrap(Phones.dmStarted(), "a DM started")
        let t = TallyTable(kernel: BridgeKernel())
        let points = try XCTUnwrap(t.preview[Category.any.rawValue], "Any has a preview on roll 1")
        XCTAssertEqual(points, t.tray.dice.reduce(0, +), "the kernel's Any is the sum of the dice")
        var stages: [TallyStage] = []
        t.onStage = { stages.append($0) }
        t.score(.any)
        XCTAssertEqual(stages.count, 1, "a score tap stages")
        let card = try XCTUnwrap(t.myCard)
        let rows = ScoreRow.rows(card: card, preview: t.preview, draft: t.view.draft, canScore: t.tray.canScore,
                                 name: { "\($0)" })
        XCTAssertEqual(rows[Category.any.rawValue].state, .staged(points), "the staged row shows its score")
        t.kernel.commit()
        t.refresh(animate: true)
        XCTAssertEqual(t.myCard?.score(.any), points, "sent, the score stands")
        XCTAssertEqual(t.myCard?.total, points, "and the total with it")
        XCTAssertEqual(t.tray.turn, 1, "the turn passes to Bo")
        XCTAssertFalse(t.tray.canKeep || t.tray.canScore, "and my tray is Bo's to play")
    }
}

@MainActor
final class BubbleTests: XCTestCase {

    func testTheBubblePictureIsFoolishsSize() throws {
        let content = BubbleContent(dice: [3, 3, 0, 0, 3], kept: [true, true, false, false, true],
                                    caption: "Alex keeps 3, 3, 3 and rerolls two")
        let img = try XCTUnwrap(BubbleSnapshot.render(content, scheme: .light, scale: 2))
        XCTAssertEqual(img.size, CGSize(width: 300, height: 195), "300 x 195 points")
        XCTAssertEqual(img.cgImage?.width, 600, "at the scale asked for")
        XCTAssertEqual(img.cgImage?.height, 390)
        XCTAssertNotNil(BubbleSnapshot.render(BubbleContent(lobby: true, title: "Tallybones", roster: ["1. Alex"]),
                                              scheme: .dark, scale: 1), "a lobby bubble renders too")
    }

    func testAKeepBubbleCarriesTheBlanks() throws {
        try XCTUnwrap(Phones.dmStarted(), "a DM started")
        let k = BridgeKernel()
        let roll1 = k.view().tray.dice
        XCTAssertNotNil(k.roll(keeping: 0b10011))
        let c = BubbleContent.of(k.view(), title: "Tallybones")
        XCTAssertFalse(c.lobby)
        XCTAssertEqual(c.dice, [roll1[0], roll1[1], 0, 0, roll1[4]], "T11: the picture has blank slots for the rerolling dice")
        XCTAssertEqual(c.kept, [true, true, false, false, true])
    }
}

@MainActor
final class TwoPhoneTests: XCTestCase {

    func testTheReceiverSeesTheSendersRerollAndCard() throws {
        try XCTUnwrap(Phones.dmStarted(seed: 3), "a DM started")
        let alex = BridgeKernel()
        XCTAssertNotNil(alex.roll(keeping: 0b00001))
        alex.commit()
        let sent = alex.view()
        let keepLink = try XCTUnwrap(alex.text)
        XCTAssertTrue(sent.tray.allKnown && sent.tray.roll == 2, "Alex sees the reroll after the send")

        Phones.be(1)
        Tb.sender(of: keepLink, isDM: true, iSent: false)
        let bo = BridgeKernel()
        XCTAssertEqual(bo.adopt(keepLink, arrival: false), 0, "Bo opens Alex's KEEP bubble")
        XCTAssertEqual(bo.view().tray.dice, sent.tray.dice, "the receiver's dice are the sender's, by replay")
        XCTAssertEqual(bo.view().me, 1)

        Phones.be(0)
        Tb.sender(of: keepLink, isDM: true, iSent: true)
        XCTAssertEqual(alex.adopt(keepLink, arrival: false), 0)
        XCTAssertNotNil(alex.score(.fullHouse))
        alex.commit()
        let scoreLink = try XCTUnwrap(alex.text)
        let alexCard = try XCTUnwrap(alex.view().cards.first)

        Phones.be(1)
        Tb.sender(of: scoreLink, isDM: true, iSent: false)
        XCTAssertEqual(bo.adopt(scoreLink, arrival: true), 0)
        let v = bo.view()
        XCTAssertEqual(v.cards.first, alexCard, "Bo's copy of Alex's card is Alex's")
        XCTAssertEqual(v.tray.turn, 1, "and it is Bo's turn")
        XCTAssertTrue(v.tray.canKeep && v.tray.allKnown, "with Bo's roll 1 on the tray")
    }
}
