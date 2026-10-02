// LiveArrival4pTests - live arrivals at THREE AND FOUR seats, where a good can
// still be pending when the next bubble lands.
//
// The owner's notes from a 4p session (2026-10-01), in his words:
//   2  "Live arrival good take a while to show up?"
//   3  "Live arrival did not see bout end ? Had to close and reopen"
//   4  "Sending good live arrival does not work"
//   5  "4 p game especially"
//   8  "Bout ending replay didn't play. Only plays for the person who sends
//       last good"
//   9  "A live arrival good, even a bout ending one, does not animate anything
//       in a 4p game"
//
// Every live-arrival suite before this one was 2p, and at two seats the shape
// these notes share cannot be posed: the one attacker's good over a covered
// table closes the bout at once, so no bubble ever carries a PENDING good.
// See LiveArrivalFixture for what is built and how faithful it is.
//
// THE MECHANISM (H1), now fixed. A board that had adopted a chain remembered
// how far it had animated as that chain's atom count and never replayed behind
// it (`MessageTurnController.rebuildBase` passed the previous chain's turn as a
// floor). But a pending good is an atom only while nothing follows it
// (c/src/replay.c log_atom_kind), so the bubble AFTER one or more pending goods
// re-encodes them away: two pending goods (turn N+2) followed by the closing
// good fold into ONE round_end atom (turn N+1). The floor N+2 was then past the
// end of the arriving stream and the kernel correctly answered "nothing to
// animate" for the move it was asked to skip to. No discard, no refill, no
// roles - on every receiver, while the sender played it from its own released
// settlement. A cold open had no floor, which is why closing and reopening the
// bubble showed it.
//
// THE FIX hands the kernel the chain the board was showing instead of a number
// (MessageKernel.OpenFrom.shown, c/src/msg_wire.h msg_open_boundary): the
// replay starts at the sender's claim, never behind the atoms the two chains
// share, and the shared prefix stops at the first pending good. The tests below
// pin it at 2, 3 and 4 seats for both shapes - the bout-closing good and a move
// after a pending good - and `testTheFlagsOldBranchIsStillTheOldFloor` pins
// that the `arrival.openboundary` dev flag's old branch still reproduces H1.

import XCTest
@testable import FoolishKit

@MainActor
final class LiveArrival4pTests: XCTestCase {

    // MARK: - the fixture's own facts

    /// The codec facts every reproduction below stands on, checked rather than
    /// assumed: at four seats two pending goods are two atoms, and the closing
    /// good folds all three into ONE round_end atom - so the chain's turn goes
    /// DOWN from the bubble before it. And the bout-end stream is really there:
    /// a cold open of the closing bubble animates the discard.
    func testFourSeatGoodRunFoldsAndAColdOpenStillAnimatesIt() async throws {
        let f = try await LiveArrivalFixture.coveredTable(players: 4)
        XCTAssertEqual(f.attackers.count, 3)
        let (a, b, c) = (f.attackers[0], f.attackers[1], f.attackers[2])
        let x1 = try await f.good(a, after: f.root)
        let x2 = try await f.good(b, after: x1)
        let x3 = try await f.good(c, after: x2)

        XCTAssertEqual(x1.turn, f.root.turn + 1, "a pending good is an atom")
        XCTAssertEqual(x2.turn, f.root.turn + 2, "…and so are two")
        XCTAssertEqual(x3.turn, f.root.turn + 1,
                       "the closing good folds the run into ONE round_end atom - the turn goes DOWN")
        XCTAssertEqual(x3.env.round, x2.env.round + 1, "…and the round moves on")
        let after = try await f.truth(f.defender, on: x3)
        XCTAssertEqual(after?.battles.isEmpty, true, "the bout closed")

        // Rule P adopts the closing good over the bubble before it - this is
        // not a refusal problem (rule 4's direct-child link, and the round).
        let pref = try await f.preferred(showing: x2, arriving: x3)
        XCTAssertGreaterThan(pref, 0, "the surface would adopt the closing bubble")

        // THE CONTROL: a cold open of the closing bubble animates its bout end.
        let cold = await f.controller(seat: f.defender, on: x3, watching: false)
        XCTAssertTrue(cold.openReplayEvents.sweepsTheTable,
                      "cold open of the closing good: [\(cold.openReplayEvents.kindNames)]")
    }

