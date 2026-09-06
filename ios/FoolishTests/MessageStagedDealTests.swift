// A staged move must not deal you a hand you can still take back (round 16).
//
// The owner: "if you hit good and it causes a discard, and you get cards dealt,
// you can see the next cards in the deck and make a decision accordingly,
// undoing your move before sending it. This is kinda cheating. […] It's not
// enough to remove undo as the user could just delete the staged message and
// basically undo the move that way. Thus pressing good cannot show the dealt
// cards."
//
// Three moves close a bout and every one of them deals in the same kernel
// apply: the last good owed, a cover that empties the defender's hand (four
// same-rank covers over four same-rank attacks is this case, not a rule of its
// own), and a pickup - which refills the picker too whenever the table left
// them short of six. So the turn is cut at the kernel's own settlement boundary
// (MessageKernel.stagedTurn's settlementCut -> evwire_frames_settlement_cut) and
// the second half is withheld until Send.
//
// WHAT IS PINNED HERE is the property, not the split: while a move is staged,
// NOTHING HAS COME OUT OF THE DECK. Every card the board can show its player
// was already in their hand or already face-up on the table. That is checkable
// without knowing how the hold works, and it is exactly what the cheat needs.
//
// The other half matters just as much and is checked in the same breath: the
// bubble still carries the whole turn. Withholding is a thing this DEVICE does
// to a move it can still take back - a recipient was never in that position, so
// nothing about the seal changes.
import XCTest
@testable import FoolishKit

@MainActor
final class MessageStagedDealTests: XCTestCase {

    private func seed(_ salt: UInt8) -> Data {
        var d = Data(repeating: 0, count: 32)
        for i in 0..<32 { d[i] = salt &+ UInt8(truncatingIfNeeded: i * 11) }
        return d
    }

    /// Everything a seat could legitimately be holding after a move of its own,
    /// before any deal: what it held, plus what was face-up on the table (a
    /// pickup takes those, and they are nobody's secret).
    private func knowable(_ v: GameView) -> Set<String> {
        var ids = Set((v.me?.hand ?? []).map(\.identity))
        for b in v.battles {
            ids.insert(b.attack.identity)
            if let d = b.defense { ids.insert(d.identity) }
        }
        return ids
    }

    // MARK: the sweep

