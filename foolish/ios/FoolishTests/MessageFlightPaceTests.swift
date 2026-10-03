// MessageFlightPaceTests - a sequence's flights run for the kernel plan's
// per-step duration, never a pacing constant of the board's own.
//
// The board used to fly every step for a fixed half second. That was right
// only while every step the kernel planned WAS half a second; the opening deal
// now goes round the table a card at a time, each card its own step at
// ANIM_DEAL_CARD_MS, and a board still flying 500ms per card would not be
// playing the plan it claims to be playing.
//
// So this pins the seam, over real kernel streams: the flight, the gap and the
// hold `runEventStream` uses for a beat (`MessageTableView.pace`) are the ones
// the kernel's plan laid out for that beat.
//
// MUTATION-CHECKED: `pace` answering a hardcoded 0.5 for the flight fails
// `testAnOpeningDealFliesAtThePlansPace` on every dealt card ("flies 0.5s, the
// plan says 350ms", and the next beat opening 525ms on instead of 375ms), while
// `testARefillFliesAtTheBeat` stays green - a refill really was 0.5s.
//
// THE SETTLE WAIT (`testTheSettleWaitOutlastsAnEightSeatDeal`,
// `testWaitForSettleWaitsOutTheAnnouncedPlan`), mutation-checked by putting
// `settleDeadline` back to the old flat `start + timeout`: both go red on
// their "gives up" assertion.
import XCTest
@testable import FoolishKit

@MainActor
final class MessageFlightPaceTests: XCTestCase {

    private func seed(_ salt: UInt8) -> Data {
        Data((0..<32).map { UInt8(truncatingIfNeeded: $0 &* 11 &+ Int(salt)) | 1 })
    }

    /// Every beat of `events` flies, waits and rests for what the plan says,
    /// and the next beat opens exactly where the plan's clock opens it.
    /// Answers the beats it checked, so a caller can pin the shapes it needs.
    private func assertPacedByThePlan(_ events: [GameEvent], finalView: GameView,
                                      _ label: String) -> [(AnimBeats.Beat, AnimPlan.Step)] {
        let beats = AnimBeats(events).beats
        let plan = AnimPlan(events, finalView: finalView)
        XCTAssertEqual(plan.steps.count, events.count, "\(label): the kernel planned every step")
        guard plan.steps.count == events.count else { return [] }
        var checked: [(AnimBeats.Beat, AnimPlan.Step)] = []
        for (i, beat) in beats.enumerated() {
            let step = plan.steps[beat.range.lowerBound]
            let pace = MessageTableView.pace(of: beat, in: plan)
            let kind = beat.kind.map(String.init(describing:)) ?? "?"
            XCTAssertEqual(pace.flight, Double(step.durationMs) / 1000, accuracy: 1e-9,
                           "\(label): beat \(i) (\(kind)) flies \(pace.flight)s, "
                           + "the plan says \(step.durationMs)ms")
            XCTAssertEqual(pace.hold, Double(step.holdMs) / 1000, accuracy: 1e-9,
                           "\(label): beat \(i) (\(kind)) rests \(pace.hold)s, "
                           + "the plan says \(step.holdMs)ms")
            // A beat that flies nothing takes no slot of the clock and no gap
            // (the board plays no flight for it); every other beat hands over
            // after its flight, the gap and its hold.
            if step.durationMs > 0, i + 1 < beats.count {
                let next = plan.steps[beats[i + 1].range.lowerBound]
                XCTAssertEqual(pace.flight + flightGap + pace.hold,
                               Double(next.startMs - step.startMs) / 1000, accuracy: 1e-9,
                               "\(label): beat \(i + 1) opens off the board's clock, not the plan's")
            }
            checked.append((beat, step))
        }
        return checked
    }

    /// THE OPENING DEAL: a card per step, at the deal's own pace. The fixture
    /// asserts the deal really is paced apart from a refill - a kernel pacing
    /// both at one beat would let a board flying a constant pass this.
    func testAnOpeningDealFliesAtThePlansPace() async throws {
        // The opening deal's shape - a card per event, round the table - and a
        // refill after it, planned by the kernel against a real dealt board.
        // The replay of a fresh chain carries no deal to open on, so the
        // stream is written here; the TIMING is entirely the kernel's.
        let k = MessageKernel.shared
        try await k.newGame(seed: seed(3), players: 2)
        let resident = await k.residentView(viewer: 0)
        let view = try XCTUnwrap(resident, "no board after the deal")
        func ev(_ kind: EventType, seat: Int, _ n: Int) -> GameEvent {
            GameEvent(type: kind.rawValue, seat: seat, msg: 0, from: 1, to: 2,
                      cards: Array(repeating: nil, count: n), target: nil, battle: nil, state: nil)
        }
        let events = (0..<12).map { ev(.deal, seat: $0 % 2, 1) } + [ev(.refill, seat: 0, 2)]
        let checked = assertPacedByThePlan(events, finalView: view, "2p opening deal")
        let deals = checked.filter { $0.0.kind == .deal }
        XCTAssertGreaterThanOrEqual(deals.count, 12, "fixture: a 2p deal is twelve cards, one beat each "
            + "(stream: \(events.map { $0.kind.map(String.init(describing:)) ?? "?" }))")
        for (_, step) in deals {
            XCTAssertLessThan(step.durationMs, ANIM_TIME_MS,
                              "fixture: the kernel paces a dealt card at the refill's beat")
        }
    }