    // MARK: - H1: the bout end and the move after a pending good animate (notes 3, 8, 9)

    /// NOTE 9 (and 3, 8). Four seats; two attackers have said good; the board
    /// is open on that bubble; the third attacker's good arrives and closes the
    /// bout. The receiving board must animate the bout end - the discard sweep,
    /// the refills, the role hand-off - exactly as the sender did and exactly
    /// as a cold open of the same bubble does (the control above).
    ///
    /// Measured before the fix: ZERO events, for every receiver, because the
    /// floor (x2's turn, N+2) was past the end of x3's stream (N+1).
    func testABoutClosingGoodArrivingOverTwoPendingGoodsAnimatesTheBoutEnd() async throws {
        let f = try await LiveArrivalFixture.coveredTable(players: 4)
        let (a, b, c) = (f.attackers[0], f.attackers[1], f.attackers[2])
        let x1 = try await f.good(a, after: f.root)
        let x2 = try await f.good(b, after: x1)
        let x3 = try await f.good(c, after: x2)

        // The defender, an attacker who already said good, and one who did too.
        for watcher in [f.defender, a, b] {
            let board = await f.controller(seat: watcher, on: x2)
            await f.arrive(x3, at: board)
            XCTAssertEqual(board.basePayload, x3.payload, "seat \(watcher): adopted the closing good")
            XCTAssertEqual(board.view?.battles.isEmpty, true, "seat \(watcher): the table is clear")
            let evs = board.openReplayEvents
            XCTAssertTrue(evs.sweepsTheTable,
                          "seat \(watcher): the arrival that closed the bout animated "
                          + "[\(evs.kindNames)] - no sweep to the discard")
            board.setBoardWatching(false)
        }
    }

    /// NOTE 8 at THREE seats: one pending good was enough. x1 (attacker A good,
    /// turn N+1) then x2 (attacker B good, closes, ONE round_end atom: turn
    /// N+1) - the old floor N+1 equalled the arriving turn and the stream was
    /// empty.
    func testThreeSeatsTheClosingGoodOverOnePendingGoodAnimatesTheBoutEnd() async throws {
        let f = try await LiveArrivalFixture.coveredTable(players: 3)
        let (a, b) = (f.attackers[0], f.attackers[1])
        let x1 = try await f.good(a, after: f.root)
        let x2 = try await f.good(b, after: x1)
        XCTAssertEqual(x2.turn, x1.turn, "the fold: one pending good + the closing good = one atom")

        let board = await f.controller(seat: f.defender, on: x1)
        await f.arrive(x2, at: board)
        XCTAssertEqual(board.basePayload, x2.payload)
        let evs = board.openReplayEvents
        XCTAssertTrue(evs.sweepsTheTable,
                      "the arrival that closed the bout animated [\(evs.kindNames)]")
        board.setBoardWatching(false)
    }

    /// NOT ONLY GOODS. Any move that follows a pending good supersedes it, so
    /// the same floor ate an ordinary THROW-IN arriving after a good: the new
    /// attack card appeared on the table with no flight. (Same mechanism; the
    /// owner's notes name goods because a good-run is where it is commonest.)
    /// Pinned at four seats and at three.
    func testAThrowInArrivingOverAPendingGoodFliesItsCardAtFourSeats() async throws {
        try await throwInOverAPendingGoodFlies(players: 4)
    }

    func testAThrowInArrivingOverAPendingGoodFliesItsCardAtThreeSeats() async throws {
        try await throwInOverAPendingGoodFlies(players: 3)
    }

