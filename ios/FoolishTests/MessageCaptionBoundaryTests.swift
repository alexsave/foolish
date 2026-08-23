// The caption under a bubble describes THAT bubble's turn - all of it, and
// nothing before it (round 16).
//
// The owner: "sometimes the bubble caption text for just the following attack
// indicates text for both the last defense cards cover + the following attack.
// It should only be the attack after the successful cover."
//
// A caption is the kernel's own event stream for the atoms this bubble added
// (MessageKernel.lastMoveEvents over MessageEnvelope.atomsBefore), so a caption
// that names the wrong moves is a BOUNDARY that names the wrong atoms - and the
// same boundary drives what the recipient watches animate. Two things moved it:
//
//   - the composer READ its own outgoing bubble with a `decode`, which adopts,
//     so the kernel was told the staged half of a turn was somebody else's
//     history (now `peek`, which adopts nothing); and
//   - the delta itself was two atom counts subtracted, and the atom stream is
//     not append-only - a pending good stops being an atom the moment anything
//     follows it, so a turn lost one atom off its front per good it superseded
//     (now measured from the log mark, msg_wire.h's n_new).
//
// What this file pins is the property, not either mechanism: play whole games
// the way the extension plays them and check every caption against the moves
// its bubble actually carries.
import XCTest
@testable import FoolishKit

@MainActor
final class MessageCaptionBoundaryTests: XCTestCase {

    private func seed(_ salt: UInt8) -> Data {
        var d = Data(repeating: 0, count: 32)
        for i in 0..<32 { d[i] = salt &+ UInt8(truncatingIfNeeded: i * 11) }
        return d
    }

    /// EVW_MSG_* - the kernel's per-event message tag, as MessageSummary reads
    /// them. Only the headline ones: an `out` or a round transition is a
    /// consequence, not a move somebody made.
    private func headlines(_ events: [GameEvent]) -> [Int: Int] {
        var out: [Int: Int] = [:]
        for e in events where [1, 2, 4, 8].contains(e.msg) {
            // A pickup is one event; the rest count the cards they moved, so a
            // cover of three attacks is three and a caption naming one fails.
            out[e.msg, default: 0] += (e.msg == 8 ? 1 : max(1, e.cards.count))
        }
        return out
    }

    /// …and the same census taken from the moves a human staged.
    private func headlines(staged: [Move]) -> [Int: Int] {
        var out: [Int: Int] = [:]
        for m in staged {
            switch m.type {
            case .attack: out[1, default: 0] += m.cards.count
            case .pass:   out[2, default: 0] += m.cards.count
            case .cover:  out[4, default: 0] += m.cards.count
            case .pickup: out[8, default: 0] += 1
            default: break      // a good leaves no headline of its own
            }
        }
        return out
    }

    /// THE COMPOSER'S OWN CALL - `MessageSummary.forStagedBubble`, which is what
    /// `MessagesViewController.stage` invokes on the bubble it is about to put
    /// in the input field. Called for its SIDE EFFECTS as much as its answer:
    /// reading a payload must leave the kernel's base where it was, and a read
    /// that does not is only visible in what the NEXT bubble claims.
    ///
    /// The events are then asked for again, off the same boundary the line was
    /// built from, so a caption can be checked move by move rather than by
    /// string matching.
    private func caption(for sealed: Data) async throws -> (String, [GameEvent], MessageEnvelope) {
        let (env, text) = await MessageSummary.forStagedBubble(payload: sealed)
        let e = try XCTUnwrap(env, "the composer could not read its own seal")
        let events = await MessageKernel.shared.lastMoveEvents(viewer: -1, atomsBefore: e.atomsBefore)
        return (text, events, e)
    }

    // MARK: the sweep