    /// THE ONE. Whole games at 2, 3 and 4 players, played the way the extension
    /// plays them - every applied move auto-stages, a turn may be several
    /// actions, Send rebases the controller onto its own bytes - and at every
    /// point where a move is staged but not yet sent, the board must not be
    /// showing a card that came off the deck.
    ///
    /// A sweep rather than a fixture because the three bout-enders arise from
    /// the run of play, and a staged-turn census is the only way to be sure all
    /// three are covered (the counts are asserted at the end, so a sweep that
    /// stops finding them fails rather than passing vacuously).
    func testAStagedTurnNeverShowsACardOffTheDeck() async throws {
        var leaks: [String] = []
        var staged = 0, settled = 0, dealt = 0, pickups = 0, goods = 0, covers = 0

        for players in [2, 3, 4] {
            for salt in UInt8(1)...UInt8(16) {
                var payload: Data?
                var env: MessageEnvelope?
                var rng = UInt64(salt) &* 6364136223846793005 &+ UInt64(players)
                func next() -> Int {
                    rng = rng &* 6364136223846793005 &+ 1442695040888963407
                    return Int((rng >> 33) & 0xffff)
                }

                for _ in 0..<120 {
                    var acted = false
                    let start = next() % players
                    for k in 0..<players {
                        let seat = (start + k) % players
                        let c: MessageTurnController
                        if let p = payload, let e = env {
                            c = MessageTurnController(parentPayload: p, parent: e, mySeat: seat)
                        } else {
                            c = MessageTurnController(genesisSeed: seed(salt), players: players,
                                                      gameId: 77, myNickname: "P\(seat)")
                        }
                        await c.begin()
                        if c.view?.isOver == true { break }
                        guard var move = c.legal.first(where: { $0.type != .wait }) else { continue }

                        // The board BEFORE this turn: what this seat could
                        // possibly know without the deck telling it something.
                        guard let before = c.view else { continue }
                        var allowed = knowable(before)
                        let deckBefore = before.deckCount

                        var sealed: Data?
                        var envOut: MessageEnvelope?
                        var kinds: [MoveType] = []
                        var more = next() % 3
                        while true {
                            await c.apply(move)
                            guard !c.pending.isEmpty else { break }
                            kinds.append(move.type)
                            let s = try await c.stagedPayload(sentAt: MessageKernel.clockNow() - 60)
                            sealed = s
                            envOut = try? await MessageEnvelope.peek(payload: s)
                            staged += 1

                            // THE CHECK, made where the cheat would be made: on
                            // the board as it stands with the move staged and
                            // Send not yet pressed.
                            if let now = c.view {
                                let shown = Set((now.me?.hand ?? []).map(\.identity))
                                let fresh = shown.subtracting(allowed)
                                if !fresh.isEmpty || now.deckCount < deckBefore {
                                    leaks.append("p\(players) seed \(salt) seat \(seat) "
                                                 + "\(kinds) -> \(fresh.count) new card(s), deck "
                                                 + "\(deckBefore)->\(now.deckCount), "
                                                 + "held=\(c.settlementHeld)")
                                }
                                // A later action of the same turn may legally
                                // put more on the table (a throw-in), and its
                                // own cards were already in hand.
                                allowed.formUnion(knowable(now))
                            }
                            if c.settlementHeld {
                                settled += 1
                                switch kinds.last {
                                case .pickup: pickups += 1
                                case .cover:  covers += 1
                                default:      goods += 1
                                }
                                // `legal` is the decode of `legalPacked`, the
                                // menu the board actually plays from, so this
                                // is both halves of the hold at once.
                                XCTAssertTrue(c.legal.isEmpty,
                                              "a held bout end left \(c.legal.count) moves legal - "
                                              + "the deal is hidden but still playable")
                            }
                            guard more > 0, !c.settlementHeld, c.isOver != true,
                                  let nxt = c.legal.first(where: { $0.type != .wait }) else { break }
                            more -= 1
                            move = nxt
                        }
                        guard let s = sealed, let e2 = envOut, !kinds.isEmpty else { continue }

                        let held = c.settlementHeld
                        await c.markSent(payload: s)
                        // …and the other half: Send hands the deal over. Not
                        // asserted per turn (a dry deck deals nothing) but
                        // counted, so a hold that never released would show up
                        // as a sweep that never dealt.
                        if held, let after = c.view, after.deckCount < deckBefore { dealt += 1 }
                        payload = s; env = e2; acted = true
                        break
                    }
                    if !acted { break }
                }
            }
        }

        // Census, so a sweep that stops reaching a case fails instead of
        // passing on the cases it still reaches. All three bout-enders are
        // named separately because they close a bout by three different routes
        // through the kernel.
        XCTAssertGreaterThan(staged, 800, "the sweep never played any games")
        XCTAssertGreaterThan(settled, 100, "no staged turn ever ended a bout")
        XCTAssertGreaterThan(goods, 20, "no staged GOOD closed a bout")
        XCTAssertGreaterThan(pickups, 20, "no staged PICKUP was seen")
        XCTAssertGreaterThan(covers, 3, "no staged cover swept the table with its last card")
        XCTAssertGreaterThan(dealt, 60, "Send never released a deal - the hold never lifts")
        XCTAssertEqual(leaks.first, nil,
                       "\(leaks.count)/\(staged) staged turns showed their player the deal")
    }

    // MARK: what the bubble carries

    /// The seal is untouched: a held turn's bubble still carries its whole bout
    /// end, so the recipient animates the discard, the deal and the roles the
    /// moment it lands. Withholding is a fact about a move this device can still
    /// take back, and a recipient can never take it back.
    func testTheBubbleStillCarriesTheWholeBoutEnd() async throws {
        let (c, sealed, deckBefore) = try await stagedBoutEnd()
        XCTAssertTrue(c.settlementHeld, "fixture: the staged turn did not end a bout")
        XCTAssertEqual(c.view?.deckCount, deckBefore, "the sender was shown the deal")

        // The recipient's side, through the ordinary open: decode the bubble and
        // read the board it produces.
        let env = try await MessageEnvelope.decode(payload: sealed, viewer: 0)
        let board = await MessageKernel.shared.residentView(viewer: -1)
        let seen = try XCTUnwrap(board)
        XCTAssertLessThan(seen.deckCount, deckBefore,
                          "the bubble did not carry the deal its bout end dealt")
        XCTAssertEqual(env.phase, 2)
    }

    /// Send releases it, and the settlement it releases is the one that was
    /// withheld - the same steps, in the same order, that the recipient will
    /// see.
    func testSendReleasesTheHeldSettlement() async throws {
        let (c, sealed, deckBefore) = try await stagedBoutEnd()
        XCTAssertNil(c.takeReleasedSettlement(), "nothing is released before Send")

        await c.markSent(payload: sealed)
        XCTAssertFalse(c.settlementHeld)
        let released = try XCTUnwrap(c.takeReleasedSettlement(),
                                     "Send released no settlement to animate")
        XCTAssertTrue(released.first?.isSettlement == true,
                      "the released half must start at the settlement, not before it")
        XCTAssertLessThan(c.view?.deckCount ?? 99, deckBefore, "the deal never arrived")
        XCTAssertNil(c.takeReleasedSettlement(), "the release is one-shot")
    }