    private func throwInOverAPendingGoodFlies(players: Int) async throws {
        // A root where some attacker other than the first can still throw in.
        var thrower = -1
        let f = try await LiveArrivalFixture.coveredTable(players: players) { f in
            for s in f.attackers.dropFirst() {
                if try await f.legal(s, on: f.root).contains(where: { $0.type == .attack }) {
                    thrower = s; return true
                }
            }
            return false
        }
        let a = f.attackers[0]
        let x1 = try await f.good(a, after: f.root)
        let menu1 = try await f.legal(thrower, on: x1)
        let attack = try XCTUnwrap(menu1.first { $0.type == .attack })
        let x2 = try await f.play(thrower, attack, after: x1)
        XCTAssertEqual(x2.turn, x1.turn, "\(players)p: the throw-in supersedes the good: same atom count")
        let pref = try await f.preferred(showing: x1, arriving: x2)
        XCTAssertGreaterThan(pref, 0, "\(players)p: rule 4: the direct child is adopted")

        let board = await f.controller(seat: f.defender, on: x1)
        await f.arrive(x2, at: board)
        XCTAssertEqual(board.basePayload, x2.payload)
        let before = try await f.truth(f.defender, on: x1)
        XCTAssertEqual(board.view?.battles.count, before.map { $0.battles.count + 1 },
                       "\(players)p: the new attack is on the table")
        let evs = board.openReplayEvents
        XCTAssertTrue(evs.has(.attackPass),
                      "\(players)p: the arriving throw-in animated [\(evs.kindNames)] - no card flight")
        board.setBoardWatching(false)
    }

    /// AND ITS DEFENDER'S ANSWER: after the throw-in, the defender covers it.
    /// The cover arrives on a board that showed the throw-in (no good pending
    /// any more), so this is the plain case - pinned so the fix cannot have
    /// bought the pending-good shapes at the price of the ordinary one.
    func testACoverArrivingAfterTheThrowInFliesAtFourSeats() async throws {
        var thrower = -1
        let f = try await LiveArrivalFixture.coveredTable(players: 4) { f in
            for s in f.attackers.dropFirst() {
                if try await f.legal(s, on: f.root).contains(where: { $0.type == .attack }) {
                    thrower = s; return true
                }
            }
            return false
        }
        let x1 = try await f.good(f.attackers[0], after: f.root)
        let throwMenu = try await f.legal(thrower, on: x1)
        let attack = try XCTUnwrap(throwMenu.first { $0.type == .attack })
        let x2 = try await f.play(thrower, attack, after: x1)
        guard let cover = try await f.legal(f.defender, on: x2).first(where: { $0.type == .cover }) else {
            throw XCTSkip("the defender cannot cover this throw-in")
        }
        let x3 = try await f.play(f.defender, cover, after: x2)
        let board = await f.controller(seat: thrower, on: x2)
        await f.arrive(x3, at: board)
        XCTAssertEqual(board.basePayload, x3.payload)
        XCTAssertEqual(board.openReplayEvents.filter { $0.kind == .cover }.count, 1,
                       "the cover animated [\(board.openReplayEvents.kindNames)]")
        board.setBoardWatching(false)
    }

    /// THE 2P CONTROL, so the reproductions above cannot be read as "every
    /// arrival is broken": at two seats the closing good has no pending good in
    /// front of it, and the receiver animates the bout end.
    func testTwoSeatsTheClosingGoodStillAnimates() async throws {
        let f = try await LiveArrivalFixture.coveredTable(players: 2)
        let x1 = try await f.good(f.attackers[0], after: f.root)
        let board = await f.controller(seat: f.defender, on: f.root)
        await f.arrive(x1, at: board)
        XCTAssertEqual(board.basePayload, x1.payload)
        XCTAssertTrue(board.openReplayEvents.sweepsTheTable,
                      "2p closing good animated [\(board.openReplayEvents.kindNames)]")
        board.setBoardWatching(false)
    }

