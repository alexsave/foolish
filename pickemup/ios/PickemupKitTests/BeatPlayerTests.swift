// BeatPlayerTests.swift - the Swift driver of the kernel's timeline: a plan
// becomes the right flights between the right anchors, a new plan replaces a
// playing one cleanly, and a range with nothing in it moves nothing. Every
// number asserted is the kernel's (PK_T_*), never typed here.
//
// Each test names the mutation it must go red on (MUTATE:), listed with the
// rest in pickemup/ios/TESTS_MUTATED.md.

import CPickemup
import XCTest
@testable import PickemupKit

@MainActor
final class BeatPlayerTests: XCTestCase {

    /// A board's anchors, as the views would report them: a deck, a pile, two
    /// fans, my hand's slots.
    private func anchors(hand n: Int) -> [String: CGRect] {
        var a: [String: CGRect] = [
            "deck": CGRect(x: 60, y: 300, width: 50, height: 70),
            "stack": CGRect(x: 120, y: 280, width: 82, height: 115),
            "fan.0": CGRect(x: 140, y: 40, width: 48, height: 44),
            "fan.1": CGRect(x: 140, y: 40, width: 48, height: 44),
            "hand": CGRect(x: 8, y: 600, width: 360, height: 80),
            "strip": CGRect(x: 2, y: 40, width: 128, height: 20),
        ]
        for i in 0..<n { a["hand.\(i)"] = CGRect(x: 12 + CGFloat(i) * 40, y: 604, width: 36, height: 72) }
        return a
    }

    /// The player's clock held at `ms` into whatever it plays next.
    private func at(_ m: TableModel, _ ms: Int) {
        let base: CFTimeInterval = 1000
        m.player.now = { base + Double(ms) / 1000 }
    }

    // MUTATE: BeatPlayer.anchorName maps PK_ANC_HAND to "hand" -> "the flight
    // lands in the new slot".
    // MUTATE: BeatPlayer.ghosts skips PK_BK_FLIP -> "then it turns over there".
    func testADrawIsOneFlightDeckToTheNewSlotThenAFlip() throws {
        XCTAssertNotNil(Phones.dmStartedByBo())
        let m = TableModel()
        let n = m.hand.count
        m.draw()
        let plan = try XCTUnwrap(m.player.plan, "a draw plays")
        let flights = plan.beat.filter { $0.kind == PK_BK_FLIGHT }
        XCTAssertEqual(flights.count, 1, "one flight per drawn card")
        XCTAssertEqual(flights.first?.from, PK_ANC_DECK)
        XCTAssertEqual(flights.first?.to, PK_ANC_HAND)
        XCTAssertEqual(flights.first?.toI, n, "the right end of my row")
        let a = anchors(hand: n + 1)
        let mid = PK_T_LEAD_LIVE + PK_T_DRAW / 2
        at(m, mid)
        let g = m.player.ghosts(mid, anchors: a)
        XCTAssertEqual(g.count, 1)
        XCTAssertEqual(g.first?.from, "deck")
        XCTAssertEqual(g.first?.to, "hand.\(n)", "the flight lands in the new slot")
        XCTAssertNil(g.first?.card, "a back while it flies")
        let deck = a["deck"]!, slot = a["hand.\(n)"]!
        let c = try XCTUnwrap(g.first?.center)
        XCTAssertTrue(c.x > deck.midX && c.x < slot.midX, "between the deck and the slot")
        // the slot is open and the card unseen until it has turned
        XCTAssertTrue(m.shown(m.player.frame(mid)).unseen.contains(n))
        let turning = PK_T_LEAD_LIVE + PK_T_DRAW + PK_T_DRAW_FLIP * 3 / 4
        let f = m.player.ghosts(turning, anchors: a)
        XCTAssertEqual(f.count, 1, "then it turns over there")
        XCTAssertEqual(f.first?.to, "hand.\(n)")
        XCTAssertEqual(f.first?.card, m.hand[n], "face up in its second half")
        let done = PK_T_LEAD_LIVE + PK_T_DRAW + PK_T_DRAW_FLIP
        XCTAssertTrue(m.player.ghosts(done, anchors: a).isEmpty)
        XCTAssertTrue(m.shown(m.player.frame(done)).unseen.isEmpty, "seen once it has turned")
    }