    /// `legal` IS `legalPacked`, always. The board plays from the packed menu
    /// (every play is the kernel's answer over it) and reads moves off the
    /// decoded one, and the hold that empties a menu while a settlement stands
    /// is applied to the packed value alone - so a second, independent
    /// assignment of `legal` would be a menu the board does not play from.
    ///
    /// Asserted on a live board with a menu on it, so it says something: an
    /// empty menu agrees with an empty menu whatever the derivation is.
    func testTheDecodedMenuIsAlwaysTheDecodeOfTheOne() async throws {
        var sawAMenu = false
        for salt in UInt8(1)...UInt8(8) {
            let c = MessageTurnController(genesisSeed: seed(salt), players: 2,
                                          gameId: 93, myNickname: "P0")
            await c.begin()
            XCTAssertEqual(c.legal, MoveWire.decode(c.legalPacked))
            guard let m = c.legal.first(where: { $0.type != .wait }) else { continue }
            sawAMenu = true
            await c.apply(m)
            XCTAssertEqual(c.legal, MoveWire.decode(c.legalPacked),
                           "the two published forms of the menu disagree")
            await c.undo()
            XCTAssertEqual(c.legal, MoveWire.decode(c.legalPacked))
        }
        XCTAssertTrue(sawAMenu, "fixture: no seat ever had a move to make")
    }

    /// Undo DROPS the hold rather than releasing it: the move it belonged to is
    /// gone, so there is nothing to animate and nothing left to withhold.
    func testUndoDropsTheHold() async throws {
        let (c, _, deckBefore) = try await stagedBoutEnd()
        await c.undo()
        XCTAssertFalse(c.settlementHeld)
        XCTAssertNil(c.takeReleasedSettlement(), "an undone move must not animate its bout end")
        XCTAssertTrue(c.pending.isEmpty)
        XCTAssertEqual(c.view?.deckCount, deckBefore)
        XCTAssertFalse(c.legal.isEmpty, "undo must give the player their move back")
    }

    /// The owner's own description of what a staged bout-ender should look
    /// like: "they stage a round ending cover/pickup/good, and [it] animates
    /// JUST the cover/pickup/good".
    ///
    /// For a GOOD that is the good mark and nothing else - the table stays
    /// covered, the discard stays where it is, and the roles do not turn over
    /// until Send. All of it comes from the kernel's own pre-discard snapshot
    /// (ENGINE_HOOK_MAGIC_TRANSITION fires before anything moves), which is why
    /// the mark is on it: handle_good sets the mask before it runs the
    /// transition that clears it.
    func testAStagedGoodShowsTheGoodMarkAndKeepsTheTable() async throws {
        let (c, sealed, deckBefore) = try await stagedBoutEnd(preferring: .good)
        guard c.pending.last?.type == .good else {
            throw XCTSkip("fixture: no staged good closed a bout")
        }
        let held = try XCTUnwrap(c.view)
        XCTAssertNotEqual(held.goodMask & (1 << c.mySeat), 0,
                          "the good this player just said is not marked")
        XCTAssertFalse(held.battles.isEmpty, "the table was swept before Send")
        XCTAssertEqual(held.deckCount, deckBefore)
        let discardBefore = held.discardCount

        await c.markSent(payload: sealed)
        let after = try XCTUnwrap(c.view)
        XCTAssertTrue(after.battles.isEmpty, "Send did not sweep the table")
        XCTAssertGreaterThan(after.discardCount, discardBefore)
        XCTAssertLessThan(after.deckCount, deckBefore)
    }

    // MARK: the boundary itself

    /// Build a frame stream by hand in the shape the kernel writes: a u16 LE
    /// length, then `version, viewer, actor, n_events`, then one 9-byte event per
    /// type (7 fixed bytes, no cards, an empty snapshot), then an empty trailer.
    /// The cut is a question about TYPES and ORDER, so a stream carrying nothing
    /// else asks it as sharply as possible.
    private func frames(_ groups: [[EventType]]) -> Data {
        var out = Data()
        for g in groups {
            let flen = 4 + g.count * 9 + 2
            out.append(UInt8(flen & 0xff)); out.append(UInt8((flen >> 8) & 0xff))
            out.append(1)                       // EVWIRE_FORMAT_VERSION
            out.append(0); out.append(0)        // viewer, actor
            out.append(UInt8(g.count))
            for t in g {
                out.append(UInt8(t.rawValue))
                out.append(1)                   // seat
                out.append(0)                   // msg
                out.append(1); out.append(2)    // from, to
                out.append(0)                   // flags
                out.append(0)                   // n_cards
                out.append(0); out.append(0)    // snap_len 0
            }
            out.append(0); out.append(0)        // final_len 0
        }
        return out
    }

