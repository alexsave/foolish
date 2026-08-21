// MessageEventsTests — the kernel is the single source of animation truth on
// iMessage too (docs/C_CORE_CONSOLIDATION.md A5/F4). An iMessage client holds
// only the encoded chain (no server emitted events at move time), so it asks the
// kernel for "the animations of the last move" via MessageKernel.lastMoveEvents
// -> fio_replay_last_events_packed -> the packed evwire frame -> EvWire.decode.
//
// The bug this pins: the OLD open-replay derived my own drawn cards by diffing
// two GameViews, which cannot recover them (the replayed hand looks the same
// from the diff's side), so a reopened pickup animated everyone ELSE's refill
// but never my own. The kernel, replaying with MY seat as the viewer, reveals my
// drawn cards and hides the opponents' - the SAME viewer-aware evwire stream live
// play emits. This test proves that end to end, and would fail against any client
// that went back to diffing.
import XCTest
@testable import FoolishKit

@MainActor
final class MessageEventsTests: XCTestCase {

    private func freshSeed(_ salt: UInt8) -> Data {
        Data((0..<32).map { UInt8(truncatingIfNeeded: $0 &* 11 &+ Int(salt)) | 1 })
    }

    /// Play a 2p game to a defender pickup (which refills the attacker from the
    /// deck), then read the last move's events for each seat. The attacker's
    /// REFILL event must carry REAL cards to the attacker and HIDDEN ones to the
    /// defender - proving the mask is the kernel's, per viewer, not the client's.
    func testLastMoveEventsRevealMyOwnRefillAndHideTheOpponents() async throws {
        let k = MessageKernel.shared
        try await k.newGame(seed: freshSeed(5), players: 2)

        // Whoever holds a legal attack is the first attacker; the other defends.
        let legal0 = await k.residentLegal(seat: 0)
        let attacker = legal0.contains(where: { $0.type == .attack }) ? 0 : 1
        let defender = 1 - attacker

        // Attack with the first legal attack, then the defender picks it up - one
        // handle_pickup call that also refills the attacker back up from the deck.
        let atkLegal = await k.residentLegal(seat: attacker)
        let atk = try XCTUnwrap(atkLegal.first { $0.type == .attack })
        try await k.apply(seat: attacker, move: atk)
        let defLegal = await k.residentLegal(seat: defender)
        let pick = try XCTUnwrap(defLegal.first { $0.type == .pickup })
        try await k.apply(seat: defender, move: pick)

        // The last move's events, as each seat would see them animate.
        let mine = await k.lastMoveEvents(viewer: attacker)
        let theirs = await k.lastMoveEvents(viewer: defender)
        XCTAssertFalse(mine.isEmpty, "the pickup step must produce animation events")

        // The attacker's refill, seen by the attacker: real cards (identities I am
        // allowed to know, because they landed in MY hand).
        let myRefill = mine.first { $0.kind == .refill && $0.seat == attacker }
        let refill = try XCTUnwrap(myRefill, "the attacker refills after a pickup")
        XCTAssertTrue(refill.cards.contains { $0 != nil },
                      "my own drawn cards must carry real identities, not backs")

        // The SAME refill, seen by the defender: every card redacted (a back).
        let theirView = theirs.first { $0.kind == .refill && $0.seat == attacker }
        if let tv = theirView {
            XCTAssertTrue(tv.cards.allSatisfy { $0 == nil },
                          "another seat's draws must be hidden from me")
        }
    }

