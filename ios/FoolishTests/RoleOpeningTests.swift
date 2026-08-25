// RoleOpeningTests - ROUND 21: the board a bubble FOUND, and the mark that
// moves before the consequences do.
//
// The owner, replaying a round-ending good: "I don't see the sword to good
// transition. It started out already in GOOD, then did the discard animation and
// role switch animation. I think yes if we just played good, and we send it off,
// we shouldn't show the good animation as staging it should've already shown it.
// But if we close and open to REPLAY it, then for sure we should show our own
// good animation (rotation)."
//
// Two independent pieces, tested separately because they fail differently:
//
//   THE SEED - `MessageKernel.lastMoveEventsWithPrior` has to hand back the
//   table as it stood BEFORE the bubble's move. Seeding the role marks from the
//   stream's own first event (what round 16 did) is off by one, because an
//   event's `state` is the board AS OF that step - so a `good` was already in
//   the mask by the time the badges were first drawn and the flip had nowhere
//   to happen.
//
//   THE RULE - `MessageTableView.goodsOpening` decides whether a stream opens
//   with a role beat at all, and the asymmetry in it is the whole point: goods
//   ADDED play first, goods CLEARED play last.
import XCTest
@testable import FoolishKit

@MainActor
final class RoleOpeningTests: XCTestCase {

    private func freshSeed(_ salt: UInt8) -> Data {
        Data((0..<32).map { UInt8(truncatingIfNeeded: $0 &* 13 &+ Int(salt)) | 1 })
    }

    // MARK: - the rule

    /// A good being SET is somebody's move, so the stream opens on it.
    func testAGoodThisMoveAddedOpensTheStream() {
        let shown = MessageTableView.RoleState(defender: 1, firstAttacker: 0, goodMask: 0)
        let o = MessageTableView.goodsOpening(shown: shown, firstGoodMask: 0b100)
        XCTAssertEqual(o?.goodMask, 0b100, "seat 2's good is the beat this stream opens on")
        // Nothing changes hands in an opening beat, so nothing may fly.
        XCTAssertEqual(o?.defender, 1)
        XCTAssertEqual(o?.firstAttacker, 0)
    }

    /// A good being CLEARED is a consequence of the attack that reopened the
    /// bout. It belongs at the closing beat, with the discard and the hand-off -
    /// flip it early and an attacker's check would turn back into a sword before
    /// the card that cleared it had left anyone's hand.
    func testAGoodThisMoveClearedDoesNotOpenTheStream() {
        let all = MessageTableView.RoleState(defender: 1, firstAttacker: 0, goodMask: 0b100)
        XCTAssertNil(MessageTableView.goodsOpening(shown: all, firstGoodMask: 0),
                     "a cleared good is a consequence, not an opening move")
        // AND WITH SOMETHING LEFT BEHIND, which is the case that tells this rule
        // apart from the obvious wrong one. "Did the mask change at all" answers
        // yes here and would open the stream on a mask that only LOST a bit;
        // "which bits did it gain" answers none, which is the truth.
        let some = MessageTableView.RoleState(defender: 1, firstAttacker: 0, goodMask: 0b101)
        XCTAssertNil(MessageTableView.goodsOpening(shown: some, firstGoodMask: 0b001),
                     "one good clearing while another stands is still only a consequence")
    }

    /// One good arriving while another is taken away: only the arrival opens,
    /// and the one being cleared is left for the closing beat. Written as a
    /// separate case because the naive `!=` test passes this and the naive
    /// `= firstGoodMask` assignment gets it wrong in the other direction.
    func testOnlyTheAddedHalfOfAMixedChangeOpens() {
        let shown = MessageTableView.RoleState(defender: 1, firstAttacker: 0, goodMask: 0b001)
        let opening = MessageTableView.goodsOpening(shown: shown, firstGoodMask: 0b100)
        XCTAssertEqual(opening?.goodMask, 0b101,
                       "the arriving good is added; the departing one waits for the end")
    }

    /// THE SENDER'S BOARD HAS NOTHING TO PLAY. It flipped the mark when the move
    /// was staged, so by the time the settlement is released the shown mask and
    /// the stream's first state already agree - which is exactly the owner's "we
    /// shouldn't show the good animation as staging it should've already shown
    /// it", falling out of the rule rather than needing a flag for it.
    func testAMarkAlreadyShownPlaysNothing() {
        let shown = MessageTableView.RoleState(defender: 1, firstAttacker: 0, goodMask: 0b100)
        XCTAssertNil(MessageTableView.goodsOpening(shown: shown, firstGoodMask: 0b100))
    }

    /// Nothing to compare against is nothing to play: a board with no marks yet,
    /// or a stream whose first step carries no state.
    func testNothingToCompareAgainstPlaysNothing() {
        XCTAssertNil(MessageTableView.goodsOpening(shown: nil, firstGoodMask: 0b1))
        XCTAssertNil(MessageTableView.goodsOpening(
            shown: MessageTableView.RoleState(defender: 1, firstAttacker: 0), firstGoodMask: nil))
    }