    /// THE 2P SIDE OF "A MOVE AFTER A GOOD": at two seats there is no pending
    /// good to follow (the good above closes the bout at once), so the shape is
    /// the next bout's opening attack arriving on a board that showed the
    /// closing good. Its card must fly.
    func testTwoSeatsTheAttackAfterTheClosingGoodFlies() async throws {
        let f = try await LiveArrivalFixture.coveredTable(players: 2)
        let x1 = try await f.good(f.attackers[0], after: f.root)
        var opener = -1
        var opening: Move?
        for s in 0..<2 where opening == nil {
            opening = try await f.legal(s, on: x1).first(where: { $0.type == .attack })
            if opening != nil { opener = s }
        }
        guard let attack = opening else { throw XCTSkip("nobody may open the next bout") }
        let x2 = try await f.play(opener, attack, after: x1)
        let board = await f.controller(seat: 1 - opener, on: x1)
        await f.arrive(x2, at: board)
        XCTAssertEqual(board.basePayload, x2.payload)
        XCTAssertTrue(board.openReplayEvents.has(.attackPass),
                      "2p attack after the bout end animated [\(board.openReplayEvents.kindNames)]")
        board.setBoardWatching(false)
    }

    /// THE FLAG'S OLD BRANCH IS THE OLD FLOOR. `arrival.openboundary=0` must
    /// put back exactly what shipped before, so a dev build can still show the
    /// defect side by side: the 4p closing good opened with the previous
    /// chain's turn as the floor animates nothing, and with the shown chain it
    /// sweeps the table.
    func testTheFlagsOldBranchIsStillTheOldFloor() async throws {
        let f = try await LiveArrivalFixture.coveredTable(players: 4)
        let (a, b, c) = (f.attackers[0], f.attackers[1], f.attackers[2])
        let x1 = try await f.good(a, after: f.root)
        let x2 = try await f.good(b, after: x1)
        let x3 = try await f.good(c, after: x2)
        let k = MessageKernel.shared
        let old = try await k.openChain(payload: x3.payload, viewer: f.defender,
                                        from: .legacyFloor(x2.turn))
        XCTAssertTrue(old.events.isEmpty, "the old floor: [\(old.events.kindNames)]")
        let new = try await k.openChain(payload: x3.payload, viewer: f.defender,
                                        from: .shown(x2.payload))
        XCTAssertTrue(new.events.sweepsTheTable, "the shown chain: [\(new.events.kindNames)]")
        XCTAssertTrue(MessageTurnController.opensFromShownChainByDefault,
                      "the fix ships on")
    }

    // MARK: - H2: a non-closing good (note 2)

    /// NOTE 2, the CONTROLLER half. A good that does not close the bout emits
    /// no step at all, so the arrival's stream is legitimately EMPTY - the only
    /// thing a board can be told about it is the role change, and the
    /// controller does hand that over: the view carries the new good bit and
    /// the prior board does not. What the BOARD does with an empty arrival
    /// (MessageTableView+OpenReplay returns before any role sync) is the
    /// harness's to show - see HarnessScenario `arrival` kind `goodmid`.
    func testANonClosingGoodArrivesAsAnEmptyStreamWithTheRoleChangeInTheView() async throws {
        let f = try await LiveArrivalFixture.coveredTable(players: 4)
        let (a, b) = (f.attackers[0], f.attackers[1])
        let x1 = try await f.good(a, after: f.root)
        let x2 = try await f.good(b, after: x1)

        let board = await f.controller(seat: f.defender, on: x1)
        await f.arrive(x2, at: board)
        XCTAssertEqual(board.basePayload, x2.payload)
        XCTAssertTrue(board.openReplayEvents.isEmpty,
                      "a non-closing good has no step: [\(board.openReplayEvents.kindNames)]")
        XCTAssertEqual(board.view.map { $0.hasSaidGood(b) }, true, "the view carries B's good")
        XCTAssertEqual(board.view.map { $0.hasSaidGood(a) }, true, "…and A's")
        // The board before B's good, for the sword -> check flip.
        let prior = try XCTUnwrap(board.openReplayPriorState,
                                  "an empty arrival still needs the board it found")
        XCTAssertFalse(prior.hasSaidGood(b), "the prior board has B still holding a sword")
        board.setBoardWatching(false)
    }