    /// Round-3 "double animations": a move animated when you played it, and
    /// again a moment later. The replay itself was never the problem — opening a
    /// chain shows its last move, whoever made it, and the owner confirmed that
    /// is what they want ("the replay works fine for the OTHER player... for the
    /// self, it doesn't play at all now" — after an earlier pass suppressed it).
    /// The double was the HOST reloading the whole surface from the bubble you
    /// had just sent; that is fixed in the hosts (HarnessModel.boardEpoch,
    /// StagedBubbleRouting.lastSentPayload), not here.
    ///
    /// So what this pins is the opposite of what it used to: the events a chain
    /// yields do NOT depend on who sealed it. Same bytes, both seats, both get
    /// the move to watch.
    func testAChainsLastMoveIsReplayedToEitherSeat() async throws {
        // Seat 0 deals and opens; seat 1 replies. `p1`'s last actor is seat 1.
        var found: MessageTurnController?
        for salt in UInt8(20)...UInt8(60) {
            let c = MessageTurnController(genesisSeed: freshSeed(salt), players: 2,
                                          gameId: 77, myNickname: "A")
            await c.begin()
            if c.legal.contains(where: { $0.type != .wait }) { found = c; break }
        }
        let creator = try XCTUnwrap(found, "no 2p deal made seat 0 the first attacker")
        let open = try XCTUnwrap(creator.legal.first { $0.type != .wait })
        await creator.apply(open)
        let p0 = try await creator.stagedPayload()
        let e0 = try await MessageEnvelope.decode(payload: p0, viewer: -1)

        let joiner = MessageTurnController(parentPayload: p0, parent: e0, mySeat: 1)
        await joiner.begin()
        let reply = try XCTUnwrap(joiner.legal.first { $0.type != .wait })
        await joiner.apply(reply)
        let p1 = try await joiner.stagedPayload()
        let e1 = try await MessageEnvelope.decode(payload: p1, viewer: -1)
        XCTAssertEqual(e1.lastActorSeat, 1, "fixture: seat 1 sealed this chain")

        let receiver = MessageTurnController(parentPayload: p1, parent: e1, mySeat: 0)
        await receiver.begin()
        XCTAssertFalse(receiver.openReplayEvents.isEmpty,
                       "the seat that did not move is shown what happened")

        let sender = MessageTurnController(parentPayload: p1, parent: e1, mySeat: 1)
        await sender.begin()
        XCTAssertFalse(sender.openReplayEvents.isEmpty,
                       "and so is the seat that did — opening a chain always shows its last move")
    }

    /// A sanity check on the decoder itself: the packed frame decodes to a
    /// non-empty, well-formed sequence whose events carry a from/to location. A
    /// malformed or empty frame must degrade to [], never crash.
    func testEvWireDecodeIsWellFormedAndEmptyIsSafe() async throws {
        let k = MessageKernel.shared
        try await k.newGame(seed: freshSeed(9), players: 2)
        let legal0 = await k.residentLegal(seat: 0)
        let attacker = legal0.contains { $0.type == .attack } ? 0 : 1
        let atkLegal = await k.residentLegal(seat: attacker)
        let atk = try XCTUnwrap(atkLegal.first { $0.type == .attack })
        try await k.apply(seat: attacker, move: atk)

        let events = await k.lastMoveEvents(viewer: attacker)
        XCTAssertFalse(events.isEmpty, "an attack is one animation event at least")
        XCTAssertTrue(events.contains { $0.kind == .attackPass }, "the attack itself is in the stream")

        XCTAssertTrue(events.isEmpty == false)
        XCTAssertTrue(EvWire.decode(Data()).isEmpty, "an empty frame is not a crash")
        XCTAssertTrue(EvWire.decode(Data([0xFF, 0x00, 0x00])).isEmpty, "a bad version is not a crash")
        XCTAssertTrue(EvWire.decodeFrames(Data()).isEmpty, "no frames is not a crash")
        // A length prefix promising more bytes than are there: keep what was
        // whole (nothing, here), never read past the end.
        XCTAssertTrue(EvWire.decodeFrames(Data([0x40, 0x00, 0x01])).isEmpty)
    }