    // MARK: - the seed

    /// THE PRIOR BOARD IS A REAL EARLIER BOARD, not the stream's own first frame.
    ///
    /// Played end to end against the kernel: attack, then cover. The cover's
    /// stream begins with the cover step, whose committed state ALREADY has the
    /// card on the table; the prior board is the state the attack left, which
    /// has it uncovered. Asserting on the battle rather than on the goodMask
    /// keeps the test honest about what "one step earlier" means for any move,
    /// not just the one that motivated it.
    func testThePriorBoardIsTheOneBeforeTheMoveNotTheOneAfterIt() async throws {
        let k = MessageKernel.shared
        try await k.newGame(seed: freshSeed(3), players: 2)
        let legal0 = await k.residentLegal(seat: 0)
        let attacker = legal0.contains { $0.type == .attack } ? 0 : 1
        let defender = 1 - attacker

        let atkMoves = await k.residentLegal(seat: attacker)
        let atk = try XCTUnwrap(atkMoves.first { $0.type == .attack })
        try await k.apply(seat: attacker, move: atk)
        let atomsAfterAttack = await k.stagedAtomsBefore() + 1

        let covMoves = await k.residentLegal(seat: defender)
        let cov = try XCTUnwrap(covMoves.first { $0.type == .cover })
        try await k.apply(seat: defender, move: cov)

        let opened = await k.lastMoveEventsWithPrior(viewer: defender,
                                                     atomsBefore: atomsAfterAttack)
        let prior = try XCTUnwrap(opened.prior, "a cover has an earlier step to report")
        let first = try XCTUnwrap(opened.events.first?.state)

        XCTAssertEqual(prior.battles.count, 1, "the attack had landed")
        XCTAssertNil(prior.battles.first?.defense,
                     "the prior board is BEFORE the cover - the attack is still bare")
        XCTAssertNotNil(first.battles.first?.defense,
                        "the stream's own first frame is already AFTER the cover, which is the "
                        + "off-by-one this call exists to fix")
    }

    /// A GOOD IS THE CASE THAT MOTIVATED IT, and it takes a REAL CHAIN to see:
    /// `atomsBefore` is a fact about a sealed bubble, and the v6 codec is not
    /// 1:1 with actions (it folds a bout's closing goods into one atom), so
    /// counting applies by hand gets the boundary wrong. Each leg here re-adopts
    /// its parent, plays one move and seals - exactly what a device does.
    ///
    /// Both kinds of good are checked, because they fail differently:
    ///
    ///   THE PLAIN GOOD emits no step at all, so its bubble's stream is EMPTY.
    ///   The difference between the two role states is the whole move, and a
    ///   prior board is the only thing that can carry it.
    ///
    ///   THE ROUND-ENDING GOOD opens onto its own consequences - the transition,
    ///   the sweep, the refills - every one of which already has the check in
    ///   its mask. That is the owner's "it started out already in GOOD".
    func testAGoodOpensFromABoardThatStillHasTheSwordUp() async throws {
        let k = MessageKernel.shared
        let n = 3
        var chain: Data?
        var legs: [(label: String, payload: Data, seat: Int)] = []

        /// Re-adopt the parent, play one move, seal. false when this deal does
        /// not offer the move, so the caller can try another.
        func leg(_ seat: Int, _ label: String, _ type: MoveType) async throws -> Bool {
            if let c = chain { _ = try await k.decode(payload: c, viewer: seat) }
            let moves = await k.residentLegal(seat: seat)
            guard let m = moves.first(where: { $0.type == type }) else { return false }
            try await k.apply(seat: seat, move: m)
            let p = try await k.seal(phase: 2, lastActorSeat: seat, gameId: 7,
                                     parent8: Data(repeating: 0, count: 8),
                                     joins: (0..<n).map { MessageJoin(seat: $0, name: "P\($0)") },
                                     sentAt: 1)
            chain = p
            legs.append((label, p, seat))
            return true
        }

        var built = false
        for salt: UInt8 in 1...40 {
            try await k.newGame(seed: freshSeed(salt), players: n)
            chain = nil; legs = []
            var opener = 0
            for s in 0..<n {
                let l = await k.residentLegal(seat: s)
                if l.contains(where: { $0.type == .attack }) { opener = s; break }
            }
            let defender = (opener + 1) % n, third = (opener + 2) % n
            guard try await leg(opener, "attack", .attack) else { continue }
            guard try await leg(defender, "cover", .cover) else { continue }
            guard try await leg(third, "plain good", .good) else { continue }
            guard try await leg(opener, "ending good", .good) else { continue }
            built = true
            // The two goods, checked against the boards their bubbles open from.
            let plain = legs[2], ending = legs[3]

            let plainEnv = try await k.decode(payload: plain.payload, viewer: plain.seat)
            let plainOpen = await k.lastMoveEventsWithPrior(viewer: plain.seat,
                                                           atomsBefore: plainEnv.atomsBefore)
            XCTAssertTrue(plainOpen.events.isEmpty,
                          "a good that does not close the bout emits no step - which is why "
                          + "the prior board has to survive an EMPTY stream")
            let plainPrior = try XCTUnwrap(plainOpen.prior,
                                           "an empty stream still has a board before it")
            XCTAssertFalse(plainPrior.hasSaidGood(third),
                           "the sword is still up on the board this bubble opens from")

            let endEnv = try await k.decode(payload: ending.payload, viewer: ending.seat)
            let endOpen = await k.lastMoveEventsWithPrior(viewer: ending.seat,
                                                          atomsBefore: endEnv.atomsBefore)
            let endPrior = try XCTUnwrap(endOpen.prior)
            let endFirst = try XCTUnwrap(endOpen.events.first?.state)
            XCTAssertFalse(endPrior.hasSaidGood(ending.seat),
                           "before the move the opener is still holding a sword")
            XCTAssertTrue(endFirst.hasSaidGood(ending.seat),
                          "the stream's own first frame already has the check on - the owner's "
                          + "'it started out already in GOOD'")

            // The two together are what makes a cold open play the flip…
            XCTAssertNotNil(
                MessageTableView.goodsOpening(shown: MessageTableView.RoleState(endPrior),
                                              firstGoodMask: endFirst.goodMask),
                "seeded from the prior board, a good replays as a move")
            // …and seeded the OLD way, from the stream's own first frame, there
            // is nothing left to animate. The bug, asserted as a bug.
            XCTAssertNil(
                MessageTableView.goodsOpening(shown: MessageTableView.RoleState(endFirst),
                                              firstGoodMask: endFirst.goodMask))
            break
        }
        XCTAssertTrue(built, "no deal in 40 gave attack/cover/good/good - rig problem, not a defect")
    }