    /// THE ONE. Whole games at 2, 3 and 4 players, played the way the extension
    /// plays them - every applied move AUTO-STAGES (board `stageNow` ->
    /// `stage`), a turn may be several actions, and Send rebases the controller
    /// onto its own bytes (`markSent`) - and every bubble's caption must
    /// describe exactly the moves that bubble carries.
    ///
    /// A sweep because the report says SOMETIMES: which turns are mis-captioned
    /// depends on what the previous bubble ended with (a pending good is the
    /// difference), and that is a census question, not a fixture one. At 932
    /// bubbles this found 294 wrong captions; the two failure shapes it reports
    /// are "a bubble that staged two covers named one" and "an attack that its
    /// own bubble's caption never mentions".
    func testEveryBubbleIsCaptionedWithExactlyItsOwnMoves() async throws {
        var wrong: [String] = []
        var bubbles = 0, multi = 0

        for players in [2, 3, 4] {
            for salt in UInt8(1)...UInt8(12) {
                var payload: Data?
                var env: MessageEnvelope?
                var rng = UInt64(salt) &* 6364136223846793005 &+ UInt64(players)
                func next() -> Int {
                    rng = rng &* 6364136223846793005 &+ 1442695040888963407
                    return Int((rng >> 33) & 0xffff)
                }

                for bubble in 0..<120 {
                    var acted = false
                    let start = next() % players
                    for k in 0..<players {
                        let seat = (start + k) % players
                        let c: MessageTurnController
                        if let p = payload, let e = env {
                            c = MessageTurnController(parentPayload: p, parent: e, mySeat: seat)
                        } else {
                            c = MessageTurnController(genesisSeed: seed(salt), players: players,
                                                      gameId: 42, myNickname: "P\(seat)")
                        }
                        await c.begin()
                        if c.view?.isOver == true { break }
                        guard var move = c.legal.first(where: { $0.type != .wait }) else { continue }

                        // One turn: this action plus 0-2 more, each of them
                        // re-staging (and so re-reading) the bubble.
                        var staged: [Move] = []
                        var sealed: Data?
                        var events: [GameEvent] = []
                        var text = ""
                        var envOut: MessageEnvelope?
                        var more = next() % 3
                        while true {
                            await c.apply(move)
                            guard !c.pending.isEmpty else { break }
                            staged.append(move)
                            // Stamped a minute ago so the pickup hold never
                            // blocks the sweep: this is about captions.
                            let s = try await c.stagedPayload(sentAt: MessageKernel.clockNow() - 60)
                            (text, events, envOut) = try await caption(for: s)
                            sealed = s
                            guard more > 0, c.view?.isOver != true,
                                  let nxt = c.legal.first(where: { $0.type != .wait }) else { break }
                            more -= 1
                            move = nxt
                        }
                        guard let s = sealed, let e2 = envOut, !staged.isEmpty else { continue }
                        bubbles += 1
                        if staged.count > 1 { multi += 1 }

                        let want = headlines(staged: staged), got = headlines(events)
                        if want != got {
                            wrong.append("p\(players) seed \(salt) b\(bubble) seat \(seat) "
                                         + "staged \(staged.map { "\($0.type)x\($0.cards.count)" }) "
                                         + "-> \(got) (want \(want)), n_new=\(e2.newAtoms) "
                                         + "turn=\(e2.turn) | \(text)")
                        }

                        await c.markSent(payload: s)
                        payload = s; env = e2; acted = true
                        break
                    }
                    if !acted { break }
                }
            }
        }

        XCTAssertGreaterThan(bubbles, 500, "the sweep never played any games")
        XCTAssertGreaterThan(multi, 100, "no bubble carried more than one action")
        XCTAssertEqual(wrong.first, nil,
                       "\(wrong.count)/\(bubbles) captions describe the wrong moves")
    }

    // MARK: the two mechanisms, each on its own