    /// Round-4 note 3: "if it's a double cover, the first cover will just
    /// already be there, and only the second one will play."
    ///
    /// A bubble carries a whole TURN, and staging is per-action, so a defender
    /// who covers two attacks before sending puts TWO cover steps on the chain.
    /// The kernel used to hand back the final step alone, so the receiver
    /// watched one cover fly onto a table that already had the other sitting
    /// there, landed and rotated. Both must be in the stream.
    func testAStagedDoubleCoverReplaysBothCovers() async throws {
        // Find a 2p deal where the attacker can open with two throw-ins the
        // defender can beat — i.e. a real double cover exists to stage.
        for salt in UInt8(1)...UInt8(80) {
            let k = MessageKernel.shared
            try await k.newGame(seed: freshSeed(salt), players: 2)
            let legal0 = await k.residentLegal(seat: 0)
            let attacker = legal0.contains { $0.type == .attack } ? 0 : 1
            let defender = 1 - attacker

            // Two attacks, so there are two uncovered battles to beat.
            guard let a1 = (await k.residentLegal(seat: attacker)).first(where: { $0.type == .attack })
            else { continue }
            try await k.apply(seat: attacker, move: a1)
            guard let a2 = (await k.residentLegal(seat: attacker)).first(where: { $0.type == .attack })
            else { continue }
            try await k.apply(seat: attacker, move: a2)

            // …then the defender covers BOTH, as two staged actions in one turn.
            guard let c1 = (await k.residentLegal(seat: defender)).first(where: { $0.type == .cover })
            else { continue }
            try await k.apply(seat: defender, move: c1)
            guard let c2 = (await k.residentLegal(seat: defender)).first(where: { $0.type == .cover })
            else { continue }
            try await k.apply(seat: defender, move: c2)

            let events = await k.lastMoveEvents(viewer: attacker)
            let covers = events.filter { $0.kind == .cover }
            XCTAssertEqual(covers.count, 2,
                           "both staged covers belong to the turn the bubble carries")
            // …and they are DIFFERENT cards, so this cannot pass by replaying
            // the same step twice.
            let ids = Set(covers.flatMap { $0.cards.compactMap { $0?.identity } })
            XCTAssertEqual(ids.count, 2, "two covers, two distinct cards")
            return
        }
        XCTFail("no 2p deal in 80 tries allowed a staged double cover")
    }

    /// The other side of the same rule, and the one that keeps the group from
    /// swallowing the whole game: the run stops at the first step someone else
    /// played. An attack by seat A followed by a cover by seat B is TWO turns,
    /// and opening the chain replays only B's.
    func testTheReplayedTurnStopsAtTheOtherSeatsAction() async throws {
        let k = MessageKernel.shared
        for salt in UInt8(1)...UInt8(80) {
            try await k.newGame(seed: freshSeed(salt), players: 2)
            let legal0 = await k.residentLegal(seat: 0)
            let attacker = legal0.contains { $0.type == .attack } ? 0 : 1
            let defender = 1 - attacker
            guard let atk = (await k.residentLegal(seat: attacker)).first(where: { $0.type == .attack })
            else { continue }
            try await k.apply(seat: attacker, move: atk)
            guard let cov = (await k.residentLegal(seat: defender)).first(where: { $0.type == .cover })
            else { continue }
            try await k.apply(seat: defender, move: cov)

            let events = await k.lastMoveEvents(viewer: attacker)
            XCTAssertTrue(events.contains { $0.kind == .cover }, "the cover is the last turn")
            XCTAssertFalse(events.contains { $0.kind == .attackPass },
                           "the attacker's own earlier turn is NOT replayed again")
            return
        }
        XCTFail("no 2p deal in 80 tries produced an attack the defender could cover")
    }

    /// Round-7 (replay bunch): a reopened pickup/discard must reconstruct the
    /// pre-bout TABLE so the board can lay it out (invisibly) and fly each swept
    /// card from its real slot instead of a shared centre. `openReplayPreBattles`
    /// is that table. For a PICKUP it is one uncovered slot per picked-up card
    /// (the pickup step's own cards - the kernel never masks a pickup); for a
    /// DISCARD it is the full covered table the trash step swept.
    ///
    /// Mutation guard: assert it is NON-EMPTY and names the RIGHT cards, so a
    /// regression back to "no pre-battles" (every card bunches at centre) fails
    /// here rather than only on a device.
    func testOpenReplayReconstructsThePreBoutTableForAPickup() async throws {
        // Seat A attacks, seat B (defender) picks it up, then B sends. Reopening
        // B's bubble as EITHER seat must reconstruct the picked-up card as one
        // uncovered battle.
        for salt in UInt8(1)...UInt8(80) {
            let k = MessageKernel.shared
            var creator: MessageTurnController?
            for s2 in UInt8(0)...UInt8(40) {
                let c = MessageTurnController(genesisSeed: freshSeed(salt &+ s2), players: 2,
                                              gameId: 91, myNickname: "A")
                await c.begin()
                if c.legal.contains(where: { $0.type == .attack }) { creator = c; break }
            }
            guard let a = creator, let atk = a.legal.first(where: { $0.type == .attack })
            else { continue }
            await a.apply(atk)
            let p0 = try await a.stagedPayload()
            let e0 = try await MessageEnvelope.decode(payload: p0, viewer: -1)

            let b = MessageTurnController(parentPayload: p0, parent: e0, mySeat: 1)
            await b.begin()
            guard let pick = b.legal.first(where: { $0.type == .pickup }) else { continue }
            // The card on the table now (the attack) is what the pickup sweeps.
            let tableCard = try XCTUnwrap(b.view?.battles.first?.attack)
            await b.apply(pick)
            let p1 = try await b.stagedPayload()
            let e1 = try await MessageEnvelope.decode(payload: p1, viewer: -1)

            let reopen = MessageTurnController(parentPayload: p1, parent: e1, mySeat: 0)
            await reopen.begin()
            let pre = reopen.openReplayPreBattles
            XCTAssertFalse(pre.isEmpty, "a reopened pickup must reconstruct the pre-bout table")
            XCTAssertTrue(pre.contains { $0.attack == tableCard },
                          "the picked-up card must be one of the reconstructed slots")
            XCTAssertTrue(pre.allSatisfy { $0.defense == nil },
                          "a pickup lays each card in its own UNCOVERED slot")
            return
        }
        XCTFail("no 2p deal in 80 tries produced an attack the defender could pick up")
    }