    /// Bo draws three and passes; Alex, looking at the start bubble, gets it.
    private func threeDrawsArrive() throws -> (PickemupHost, String) {
        let start = try XCTUnwrap(Phones.dmStartedByBo())
        _ = start
        let startLink = try XCTUnwrap(Pk.text, "the start bubble")
        for _ in 0..<3 { XCTAssertTrue(Pk.draw()) }
        XCTAssertTrue(Pk.pass())
        let bo = try XCTUnwrap(Pk.text, "Bo's bubble")
        Phones.be(0)
        XCTAssertEqual(Pk.read(startLink), 0, "Alex has the start bubble up")
        let host = PickemupHost()
        host.showResident()
        at(host.model, 0)                      // the plan begins at the held clock
        XCTAssertEqual(host.adopt(bo, arrival: true), 0)
        return (host, bo)
    }

    // MUTATE: BeatPlayer.rect lands a FAN flight on the fan's left edge
    // (r.minX) -> "at the fan's right end".
    // MUTATE: PickemupHost.adopt plays from bubble to - 1 even when the board
    // was already up -> "from the bubble on screen" (the arrival lead).
    func testAnArrivalFliesEachDrawnCardIntoTheSeatsFan() throws {
        let (host, _) = try threeDrawsArrive()
        let m = host.model
        let plan = try XCTUnwrap(m.player.plan, "an arrival plays")
        XCTAssertEqual(plan.mode, PK_BEATS_ARRIVAL, "from the bubble on screen")
        let draws = plan.beat.filter { $0.kind == PK_BK_FLIGHT && $0.evKind == PK_EV_DRAW }
        XCTAssertEqual(draws.count, 3, "one flight per card, never one carrying three")
        XCTAssertTrue(draws.allSatisfy { $0.to == PK_ANC_FAN && $0.toI == 1 && $0.card == PK_CARD_HIDDEN },
                      "backs into Bo's fan")
        XCTAssertEqual(draws.map(\.startMs), [0, 1, 2].map { PK_T_LEAD_LIVE + $0 * PK_T_DRAW_STEP },
                       "the kernel's stagger")
        let a = anchors(hand: m.hand.count)
        let t = PK_T_LEAD_LIVE + PK_T_DRAW_STEP + PK_T_DRAW_STEP / 2
        let g = m.player.ghosts(t, anchors: a)
        XCTAssertEqual(g.count, 2, "two in the air at once: they overlap and still count out")
        let fan = a["fan.1"]!
        XCTAssertTrue(g.allSatisfy { $0.to == "fan.1" })
        let last = PK_T_LEAD_LIVE + PK_T_DRAW - 1
        let landing = try XCTUnwrap(m.player.ghosts(last, anchors: a).first)
        XCTAssertEqual(landing.center.x, fan.maxX - PkLayout.fanCard.width / 2, accuracy: 2,
                       "at the fan's right end")
        // the pass: Bo's fan shrugs, then the turn bar moves to Alex's side
        let shrug = try XCTUnwrap(plan.beat.first { $0.kind == PK_BK_SHRUG })
        let fx = m.player.effects(shrug.startMs + shrug.durMs / 2, anchors: a)
        XCTAssertLessThan(fx["fan.1"]?.scale ?? 1, 1, "the passer's fan shrugs")
        let bar = try XCTUnwrap(plan.beat.first { $0.kind == PK_BK_TURN_BAR })
        let fb = m.player.effects(bar.startMs + bar.durMs / 2, anchors: a)
        XCTAssertNotNil(fb["bar.1"]?.bar, "the old bar fades out")
        XCTAssertEqual(m.shown(m.player.frame(bar.startMs - 1)).turn, 1, "Bo's turn until the bar moves")
        XCTAssertEqual(m.shown(m.player.frame(bar.startMs)).turn, 0, "then Alex's")
    }