    /// A STEP TAKES THE PLAN'S TIME, NOT THE PLAN'S TIME PLUS A PAINT.
    /// `BoardAnimator.play` paints a step at its from-position before flying
    /// it; that paint used to be 25ms on top of the flight and the gap, so the
    /// opening deal went round at ~415ms a card on the rig against the kernel's
    /// 375. Mutation: `flight.paintgap` off (the old waits) fails the sum.
    func testAStepWaitsExactlyTheFlightAndTheGap() {
        XCTAssertTrue(FlightPace.paintIsGapByDefault, "the plan's own pace is what ships")
        XCTAssertEqual(FlightPace.paintIsGap, FlightPace.paintIsGapByDefault, "no dev.flags override is in play")
        for ms in [Int(ANIM_DEAL_CARD_MS), Int(ANIM_TIME_MS)] {
            let duration = Double(ms) / 1000
            let w = FlightPace.waits(flying: duration)
            XCTAssertGreaterThan(w.paint, 0, "\(ms)ms: the from-position still gets its paint")
            XCTAssertEqual(w.paint + w.after, duration + flightGap, accuracy: 1e-9,
                           "\(ms)ms: a step waits \(w.paint + w.after)s, the plan gives it \(duration + flightGap)s")
        }
    }

    /// THE SETTLE WAIT OUTLASTS THE DEAL. The extension awaits
    /// `BoardAnimator.waitForSettle` before staging a bubble, and that wait
    /// used to give up a flat 8 s after it began. An 8-seat opening deal's
    /// kernel plan runs past that, so the bubble would have been staged with
    /// cards still being dealt. The bound now starts counting where the plan
    /// the sequence announced ends.
    func testTheSettleWaitOutlastsAnEightSeatDeal() async throws {
        let k = MessageKernel.shared
        try await k.newGame(seed: seed(7), players: 8)
        let resident = await k.residentView(viewer: 0)
        let view = try XCTUnwrap(resident, "no board after the 8-seat deal")
        func ev(_ seat: Int) -> GameEvent {
            GameEvent(type: EventType.deal.rawValue, seat: seat, msg: 0, from: 1, to: 2,
                      cards: [nil], target: nil, battle: nil, state: nil)
        }
        let events = (0..<48).map { ev($0 % 8) }
        let plan = AnimPlan(events, finalView: view)
        XCTAssertEqual(plan.steps.count, 48, "the kernel planned all 48 dealt cards")
        let total = boardSeconds(plan.totalMs)
        XCTAssertGreaterThan(total, 8.0,
                             "fixture: an 8-seat deal's plan (\(plan.totalMs)ms) outlasts the old flat 8 s")

        let hold = BoardAnimator.holdSequence()
        defer { hold.release() }
        let start = Date()
        hold.expect(total)
        let deadline = BoardAnimator.settleDeadline(from: start, timeout: 8.0)
        XCTAssertGreaterThanOrEqual(deadline.timeIntervalSince(start), total,
                                    "waitForSettle gives up \(deadline.timeIntervalSince(start))s in, "
                                    + "before the \(plan.totalMs)ms deal it is waiting on has landed")
    }

    /// The same rule on the real wait, at test-sized numbers: a sequence
    /// announcing a 0.4 s plan holds `waitForSettle` past a 0.05 s timeout,
    /// and the wait still returns as soon as the hold is given back.
    func testWaitForSettleWaitsOutTheAnnouncedPlan() async {
        let hold = BoardAnimator.holdSequence()
        hold.expect(0.4)
        let start = Date()
        Task { @MainActor in
            try? await Task.sleep(nanoseconds: 300_000_000)
            hold.release()
        }
        await BoardAnimator.waitForSettle(pollInterval: 10_000_000, timeout: 0.05)
        let waited = Date().timeIntervalSince(start)
        XCTAssertGreaterThanOrEqual(waited, 0.29, "the wait gave up \(waited)s in, inside the plan")
        XCTAssertLessThan(waited, 1.0, "the wait outlived the hold (\(waited)s)")
        XCTAssertNil(BoardAnimator.plannedSettle, "the last release clears the planned landing")
    }

    /// A REFILL is unchanged: the plan paces it at the kernel beat, so the
    /// board flies it for exactly what it flew before the constant went.
    func testARefillFliesAtTheBeat() async throws {
        let k = MessageKernel.shared
        try await k.newGame(seed: seed(5), players: 2)
        let legal0 = await k.residentLegal(seat: 0)
        let attacker = legal0.contains(where: { $0.type == .attack }) ? 0 : 1
        let atkLegal = await k.residentLegal(seat: attacker)
        let atk = try XCTUnwrap(atkLegal.first { $0.type == .attack })
        try await k.apply(seat: attacker, move: atk)
        let defLegal = await k.residentLegal(seat: 1 - attacker)
        let pick = try XCTUnwrap(defLegal.first { $0.type == .pickup })
        try await k.apply(seat: 1 - attacker, move: pick)
        let events = await k.lastMoveEvents(viewer: attacker)
        let resident = await k.residentView(viewer: attacker)
        let view = try XCTUnwrap(resident, "no board after the pickup")
        let checked = assertPacedByThePlan(events, finalView: view, "2p pickup")
        let refills = checked.filter { $0.0.kind == .refill }
        XCTAssertFalse(refills.isEmpty, "fixture: the pickup refills the attacker")
        for (beat, step) in refills {
            XCTAssertEqual(step.durationMs, ANIM_TIME_MS, "a refill is one kernel beat")
            XCTAssertEqual(MessageTableView.pace(of: beat, in: AnimPlan(events, finalView: view)).flight,
                           0.5, accuracy: 1e-9, "a refill flies what it always flew")
        }
    }
}