    func testOpenReplayReconstructsThePreBoutTableForADiscard() async throws {
        for salt in UInt8(1)...UInt8(120) {
            let k = MessageKernel.shared
            var creator: MessageTurnController?
            for s2 in UInt8(0)...UInt8(40) {
                let c = MessageTurnController(genesisSeed: freshSeed(salt &+ s2), players: 2,
                                              gameId: 92, myNickname: "A")
                await c.begin()
                if c.legal.contains(where: { $0.type == .attack }) { creator = c; break }
            }
            guard let a = creator, let atk = a.legal.first(where: { $0.type == .attack })
            else { continue }
            await a.apply(atk)
            let p0 = try await a.stagedPayload()
            let e0 = try await MessageEnvelope.decode(payload: p0, viewer: -1)

            let b = MessageTurnController(parentPayload: p0, parent: e0, mySeat: 1)
            await b.begin()
            guard let cov = b.legal.first(where: { $0.type == .cover }) else { continue }
            let covered = try XCTUnwrap(b.view?.battles.first?.attack)
            await b.apply(cov)
            let coverCard = try XCTUnwrap(b.view?.battles.first?.defense)
            let p1 = try await b.stagedPayload()
            let e1 = try await MessageEnvelope.decode(payload: p1, viewer: -1)

            // Seat A now says good -> the covered table is swept to the discard.
            let a2 = MessageTurnController(parentPayload: p1, parent: e1, mySeat: 0)
            await a2.begin()
            guard let good = a2.legal.first(where: { $0.type == .good }) else { continue }
            await a2.apply(good)
            let p2 = try await a2.stagedPayload()
            let e2 = try await MessageEnvelope.decode(payload: p2, viewer: -1)

            let reopen = MessageTurnController(parentPayload: p2, parent: e2, mySeat: 1)
            await reopen.begin()
            let pre = reopen.openReplayPreBattles
            guard reopen.openReplayEvents.contains(where: { $0.kind == .discard || $0.kind == .cardsToTrash })
            else { continue }
            XCTAssertFalse(pre.isEmpty, "a reopened discard must reconstruct the pre-bout table")
            XCTAssertTrue(pre.contains { $0.attack == covered && $0.defense == coverCard },
                          "the swept table must carry the covered attack+defense pair, not a bare list")
            return
        }
        XCTFail("no 2p deal in 120 tries produced a clean covered defence to discard")
    }

    // MARK: round-8 #2 — the "says good" bubble captions

    private let names = [0: "Alex", 1: "Vera", 2: "Boris"]