    /// NO EARLIER STEP, NO GUESS. The first move on a fresh deal has nothing
    /// before it but the deal, so the prior board is reported as nil and the
    /// board falls back to seeding from the first frame - which is what every
    /// open did before this existed.
    func testTheFirstMoveOnAFreshDealReportsNoPriorBoard() async throws {
        let k = MessageKernel.shared
        try await k.newGame(seed: freshSeed(21), players: 2)
        let legal0 = await k.residentLegal(seat: 0)
        let attacker = legal0.contains { $0.type == .attack } ? 0 : 1
        let atkMoves = await k.residentLegal(seat: attacker)
        let atk = try XCTUnwrap(atkMoves.first { $0.type == .attack })
        try await k.apply(seat: attacker, move: atk)

        let opened = await k.lastMoveEventsWithPrior(viewer: attacker, atomsBefore: 0)
        XCTAssertFalse(opened.events.isEmpty, "the attack still animates")
        XCTAssertNil(opened.prior, "there is no step before the first move to report")

        // BELT AND BRACES, both pinned. Two independent things refuse this case -
        // the `atomsBefore >= 1` early-out, and the self-check on the step count -
        // and an assertion on the result alone cannot tell which one did the
        // work. So the second one is asserted directly: an `atomsBefore` of -1 is
        // the kernel's "no delta, guess it" path, and what it guesses is not one
        // step longer than the real stream, which is what the self-check rejects.
        let guessed = await k.lastMoveEvents(viewer: attacker, atomsBefore: -1)
        XCTAssertNotEqual(guessed.count, opened.events.count + 1,
                          "the guess path must not be mistakable for one step earlier")
    }

    /// The events are the SAME events either way. The prior board is an
    /// addition, not a re-cut of the stream: anything else would move the
    /// boundary every existing open-replay is built on.
    func testAskingForThePriorBoardDoesNotChangeTheStream() async throws {
        let k = MessageKernel.shared
        try await k.newGame(seed: freshSeed(31), players: 2)
        let legal0 = await k.residentLegal(seat: 0)
        let attacker = legal0.contains { $0.type == .attack } ? 0 : 1
        let defender = 1 - attacker
        let atkMoves = await k.residentLegal(seat: attacker)
        let atk = try XCTUnwrap(atkMoves.first { $0.type == .attack })
        try await k.apply(seat: attacker, move: atk)
        let atoms = await k.stagedAtomsBefore() + 1
        let pickMoves = await k.residentLegal(seat: defender)
        let pick = try XCTUnwrap(pickMoves.first { $0.type == .pickup })
        try await k.apply(seat: defender, move: pick)

        let plain = await k.lastMoveEvents(viewer: defender, atomsBefore: atoms)
        let opened = await k.lastMoveEventsWithPrior(viewer: defender, atomsBefore: atoms)
        XCTAssertEqual(opened.events, plain,
                       "the stream a bubble animates must not depend on whether its "
                       + "starting board was also asked for")
    }
}