    /// The cut is the kernel's (evwire_frames_settlement_cut, c/src/evwire.c). A
    /// settlement is the transition, the discard, the refill and the trash
    /// sweep; a card being played is not.
    ///
    /// These are the same seven shapes `[GameEvent].settlementStart` used to be
    /// pinned on. That extension is gone - the cut moved into C, where the rule
    /// and the count-across-frames both live - so they are asked of the wire the
    /// kernel actually answers over.
    func testTheSettlementBoundaryIsTheKernels() {
        func cut(_ types: [EventType]) -> Int? { EvWire.settlementCut(frames([types])) }
        // A cover that swept the table: the cover is the player's, the rest is not.
        XCTAssertEqual(cut([.cover, .cardsToTrash, .refill]), 1)
        XCTAssertEqual(cut([.cover, .cover, .discard, .refill]), 2)
        // A pickup: the sweep into the hand is the move; the refill is not.
        XCTAssertEqual(cut([.pickup, .refill, .defenderMove]), 1)
        // A good emits no step of its own, so the whole stream is the bout end.
        XCTAssertEqual(cut([.magicTransition, .discard, .refill]), 0)
        // Ordinary play settles nothing.
        XCTAssertNil(cut([.attackPass]))
        XCTAssertNil(cut([.cover, .out]))
        XCTAssertNil(cut([]))
    }

    /// A staged turn is one frame per ACTION and the board animates the flattened
    /// list, so the cut has to count ACROSS frames. This is the half a client
    /// working from its own decoded list would get wrong for free, and the reason
    /// the question is the kernel's rather than an index computed after the fact.
    func testTheCutCountsAcrossFrames() {
        let stream = frames([[.attackPass], [.cover, .discard, .refill]])
        XCTAssertEqual(EvWire.decodeFrames(stream).count, 4, "the frames flatten to one list")
        XCTAssertEqual(EvWire.settlementCut(stream), 2,
                       "the second frame's settlement is at 2 in the flat list, not at 0")
    }

    // MARK: fixture

    /// Play a 2-player game the extension's way until some seat stages a turn
    /// that ends a bout, and stop there: the move is applied, the bubble is
    /// sealed, Send has not been pressed.
    private func stagedBoutEnd(preferring kind: MoveType? = nil) async throws
        -> (MessageTurnController, Data, Int) {
        for salt in UInt8(1)...UInt8(40) {
            var payload: Data?
            var env: MessageEnvelope?
            for _ in 0..<80 {
                var acted = false
                for seat in 0..<2 {
                    let c: MessageTurnController
                    if let p = payload, let e = env {
                        c = MessageTurnController(parentPayload: p, parent: e, mySeat: seat)
                    } else {
                        c = MessageTurnController(genesisSeed: seed(salt), players: 2,
                                                  gameId: 91, myNickname: "P\(seat)")
                    }
                    await c.begin()
                    if c.view?.isOver == true { return try fixtureFailed() }
                    // Prefer the move the caller is hunting for, so a test
                    // about one of the three bout-enders is not at the mercy of
                    // which one the run of play offers first.
                    let move: Move
                    if let kind, let want = c.legal.first(where: { $0.type == kind }) {
                        move = want
                    } else if let any = c.legal.first(where: { $0.type != .wait }) {
                        move = any
                    } else { continue }
                    guard let before = c.view else { continue }
                    let deckBefore = before.deckCount
                    await c.apply(move)
                    guard !c.pending.isEmpty else { continue }
                    let s = try await c.stagedPayload(sentAt: MessageKernel.clockNow() - 60)
                    // A bout end with cards still in the stock: a dry-deck one
                    // has nothing to withhold and would pin nothing.
                    if c.settlementHeld, deckBefore > 0,
                       kind == nil || c.pending.last?.type == kind {
                        return (c, s, deckBefore)
                    }
                    await c.markSent(payload: s)
                    payload = s
                    env = try? await MessageEnvelope.peek(payload: s)
                    acted = true
                    break
                }
                if !acted { break }
            }
        }
        return try fixtureFailed()
    }

    private func fixtureFailed() throws -> (MessageTurnController, Data, Int) {
        throw XCTSkip("fixture: no staged bout end with a live stock in 40 games")
    }
}