    /// A bare good (3p: one attacker says good, the bout stays open) is the ONE
    /// move that leaves the kernel's event stream with no headline at all - the
    /// old summary fell through to the generic "tap to play" line. It must now
    /// read "X says good", named from the envelope's own lastActorSeat. Built
    /// the way real bubbles are (each action sealed into the chain, the summary
    /// read off the decoded chain), because the grouping of "the last move"
    /// only holds at the chain's action boundaries.
    func testBareGoodCaptionNamesTheActor() async throws {
        let k = MessageKernel.shared
        for salt in UInt8(1)...UInt8(120) {
            var creator: MessageTurnController?
            for s2 in UInt8(0)...UInt8(40) {
                let c = MessageTurnController(genesisSeed: freshSeed(salt &+ s2), players: 3,
                                              gameId: 93, myNickname: "Alex")
                await c.begin()
                if c.legal.contains(where: { $0.type == .attack }) { creator = c; break }
            }
            guard let a = creator, let atk = a.legal.first(where: { $0.type == .attack })
            else { continue }
            await a.apply(atk)
            let p0 = try await a.stagedPayload()
            let e0 = try await MessageEnvelope.decode(payload: p0, viewer: -1)

            let defenderOrNil = await firstSeat(k, n: 3, with: .cover)
            guard let defender = defenderOrNil else { continue }
            let b = MessageTurnController(parentPayload: p0, parent: e0, mySeat: defender)
            await b.begin()
            guard let cov = b.legal.first(where: { $0.type == .cover }) else { continue }
            await b.apply(cov)
            let p1 = try await b.stagedPayload()
            let e1 = try await MessageEnvelope.decode(payload: p1, viewer: -1)

            // ONE of the two attackers says good; with the other still to
            // speak, the bout stays open - the exact silent-move case.
            guard let gooder = await firstSeat(k, n: 3, with: .good) else { continue }
            let g = MessageTurnController(parentPayload: p1, parent: e1, mySeat: gooder)
            await g.begin()
            guard let good = g.legal.first(where: { $0.type == .good }) else { continue }
            await g.apply(good)
            let p2 = try await g.stagedPayload()
            let e2 = try await MessageEnvelope.decode(payload: p2, viewer: -1)
            guard let v = await k.residentView(viewer: -1), !v.battles.isEmpty else { continue }

            let events = await k.lastMoveEvents(viewer: -1)
            let s = MessageSummary.move(events: events, names: names, view: v,
                                        actor: e2.lastActorSeat)
            XCTAssertEqual(s, FStrings.t("ios.msg.mv.good", ["name": names[gooder]!]),
                           "a bare good must caption who said it, not the generic tap line")
            return
        }
        XCTFail("no 3p deal in 120 tries produced attack -> cover -> one good with the bout open")
    }

    /// A good that CLOSES the bout emits only consequences (discard + draws +
    /// the round transition) - still no headline naming the actor. The caption
    /// must read "X says good · Round over - Y attacks next". Same sealed-chain
    /// construction as the bare-good test above.
    func testRoundClosingGoodCaptionCarriesGoodAndRoundOver() async throws {
        let k = MessageKernel.shared
        for salt in UInt8(1)...UInt8(120) {
            var creator: MessageTurnController?
            for s2 in UInt8(0)...UInt8(40) {
                let c = MessageTurnController(genesisSeed: freshSeed(salt &+ s2), players: 2,
                                              gameId: 94, myNickname: "Alex")
                await c.begin()
                if c.legal.contains(where: { $0.type == .attack }) { creator = c; break }
            }
            guard let a = creator, let atk = a.legal.first(where: { $0.type == .attack })
            else { continue }
            await a.apply(atk)
            let p0 = try await a.stagedPayload()
            let e0 = try await MessageEnvelope.decode(payload: p0, viewer: -1)

            let b = MessageTurnController(parentPayload: p0, parent: e0, mySeat: 1)
            await b.begin()
            guard let cov = b.legal.first(where: { $0.type == .cover }) else { continue }
            await b.apply(cov)
            let p1 = try await b.stagedPayload()
            let e1 = try await MessageEnvelope.decode(payload: p1, viewer: -1)

            let g = MessageTurnController(parentPayload: p1, parent: e1, mySeat: 0)
            await g.begin()
            guard let good = g.legal.first(where: { $0.type == .good }) else { continue }
            await g.apply(good)
            let p2 = try await g.stagedPayload()
            let e2 = try await MessageEnvelope.decode(payload: p2, viewer: -1)
            guard let v = await k.residentView(viewer: -1), v.battles.isEmpty, !v.isOver
            else { continue }

            let events = await k.lastMoveEvents(viewer: -1)
            let s = MessageSummary.move(events: events, names: names, view: v,
                                        actor: e2.lastActorSeat)
            XCTAssertTrue(s.contains(FStrings.t("ios.msg.mv.good", ["name": names[0]!])),
                          "a round-closing good must still say who said it: \(s)")
            if v.firstAttacker >= 0 {
                XCTAssertTrue(s.contains(FStrings.t("ios.msg.mv.roundover",
                                                    ["name": names[v.firstAttacker]!])),
                              "the round transition must name the next attacker: \(s)")
            }
            return
        }
        XCTFail("no 2p deal in 120 tries produced attack -> cover -> good closing the bout")
    }