    // MUTATE: BeatPlayer.play keeps the old plan when a new one arrives
    // (`guard self.plan == nil`) -> "the new plan is current".
    // MUTATE: BeatPlayer.play does not restart the clock -> "the new plan
    // starts from its own beginning".
    func testASupersedingPlanReplacesThePlayingOneCleanly() throws {
        let (host, _) = try threeDrawsArrive()
        let m = host.model
        let a = anchors(hand: m.hand.count)
        let first = try XCTUnwrap(m.player.plan)
        let t = PK_T_LEAD_LIVE + PK_T_DRAW_STEP + PK_T_DRAW_STEP / 2
        at(m, t)
        XCTAssertEqual(m.player.ghosts(m.player.ms(), anchors: a).count, 2, "mid-flight")
        // a newer chain lands on top of it: the kernel lays it out again from the
        // board it starts from, and the old flights are simply gone
        let second = try XCTUnwrap(Pk.beats(from: 0, to: 1, open: false))
        XCTAssertNotEqual(second.serial, first.serial)
        m.player.play(second)
        XCTAssertEqual(m.player.plan?.serial, second.serial, "the new plan is current")
        XCTAssertEqual(m.player.ms(), 0, "the new plan starts from its own beginning")
        XCTAssertTrue(m.player.ghosts(m.player.ms(), anchors: a).isEmpty, "none of the old flights survive")
        let f = m.shown(m.player.frame(0))
        XCTAssertEqual(f.deckN, second.start.deckN, "the board is the new plan's start, not a revert")
        // the old plan's end must not end the new one
        let gone = expectation(description: "the old plan's end has passed")
        DispatchQueue.main.asyncAfter(deadline: .now() + Double(first.totalMs) / 1000 + 0.1) { gone.fulfill() }
        m.player.now = { CACurrentMediaTime() }
        wait(for: [gone], timeout: 5)
        XCTAssertTrue(m.player.plan == nil || m.player.plan?.serial == second.serial,
                      "the superseded plan never comes back")
    }

    // MUTATE: pk_api_beats accepts from == to by laying out the newest bubble
    // (C) -> "no motion when nothing arrived".
    // MUTATE: PickemupHost.adopt plays whenever the game is the same, without
    // comparing the bubbles -> "adopting the same bubble again moves nothing".
    func testNoAnimationWhenFromEqualsTo() throws {
        let (host, bo) = try threeDrawsArrive()
        XCTAssertNotNil(host.model.player.plan)
        let same = Pk.beats(from: 1, to: 1, open: false)
        XCTAssertEqual(same?.beat.count ?? -1, 0, "no motion when nothing arrived")
        host.model.player.play(same)
        XCTAssertNil(host.model.player.plan, "an empty plan clears")
        XCTAssertEqual(host.adopt(bo, arrival: true), 0)
        XCTAssertNil(host.model.player.plan, "adopting the same bubble again moves nothing")
    }

    // MUTATE: BeatPlayer.ended drops a held plan -> "the staged draft keeps
    // its frame after the flight".
    // MUTATE: TableModel.sent plays nothing -> "Send plays what was held".
    func testAStagedPlayHoldsItsSettleUntilSend() throws {
        var found = false
        for k in 0..<50 {
            Phones.dmStartedByBo(seed: k)
            if Phones.plainPlayable() != nil { found = true; break }
        }
        XCTAssertTrue(found)
        let m = TableModel()
        let pos = try XCTUnwrap(Phones.plainPlayable())
        m.play(pos)
        let plan = try XCTUnwrap(m.player.plan)
        XCTAssertEqual(plan.mode, PK_BEATS_STAGE)
        XCTAssertGreaterThan(plan.held, 0, "the turn bar is held until Send")
        XCTAssertFalse(plan.beat.contains { $0.kind == PK_BK_TURN_BAR }, "and does not play at stage")
        let over = expectation(description: "the stage plan has run")
        DispatchQueue.main.asyncAfter(deadline: .now() + Double(plan.totalMs) / 1000 + 0.15) { over.fulfill() }
        wait(for: [over], timeout: 5)
        XCTAssertEqual(m.player.plan?.serial, plan.serial, "the staged draft keeps its frame after the flight")
        XCTAssertFalse(m.player.animating)
        XCTAssertEqual(m.shown(m.player.frame(m.player.ms())).turn, 1, "Bo's turn bar has not moved")
        XCTAssertTrue(Pk.commit())
        m.refresh()
        m.sent()
        let b = try XCTUnwrap(m.player.plan, "Send plays what was held")
        XCTAssertEqual(b.mode, PK_BEATS_SEND)
        XCTAssertTrue(b.beat.contains { $0.kind == PK_BK_TURN_BAR && $0.toI == 0 }, "the bar moves to Alex")
    }