    /// A turn of two actions is captioned as two actions - even though the
    /// composer read the bubble in between, which is what the auto-stage does
    /// after every single move.
    ///
    /// The read is a `peek`. As a `decode` it re-adopts, and the second seal
    /// then measures its delta from the middle of its own turn: the caption
    /// (and the recipient's animation) keeps only the tail.
    func testReadingAStagedBubbleDoesNotShrinkTheNextOne() async throws {
        let (p0, e0, seat) = try await twoAttackChain(gameId: 4820)
        let c = MessageTurnController(parentPayload: p0, parent: e0, mySeat: seat)
        await c.begin()

        guard let first = c.legal.first(where: { $0.type == .cover && $0.cards.count == 1 }) else {
            throw XCTSkip("fixture: no cover available")
        }
        await c.apply(first)
        let staged1 = try await c.stagedPayload(sentAt: MessageKernel.clockNow() - 30)
        _ = try await caption(for: staged1)          // THE READ, as stage() makes it

        guard let second = c.legal.first(where: { $0.type == .cover }) else {
            throw XCTSkip("fixture: the second attack was not coverable")
        }
        await c.apply(second)
        let staged2 = try await c.stagedPayload(sentAt: MessageKernel.clockNow() - 30)

        let (text, events, env) = try await caption(for: staged2)
        XCTAssertEqual(headlines(events), [4: 2],
                       "the bubble carries two covers; its caption named \(text)")
        XCTAssertEqual(env.newAtoms, 2, "the bubble claimed \(env.newAtoms) atoms for two covers")

        // Both cards are named, not just the last one.
        for card in [first, second].compactMap({ $0.cards.first }) {
            XCTAssertTrue(text.contains(CardRank.label(card.v)),
                          "\(text) does not name \(card.identity)")
        }
    }

    /// A GOOD still pending on the parent chain is an atom that stops being one
    /// the moment I cover - so the atom count barely moves while my turn is
    /// real. That is the arithmetic that lost the front of a turn, and it is
    /// the case the log mark exists for.
    func testATurnThatSupersedesAPendingGoodIsCaptionedInFull() async throws {
        // Seat 0 attacks twice; seat 1 covers ONE, so seat 0 may say good with
        // the table still open - that good is the parent chain's last atom.
        let (p0, e0, def) = try await twoAttackChain(gameId: 4821)
        let atk = def == 0 ? 1 : 0

        let d = MessageTurnController(parentPayload: p0, parent: e0, mySeat: def)
        await d.begin()
        guard let cover1 = d.legal.first(where: { $0.type == .cover && $0.cards.count == 1 }) else {
            throw XCTSkip("fixture: no cover available")
        }
        await d.apply(cover1)
        let pCover = try await d.stagedPayload(sentAt: MessageKernel.clockNow() - 40)
        let eCover = try await MessageEnvelope.decode(payload: pCover, viewer: -1)

        let a = MessageTurnController(parentPayload: pCover, parent: eCover, mySeat: atk)
        await a.begin()
        guard let good = a.legal.first(where: { $0.type == .good }) else {
            throw XCTSkip("fixture: the attacker could not say good")
        }
        await a.apply(good)
        let pGood = try await a.stagedPayload(sentAt: MessageKernel.clockNow() - 30)
        let eGood = try await MessageEnvelope.decode(payload: pGood, viewer: -1)

        // …and now the defender covers the rest. The good is dead state the
        // moment this lands, so the chain grows by less than the turn is.
        let d2 = MessageTurnController(parentPayload: pGood, parent: eGood, mySeat: def)
        await d2.begin()
        guard let cover2 = d2.legal.first(where: { $0.type == .cover }) else {
            throw XCTSkip("fixture: the second attack was not coverable")
        }
        await d2.apply(cover2)
        let sealed = try await d2.stagedPayload(sentAt: MessageKernel.clockNow())

        let (text, events, env) = try await caption(for: sealed)
        XCTAssertEqual(headlines(events), [4: cover2.cards.count],
                       "the bubble carries one cover; its caption named \(text)")
        XCTAssertLessThanOrEqual(env.atomsBefore, eGood.turn,
                                 "the bubble starts past the good it superseded")
        XCTAssertFalse(text.isEmpty)
    }

