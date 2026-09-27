// DiceTests.swift - the die's face, the tray's keep toggling and T11's blanks,
// the tumble, the scorecard's rows and the bubble picture. Each test's
// mutation, and the assertion it went red on, is in ../TESTS_MUTATED.md.

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

    private func table() -> (TallyTable, StandInKernel) {
        let k = StandInKernel(started: true)
        return (TallyTable(kernel: k, previewScores: StandInKernel.preview), k)
    }

    func testATapTogglesTheKeepMarkAndASecondTapClearsIt() {
        let (t, _) = table()
        XCTAssertTrue(t.tray.canKeep, "roll 1 of my turn may be kept")
        XCTAssertEqual(t.tray.kept, [false, false, false, false, false], "nothing kept at roll 1")
        t.toggle(2)
        XCTAssertEqual(t.tray.kept, [false, false, true, false, false], "a tap keeps die 2")
        t.toggle(0)
        t.toggle(2)
        XCTAssertEqual(t.tray.kept, [true, false, false, false, false], "a second tap on die 2 clears it")
        t.toggle(9)
        XCTAssertEqual(t.tray.kept, [true, false, false, false, false], "a die that is not there changes nothing")
    }

    func testAStagedKeepShowsBlanksUntilItIsSentThenTumblesIn() {
        let (t, k) = table()
        var stages: [TallyStage] = []
        t.onStage = { stages.append($0) }
        for i in [0, 1, 4] { t.toggle(i) }                 // keep the three threes
        t.roll()
        XCTAssertEqual(stages.count, 1, "Roll stages one KEEP")
        XCTAssertEqual(t.view.draft, .keep)
        XCTAssertEqual(t.tray.dice, [3, 3, 0, 0, 3], "T11: the rerolling dice are blank while staged")
        XCTAssertFalse(stages[0].caption.contains("5") || stages[0].caption.contains("2"),
                       "the KEEP's caption names no rerolled value")
        XCTAssertFalse(t.tray.canKeep, "no toggling under a staged KEEP")
        XCTAssertFalse(t.tray.canScore, "no scoring blank dice")
        t.toggle(2)
        XCTAssertEqual(t.tray.kept, [true, true, false, false, true], "a refused tap changes no mark")
        t.cancelStaged()                                    // Messages' X: the marks are as they were
        XCTAssertEqual(t.tray.kept, [true, true, false, false, true], "the refused tap left no mark behind")
        XCTAssertEqual(t.tray.dice, StandInKernel.h1, "a cancelled KEEP rerolls nothing")
        t.roll()

        k.commit()                                          // the send echo
        t.refresh()
        XCTAssertEqual(t.tray.dice, StandInKernel.h2, "the reroll exists once the KEEP is sent")
        XCTAssertEqual(t.tray.roll, 2)
        XCTAssertEqual(t.tumble.dice, [2, 3], "exactly the rerolled dice tumble")
        XCTAssertTrue(t.tumble.animating)
    }

    func testTheTumbleTurnsSwellsFlickersAndLandsAtRest() {
        let mid = TumblePlayer.sample(die: 0, ms: 180)
        XCTAssertGreaterThan(mid.scale, 1.1, "swollen near 30%")
        XCTAssertNotEqual(mid.rotation, 0, "turning")
        XCTAssertNotNil(mid.face, "the face flickers early on")
        XCTAssertNil(TumblePlayer.sample(die: 0, ms: 500).face, "the landed face shows from 70%")
        XCTAssertEqual(TumblePlayer.sample(die: 0, ms: TumblePlayer.durationMs), .rest, "at rest when done")
        XCTAssertEqual(TumblePlayer.sample(die: 4, ms: TumblePlayer.durationMs + 4 * TumblePlayer.staggerMs), .rest,
                       "the last die, staggered, is at rest when its own time is up")
        XCTAssertLessThan(TumblePlayer.sample(die: 1, ms: 180).rotation, 0, "alternate dice turn the other way")
    }
}

@MainActor
final class ScorecardTests: XCTestCase {

    func testRowsAreThirteenInTwoHalvesWithPreviewsOnlyWhereOpen() {
        var card = CardModel(seat: 0, name: "Alex")
        card.slots[Category.threes.rawValue] = 9
        card.slots[Category.fullHouse.rawValue] = 25
        let preview = StandInKernel.preview(StandInKernel.h2)
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

    func testAScoreTapStagesTheScoreAndTheRowShowsItStaged() {
        let k = StandInKernel(started: true)
        let t = TallyTable(kernel: k, previewScores: StandInKernel.preview)
        var stages: [TallyStage] = []
        t.onStage = { stages.append($0) }
        t.score(.threeAlike)
        XCTAssertEqual(stages.count, 1, "a score tap stages")
        let card = try! XCTUnwrap(t.myCard)
        let rows = ScoreRow.rows(card: card, preview: t.preview, draft: t.view.draft, canScore: t.tray.canScore,
                                 name: { "\($0)" })
        XCTAssertEqual(rows[Category.threeAlike.rawValue].state, .staged(16), "the staged row shows its score")
        k.commit()
        t.refresh()
        XCTAssertEqual(t.myCard?.score(.threeAlike), 16, "sent, the score stands")
        XCTAssertEqual(t.tray.roll, 1, "and my next turn's roll 1 is on the tray")
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

    func testAKeepBubbleCarriesTheBlanks() {
        let k = StandInKernel(started: true)
        _ = k.roll(keeping: 0b10011)
        let c = BubbleContent.of(k.view(), title: "Tallybones")
        XCTAssertFalse(c.lobby)
        XCTAssertEqual(c.dice, [3, 3, 0, 0, 3], "T11: the picture has blank slots for the rerolling dice")
        XCTAssertEqual(c.kept, [true, true, false, false, true])
    }
}