    // MUTATE: TableModel.undo skips the host flight -> "the card flies home".
    func testUndoFliesThePlayedCardHome() throws {
        var found = false
        for k in 0..<50 {
            Phones.dmStartedByBo(seed: k)
            if Phones.plainPlayable() != nil { found = true; break }
        }
        XCTAssertTrue(found)
        let m = TableModel()
        let pos = try XCTUnwrap(Phones.plainPlayable())
        let card = m.hand[pos]
        m.play(pos)
        m.undo()
        let plan = try XCTUnwrap(m.player.plan, "the card flies home")
        let f = try XCTUnwrap(plan.beat.first { $0.kind == PK_BK_FLIGHT })
        XCTAssertEqual(f.from, PK_ANC_STACK)
        XCTAssertEqual(f.to, PK_ANC_HAND)
        XCTAssertEqual(f.toI, pos, "to the slot it came from")
        XCTAssertEqual(f.card, card)
        XCTAssertEqual(f.startMs, PK_T_LEAD_LIVE, "after the 16ms beat")
        XCTAssertEqual(f.durMs, PK_T_FLIGHT)
    }

    // MUTATE: TableModel.join never calls onDealt -> "the join that fills the
    // table plays the deal".
    func testTheJoinThatStartsTheGamePlaysTheDeal() throws {
        Phones.reset()
        Phones.be(0)
        XCTAssertTrue(Pk.newGame(dm: true, seed: Phones.seed(3)))
        let invite = try XCTUnwrap(Pk.text)
        Phones.be(1)
        Pk.sender(of: invite, isDM: true, iSent: false)
        let host = PickemupHost()
        XCTAssertEqual(host.adopt(invite, arrival: false), 0)
        XCTAssertEqual(host.screen, .lobby)
        XCTAssertNil(host.model.player.plan, "a lobby plays nothing here")
        host.model.join()
        XCTAssertEqual(host.screen, .table, "the table takes over")
        let plan = try XCTUnwrap(host.model.player.plan, "the join that fills the table plays the deal")
        XCTAssertEqual(plan.beat.first?.kind, PK_BK_HOLD, "the lobby rests first")
        XCTAssertEqual(plan.beat.first?.durMs, PK_T_LOBBY_REST)
        XCTAssertEqual(plan.beat.filter { $0.evKind == PK_EV_DEAL && $0.kind == PK_BK_FLIGHT }.count, 2 * PK_HAND_SIZE,
                       "one flight per dealt card")
        XCTAssertEqual(plan.beat.filter { $0.evKind == PK_EV_DEAL && $0.kind == PK_BK_FLIGHT && $0.to == PK_ANC_HAND }.count, PK_HAND_SIZE,
                       "seven into my hand")
    }

    // MUTATE: BeatPlayer.effects skips PK_BK_BAND -> "the band is part way up".
    // MUTATE: BandSlide ignores the fx (`let up: CGFloat = 1`) -> nothing here;
    // it is a view, and the planned red run for it is a screenshot (A12).
    func testAPickedWildSlidesItsBandUp() throws {
        let pos = try XCTUnwrap(Phones.dmWithWild())
        let m = TableModel()
        m.play(pos)
        XCTAssertEqual(m.pickerFor, pos)
        m.choose(2)
        let plan = try XCTUnwrap(m.player.plan, "the choice plays")
        let band = try XCTUnwrap(plan.beat.first { $0.kind == PK_BK_BAND }, "my wild's band is a beat")
        XCTAssertEqual(band.suit, 2, "in the chosen suit")
        XCTAssertEqual(band.durMs, PK_T_FADE)
        let a = anchors(hand: m.hand.count)
        let before = m.player.effects(band.startMs - 1, anchors: a)
        XCTAssertEqual(before["band"]?.band, 0, "hidden under the foot until it starts")
        let mid = m.player.effects(band.startMs + band.durMs / 2, anchors: a)
        let up = try XCTUnwrap(mid["band"]?.band, "the band is part way up")
        XCTAssertGreaterThan(up, 0.5, "ease-out: past halfway at half time")
        XCTAssertLessThan(up, 1)
        XCTAssertNil(m.player.effects(band.startMs + band.durMs + 1, anchors: a)["band"]?.band,
                     "in place once it has run")
    }