    // MARK: - the 4p sibling race (notes 4, 5)

    /// NOTE 4/5 CANDIDATE - "Sending good live arrival does not work", "4 p
    /// game especially". Two attackers say good off the SAME bubble at once,
    /// which only a 3+ seat table can do. Their bubbles are siblings at the
    /// same round and turn, so Rule P falls through to the digest: one of them
    /// is the game and the other's good is simply gone. The loser's board
    /// adopts the winner (as designed - Rule P) and its own check disappears:
    /// its sword is back and it must say good again, with nothing to say why
    /// (Rule N, docs/IMESSAGE_SUPERSEDED_MOVES.md, is design-only).
    ///
    /// A CHARACTERISATION, not an expected failure: what is pinned is today's
    /// behaviour, so the Rule N work that adds the notice starts from a red
    /// line it can see move.
    func testTwoGoodsOffOneBubbleLoseOneSilently() async throws {
        let f = try await LiveArrivalFixture.coveredTable(players: 4)
        let (a, b, c) = (f.attackers[0], f.attackers[1], f.attackers[2])
        let x1 = try await f.good(a, after: f.root)

        // B and C each stage good on x1 and seal it, as their phones do.
        var sealed: [Int: Data] = [:]
        var boards: [Int: MessageTurnController] = [:]
        for s in [b, c] {
            let board = await f.controller(seat: s, on: x1)
            let good = try XCTUnwrap(board.legal.first { $0.type == .good })
            let applied = await board.apply(good)
            XCTAssertTrue(applied)
            sealed[s] = try await board.stagedPayload()
            boards[s] = board
        }
        let sb = try XCTUnwrap(sealed[b]), sc = try XCTUnwrap(sealed[c])
        let ebB = try await MessageEnvelope.decode(payload: sb, viewer: -1)
        let ebC = try await MessageEnvelope.decode(payload: sc, viewer: -1)
        XCTAssertEqual(ebB.turn, ebC.turn, "siblings: same turn")
        XCTAssertEqual(ebB.round, ebC.round, "…same round")
        XCTAssertEqual(ebB.parent8, ebC.parent8, "…same parent")

        let order = try await MessageKernel.shared.preferred(sb, sc)
        XCTAssertNotEqual(order, 0)
        let (winner, loser) = order < 0 ? (b, c) : (c, b)
        let winBytes = winner == b ? sb : sc, loseBytes = loser == b ? sb : sc
        let winEnv = winner == b ? ebB : ebC

        // Both press Send. The loser's board rebases onto its own bubble.
        let loserBoard = try XCTUnwrap(boards[loser])
        await loserBoard.markSent(payload: loseBytes)
        XCTAssertEqual(loserBoard.basePayload, loseBytes)
        XCTAssertEqual(loserBoard.view.map { $0.hasSaidGood(loser) }, true, "my check is up")

        // The winner arrives. The surface asks Rule P first and adopts it.
        let surfaceAdopts = try await MessageKernel.shared.preferred(loseBytes, winBytes) > 0
        XCTAssertTrue(surfaceAdopts, "Rule P hands the loser's board the winning sibling")
        await loserBoard.offerArrival(payload: winBytes, parent: winEnv)
        XCTAssertEqual(loserBoard.basePayload, winBytes)
        XCTAssertEqual(loserBoard.view.map { $0.hasSaidGood(loser) }, false,
                       "the good this seat SENT is gone from its own board")
        XCTAssertTrue(loserBoard.humanLegal.contains { $0.type == .good },
                      "…and it must say good again")
        XCTAssertFalse(loserBoard.superseded, "nothing marks the board; nothing tells the human")
        for board in boards.values { board.setBoardWatching(false) }
    }

    // MARK: - a staged good, invalidated by an arrival (note 6)