    /// Being told what went out is the whole signal a rebase needs - whether or
    /// not this controller is the one that staged it.
    ///
    /// The send signal reaches whatever controller the surface is holding, and
    /// that is not always the one with the move in `pending` (a reload, an
    /// undo-to-empty re-seal, a second signal for the same send). An empty
    /// `pending` used to return before the rebase, leaving the base one bubble
    /// behind the thread - and the next bubble then claimed BOTH moves, which
    /// is the doubling the delta exists to prevent, and the caption the owner
    /// reported.
    func testASendRebasesEvenWhenNothingWasStagedHere() async throws {
        let (p0, e0, seat) = try await twoAttackChain(gameId: 4822)

        // Somebody staged and sent a cover on this chain.
        let staging = MessageTurnController(parentPayload: p0, parent: e0, mySeat: seat)
        await staging.begin()
        guard let first = staging.legal.first(where: { $0.type == .cover && $0.cards.count == 1 }) else {
            throw XCTSkip("fixture: no cover available")
        }
        await staging.apply(first)
        let sent = try await staging.stagedPayload(sentAt: MessageKernel.clockNow() - 20)

        // A DIFFERENT controller - still on the old parent, nothing pending -
        // is the one the send signal reaches.
        let live = MessageTurnController(parentPayload: p0, parent: e0, mySeat: seat)
        await live.begin()
        XCTAssertTrue(live.pending.isEmpty, "fixture: this controller staged nothing")
        await live.markSent(payload: sent)

        guard let second = live.legal.first(where: { $0.type == .cover }) else {
            throw XCTSkip("fixture: the second attack was not coverable")
        }
        await live.apply(second)
        let sealed = try await live.stagedPayload(sentAt: MessageKernel.clockNow())

        let (text, events, _) = try await caption(for: sealed)
        XCTAssertEqual(headlines(events), [4: second.cards.count],
                       "this bubble carries one cover; its caption named \(text)")
        if let card = first.cards.first {
            XCTAssertFalse(events.contains { $0.cards.contains { $0?.identity == card.identity } },
                           "the caption re-announced the cover that went out a moment ago")
        }
    }

    // MARK: fixture

    /// A 2p chain with TWO uncovered attacks on the table, and the seat that
    /// may cover them. Searched rather than hand-built: what matters is that
    /// one seat can legally take two actions in a row, which is what makes a
    /// turn wider than one atom.
    private func twoAttackChain(gameId: UInt64) async throws -> (Data, MessageEnvelope, Int) {
        for salt in UInt8(1)...UInt8(120) {
            let bytes = Data((0..<32).map { UInt8(truncatingIfNeeded: $0 &* 17 &+ Int(salt)) | 1 })
            let a = MessageTurnController(genesisSeed: bytes, players: 2,
                                          gameId: gameId, myNickname: "A")
            await a.begin()
            guard let atk1 = a.legal.first(where: { $0.type == .attack }) else { continue }
            await a.apply(atk1)
            guard let atk2 = a.legal.first(where: { $0.type == .attack }) else { continue }
            await a.apply(atk2)
            let p = try await a.stagedPayload(sentAt: MessageKernel.clockNow() - 60)
            let e = try await MessageEnvelope.decode(payload: p, viewer: -1)
            let def = e.lastActorSeat == 0 ? 1 : 0

            // Two attacks are not two COVERS: the defender must still have one
            // after playing the first, which is what makes a turn two atoms
            // wide. Checked by playing it, not by counting the menu (a
            // full-table cover is one menu entry answering both).
            let b = MessageTurnController(parentPayload: p, parent: e, mySeat: def)
            await b.begin()
            guard let one = b.legal.first(where: { $0.type == .cover && $0.cards.count == 1 })
            else { continue }
            await b.apply(one)
            if b.legal.contains(where: { $0.type == .cover }) { return (p, e, def) }
        }
        throw XCTSkip("no 2p deal in 120 tries opened with two separately coverable attacks")
    }
}