    /// ROUND 12 (owner): "multiple cover does not result in full text
    /// explanation in bubble text, only one cover". A bubble carries the whole
    /// trailing run of steps its sender staged, so a defender who covers TWO
    /// cards before pressing Send sends one bubble describing two covers - and
    /// the caption named only the first.
    ///
    /// Built through the real seal path (attacker stages two same-rank cards in
    /// ONE bubble, defender covers both in ONE bubble) so the events under test
    /// are the ones a real double cover produces, not a hand-built stream.
    func testDoubleCoverCaptionNamesBothCovers() async throws {
        let k = MessageKernel.shared
        for salt in UInt8(1)...UInt8(120) {
            let a = MessageTurnController(genesisSeed: freshSeed(salt), players: 2,
                                          gameId: 96, myNickname: "Alex")
            await a.begin()
            guard let atk = a.legal.first(where: { $0.type == .attack }) else { continue }
            await a.apply(atk)
            // A second attack in the SAME bubble - a throw-in of the matching
            // rank. Only some deals allow it; that is what the seed loop is for.
            guard let atk2 = a.legal.first(where: { $0.type == .attack }) else { continue }
            await a.apply(atk2)
            let p0 = try await a.stagedPayload()
            let e0 = try await MessageEnvelope.decode(payload: p0, viewer: -1)

            let defender = 1 - a.mySeat
            let b = MessageTurnController(parentPayload: p0, parent: e0, mySeat: defender)
            await b.begin()
            guard let c1 = b.legal.first(where: { $0.type == .cover }) else { continue }
            await b.apply(c1)
            guard let c2 = b.legal.first(where: { $0.type == .cover }) else { continue }
            await b.apply(c2)
            let p1 = try await b.stagedPayload()
            let e1 = try await MessageEnvelope.decode(payload: p1, viewer: -1)

            let events = await k.lastMoveEvents(viewer: -1)
            let v = await k.residentView(viewer: -1)
            let s = MessageSummary.move(events: events, names: names, view: v,
                                        actor: e1.lastActorSeat)

            // Both covers are in the sentence: all four cards it moved, named.
            let covers = events.filter { $0.msg == 4 }
            XCTAssertEqual(covers.count, 2, "the bubble must carry two cover events")
            for e in covers {
                if let t = e.target {
                    XCTAssertTrue(s.contains(MessageSummary.card(t)),
                                  "the beaten card \(MessageSummary.card(t)) is missing: \(s)")
                }
                if let d = e.cards.compactMap({ $0 }).first {
                    XCTAssertTrue(s.contains(MessageSummary.card(d)),
                                  "the covering card \(MessageSummary.card(d)) is missing: \(s)")
                }
            }
            // ONE sentence, not two: the defender is named once, and the pairs
            // are listed inside that one clause.
            XCTAssertEqual(s.components(separatedBy: names[defender]!).count - 1, 1,
                           "a merged double cover names its player once: \(s)")
            return
        }
        XCTFail("no 2p deal in 120 tries produced a double attack answered by a double cover")
    }

    /// The synthesis must be CONDITIONAL: a move with a real headline (an
    /// attack) may never also claim its actor said good.
    func testAttackCaptionDoesNotSynthesizeGood() async throws {
        let k = MessageKernel.shared
        try await k.newGame(seed: freshSeed(9), players: 2)
        let attackerOrNil = await firstSeat(k, n: 2, with: .attack)
        let attacker = try XCTUnwrap(attackerOrNil)
        let atk = await k.residentLegal(seat: attacker).first { $0.type == .attack }!
        try await k.apply(seat: attacker, move: atk)
        let events = await k.lastMoveEvents(viewer: -1)
        let v = await k.residentView(viewer: -1)
        let s = MessageSummary.move(events: events, names: names, view: v, actor: attacker)
        XCTAssertFalse(s.contains(FStrings.t("ios.msg.mv.good", ["name": names[attacker]!])),
                       "an attack already has a headline; no good may be synthesized: \(s)")
        XCTAssertTrue(s.contains(names[attacker]!), "the attack headline names the attacker")
    }

    private func firstSeat(_ k: MessageKernel, n: Int, with type: MoveType) async -> Int? {
        for s in 0..<n {
            let legal = await k.residentLegal(seat: s)
            if legal.contains(where: { $0.type == type }) { return s }
        }
        return nil
    }
}