    /// NOTE 6, the CONTROLLER half: "Live arrival that invalidates a good
    /// should update the staged bubble". One attacker has said good; a second
    /// stages good too; before Send the THIRD throws in a card off the same
    /// bubble, which re-opens the table the staged good was saying "done" to.
    /// The controller retracts and adopts - the board is right - but the bubble
    /// in the input field is the one it sealed BEFORE the arrival, and nothing
    /// replaces it (the investigation found no caller that re-stages after a
    /// retraction). The input field itself is the harness's to show; this pins
    /// what sending the leftover bubble then does.
    ///
    /// (A CLOSING good cannot be invalidated this way: by construction every
    /// other attacker has said good, and a seat that has said good is not
    /// offered a throw-in - the fixture search below proves it by finding none.)
    ///
    /// Measured: the leftover good is a SIBLING of the throw-in, carrying two
    /// pending goods (turn N+2) against the throw-in's one atom past the
    /// parent (N+1). Rule P prefers it on TURN, so sending it is refused
    /// nowhere: it rebases this board onto the stale good and erases the other
    /// attacker's throw-in for the whole thread.
    func testSendingTheLeftoverGoodAfterAnInvalidatingArrivalOverrulesIt() async throws {
        var thrower = -1
        let f = try await LiveArrivalFixture.coveredTable(players: 4) { f in
            for s in f.attackers.dropFirst() {
                if try await f.legal(s, on: f.root).contains(where: { $0.type == .attack }) {
                    thrower = s; return true
                }
            }
            return false
        }
        let a = f.attackers[0]
        let stager = try XCTUnwrap(f.attackers.dropFirst().first { $0 != thrower })
        let x1 = try await f.good(a, after: f.root)
        let saidGoodMenu = try await f.legal(a, on: x1)
        XCTAssertFalse(saidGoodMenu.contains { $0.type == .attack },
                       "fixture check: a seat that said good is offered no throw-in")

        // The stager says good on x1 (not closing - the thrower has not) and
        // its phone seals the bubble into the input field.
        let board = await f.controller(seat: stager, on: x1)
        let good = try XCTUnwrap(board.legal.first { $0.type == .good })
        let applied = await board.apply(good)
        XCTAssertTrue(applied)
        XCTAssertEqual(board.view.map { $0.hasSaidGood(stager) }, true, "my check is up")
        let leftover = try await board.stagedPayload()

        // The thrower throws in off x1, and it arrives over the staged good.
        let menu = try await f.legal(thrower, on: x1)
        let attack = try XCTUnwrap(menu.first { $0.type == .attack })
        let x2 = try await f.play(thrower, attack, after: x1)
        await f.arrive(x2, at: board, finishRetraction: false)
        XCTAssertTrue(board.conflictRetracting || board.basePayload == x2.payload,
                      "the arrival either retracts the staged good or has already adopted")
        if board.conflictRetracting { await board.finishConflictAdopt() }
        XCTAssertEqual(board.basePayload, x2.payload, "the arrival was adopted")
        XCTAssertTrue(board.pending.isEmpty, "the staged good was dropped")
        XCTAssertFalse(board.canSend, "the controller has nothing staged any more")
        XCTAssertEqual(board.view.map { $0.hasSaidGood(stager) }, false, "my check is gone")

        // The leftover bubble is a sibling of x2, and Rule P prefers it.
        let pref = try await MessageKernel.shared.preferred(x2.payload, leftover)
        XCTAssertGreaterThan(pref, 0, "the stale good outranks the throw-in")

        // The human presses Send on what is still in the input field.
        await board.markSent(payload: leftover)
        XCTAssertEqual(board.basePayload, leftover,
                       "the board jumps to the stale good's branch, and so does the thread")
        let shown = try XCTUnwrap(board.view)
        let parentTable = try await f.truth(stager, on: x1)?.battles.count
        XCTAssertEqual(shown.battles.count, parentTable, "the throw-in is gone")
        board.setBoardWatching(false)
    }
}