    // MUTATE: TableModel.join plays nothing for a join that does not start
    // the game -> "a join fades its row up".
    // MUTATE: BeatPlayer.effects drops the leave's `roster.gone` -> "the row
    // that left fades where it stood".
    // MUTATE: BeatPlayer.effects lets the rows close up before the HOLD
    // (`open` 0 while pending) -> "the rows below stand one lower".
    func testTheLobbyRowsFadeInAndOutOnTheKernelsBeats() throws {
        Phones.reset()
        Phones.be(0)
        XCTAssertTrue(Pk.newGame(dm: false, seed: Phones.seed(9)))
        let invite = try XCTUnwrap(Pk.text)
        Phones.be(1)
        XCTAssertEqual(Pk.read(invite), 0)
        let m = TableModel()
        m.join()
        let join = try XCTUnwrap(m.player.plan, "a join fades its row up")
        let fade = try XCTUnwrap(join.beat.first)
        XCTAssertEqual(fade.kind, PK_BK_FADE)
        XCTAssertEqual(fade.to, PK_ANC_ROW)
        XCTAssertEqual(fade.toI, 1, "Bo's row")
        XCTAssertEqual(m.player.effects(0, anchors: [:])["roster.1"]?.opacity, 0, "unseen before its fade")
        m.leave()
        let leave = try XCTUnwrap(m.player.plan, "a leave plays")
        let out = try XCTUnwrap(leave.beat.first { $0.kind == PK_BK_FADE })
        let hold = try XCTUnwrap(leave.beat.first { $0.kind == PK_BK_HOLD })
        let early = m.player.effects(0, anchors: [:])
        XCTAssertEqual(early["roster.gone"]?.gone, 1, "the row that left fades where it stood")
        XCTAssertEqual(early["roster.gone"]?.opacity, 1)
        XCTAssertEqual(early["roster.1"]?.close, 1, "the rows below stand one lower")
        let mid = m.player.effects(out.startMs + out.durMs / 2, anchors: [:])
        let o = try XCTUnwrap(mid["roster.gone"]?.opacity)
        XCTAssertGreaterThan(o, 0)
        XCTAssertLessThan(o, 1)
        let closing = m.player.effects(hold.startMs + hold.durMs / 2, anchors: [:])
        let c = try XCTUnwrap(closing["roster.1"]?.close)
        XCTAssertLessThan(c, 1, "then close up")
        XCTAssertEqual(m.player.effects(hold.startMs + hold.durMs, anchors: [:])["roster.1"]?.close, 0)
        XCTAssertFalse(Pk.words(PK_API_W_LOBBY_GONE, 1).isEmpty, "the gone row's words are the kernel's")
    }

    // MUTATE: pk_lay_picker's east reach 96 becomes 90 (C) -> "triangles east".
    func testThePickerTilesAreTheKernels() {
        let c = CGPoint(x: 200, y: 300)
        XCTAssertEqual(PkLayout.pickerTile(0, centre: c), CGPoint(x: 200, y: 196), "circles north")
        XCTAssertEqual(PkLayout.pickerTile(1, centre: c), CGPoint(x: 296, y: 300), "triangles east")
        XCTAssertEqual(PkLayout.pickerTile(2, centre: c), CGPoint(x: 200, y: 404), "squares south")
        XCTAssertEqual(PkLayout.pickerTile(3, centre: c), CGPoint(x: 104, y: 300), "diamonds west")
        XCTAssertEqual(PkLayout.pickerTile(4, centre: c), CGPoint(x: 296, y: 196), "the x")
    }
}
