// MessageCaptionActorTests - a bubble's summary line names the seat that SEALED
// it, and never the seat that sealed the one before it.
//
// WHY THIS FILE EXISTS. A store shoot filmed a five-message chain whose
// collapsed caption lines each appeared to describe the PREVIOUS message's
// move: a bubble sealed by seat 1 read "SEATZERO covers 10 of C with 8 of D",
// the next one sealed by seat 0 read "SEATONE attacks with 8 of C", and so on
// down the transcript. In a 2-player game that is not a cosmetic slip - the
// line names the wrong PERSON on every turn - so it had to be settled before
// the next build went up.
//
// It is settled here rather than from frames because the frames could not
// settle it. The one thing in the same picture that looked like a control - the
// layout caption row, "SEATZERO is defending" - is not one: a defender is
// constant for the whole of a bout, so that row is the SAME SENTENCE for a
// bubble and its predecessor and cannot tell a one-bubble lag from a correct
// frame. `testTheDefenderRowCannotDetectAOneBubbleLag` pins exactly that, so
// nobody spends another shoot trusting it.
//
// WHAT IS UNDER TEST is the composer's own path and nothing built for the
// occasion: `MessageKernel.publicRead` (the single read
// `MessagesViewController.stage` makes) and `MessageSummary.move` /
// `MessageSummary.line` over what it hands back. The chain is PLAYED, bubble by
// bubble, the way two devices play one - each seat adopts the parent, moves,
// and seals - so the property held is a property of the kernel's own delta
// (`MessageEnvelope.atomsBefore`), not of a state a test thought to build.
//
// The names are SEAT-ENCODING on purpose. "SEATZERO" at seat 0 and "SEATONE" at
// seat 1 mean a sentence naming the wrong seat says so in as many words, in the
// failure message, instead of leaving a reader to map two first names onto two
// chairs - which is how the original report stayed ambiguous for three rounds.
import XCTest
@testable import FoolishKit

@MainActor
final class MessageCaptionActorTests: XCTestCase {

    private static let seatNames = ["SEATZERO", "SEATONE"]

    private func seed(_ salt: UInt8) -> Data {
        var d = Data(repeating: 0, count: 32)
        for i in 0..<32 { d[i] = salt &+ UInt8(truncatingIfNeeded: i * 11) }
        return d
    }

    /// One bubble of a played chain: the bytes, and what the chain itself says
    /// about them.
    private struct Bubble {
        var payload: Data
        var env: MessageEnvelope
        /// The seat whose device sealed it - `env.lastActorSeat`, kept beside it
        /// so an assertion can compare the two rather than assume they agree.
        var actor: Int
    }

    /// Play `count` CONSECUTIVE bubbles of one 2-player game, alternating
    /// devices, exactly as `c/tests/msg_wire_test --chain 2 <count> <depth>`
    /// does on the rig: every entry carries one seat's own move and is sealed
    /// against the log mark taken BEFORE that move, so a bubble's delta covers
    /// its own move and nothing earlier.
    private func chain(salt: UInt8, count: Int) async throws -> [Bubble] {
        var out: [Bubble] = []
        var parent: (payload: Data, env: MessageEnvelope)?

        for step in 0..<(count * 6) {
            if out.count >= count { break }
            let seat = step % 2
            // The seat's OWN device: `sealJoins` appends this nickname the first
            // time that seat acts, which is what puts both seat-encoding names
            // on the wire (§5.2).
            MessageGameStore.shared.nickname = Self.seatNames[seat]
            let c: MessageTurnController
            if let p = parent {
                c = MessageTurnController(parentPayload: p.payload, parent: p.env, mySeat: seat)
            } else {
                c = MessageTurnController(genesisSeed: seed(salt), players: 2, gameId: 77,
                                          myNickname: Self.seatNames[0])
            }
            await c.begin()
            if c.view?.isOver == true { break }
            guard let move = c.legal.first(where: { $0.type != .wait }) else {
                // A GENESIS this seat cannot move from is the end of this seed,
                // not a step to retry. `MessageTurnController(genesisSeed:)`
                // hard-codes `mySeat = 0` (the creator's chair is the only one
                // that exists yet), so a deal whose first attacker is seat 1
                // gives seat 0 nothing but `wait` forever and the walk spins
                // until its step budget runs out with an empty chain. Roughly
                // half of all deals are like that - the opener is the lowest
                // trump, which belongs to whichever seat was dealt it.
                if parent == nil { return [] }
                continue
            }
            await c.apply(move)
            guard !c.pending.isEmpty else { continue }
            // A minute ago, so round 16's pickup hold never gates the walk -
            // this file is about who a line names, not about timing.
            let sealed = try await c.stagedPayload(sentAt: MessageKernel.clockNow() - 60)
            let env = try await MessageEnvelope.decode(payload: sealed, viewer: -1)
            out.append(Bubble(payload: sealed, env: env, actor: env.lastActorSeat))
            parent = (sealed, env)
        }
        return out
    }

    /// The seat's name as the chain itself spells it, or the numbered fallback -
    /// `MessageSummary`'s own rule, restated so a failure message can print the
    /// string the product would have printed.
    private func name(_ seat: Int, _ env: MessageEnvelope) -> String {
        env.joins.first { $0.seat == seat }?.name
            ?? FStrings.t("ios.msg.seatn", ["n": "\(seat + 1)"])
    }

    /// THE REPORT, offline: every bubble's summary names the seat that sealed
    /// THAT bubble.
    ///
    /// Two assertions per bubble, and the first is the one the filmed lag would
    /// have tripped: the kernel's delta for this payload carries only the
    /// actor's own headline events, and the sentence built from them names the
    /// actor and does NOT name the other player.
    ///
    /// Only the PRIMARY beat is held to the second rule. A bubble may legally
    /// name somebody else in its tail - a seat going out, the next round's
    /// first attacker - and those are facts about consequences, not a claim
    /// about who moved.
    func testEveryBubbleSummaryNamesItsOwnActor() async throws {
        var checked = 0
        var wrong: [String] = []

        // MORE SALTS THAN NEEDED, and a floor on how many WORKED rather than a
        // demand that each one does. About half of all deals open from seat 1,
        // which the creator's device cannot play (see `chain`), so requiring
        // every seed to yield a chain fails on the deal rather than on the
        // property - while accepting "some seed worked" would let a walk that
        // silently stopped building anything pass in silence. Both floors are
        // asserted.
        var walked = 0
        for salt in UInt8(1)...UInt8(16) {
            let bubbles = try await chain(salt: salt, count: 8)
            if bubbles.count < 5 { continue }
            walked += 1

            for (i, b) in bubbles.enumerated() {
                // The composer's own read: ONE decode, and the events, view and
                // envelope all come back from it (MessageSummary.forStagedBubble
                // makes this same call).
                let read = try await MessageKernel.shared.publicRead(payload: b.payload)
                let names = Dictionary(read.env.joins.map { ($0.seat, $0.name) },
                                       uniquingKeysWith: { a, _ in a })
                checked += 1

                // 1. THE DELTA. Every headline event (EVW_MSG_ attacked / passed
                //    / covered / pickup) in this bubble's own window belongs to
                //    the seat the envelope names. A window that reached back one
                //    move would carry the other seat's headline, which is
                //    precisely what the transcript looked like.
                for e in read.events where [1, 2, 4, 8].contains(e.msg) {
                    guard e.seat != b.actor else { continue }
                    wrong.append("seed \(salt) bubble \(i): a msg-\(e.msg) event by seat " +
                                 "\(e.seat) in a bubble sealed by seat \(b.actor)")
                }

                // 2. THE SENTENCE. Built the way `MessageSummary.line` builds it
                //    for a live bubble, from the same read.
                let text = MessageSummary.move(events: read.events, names: names,
                                               view: read.view, actor: b.actor,
                                               addedNothing: read.env.addedNothing)
                let primary = text.components(separatedBy: " · ").first ?? text
                let mine = name(b.actor, read.env)
                let theirs = name(1 - b.actor, read.env)
                if !primary.contains(mine) {
                    wrong.append("seed \(salt) bubble \(i): \"\(primary)\" does not name " +
                                 "\(mine), the seat that sealed it (seat \(b.actor))")
                }
                if mine != theirs, primary.contains(theirs) {
                    wrong.append("seed \(salt) bubble \(i): \"\(primary)\" names \(theirs), " +
                                 "but seat \(b.actor) (\(mine)) sealed it")
                }
            }
        }

        XCTAssertGreaterThan(walked, 3, "too few deals opened from the creator's seat to walk")
        XCTAssertGreaterThan(checked, 25, "the walk did not actually build any chains")
        XCTAssertEqual(wrong, [], "bubbles whose line describes somebody else's move")
    }

    /// …and the same line, run over the WHOLE composer path (`line`, which is
    /// what `forStagedBubble` returns), still never opens with the other seat.
    ///
    /// Separate from the test above because `line` is the function the bubble
    /// actually ships: it routes lobby, result-card and live bubbles before
    /// `move` ever sees them, and a routing mistake there would name a seat with
    /// a perfectly correct `move` underneath it.
    func testTheComposerLineNamesItsOwnActor() async throws {
        var wrong: [String] = []
        var checked = 0

        var walked = 0
        for salt in UInt8(30)...UInt8(45) {
            let bubbles = try await chain(salt: salt, count: 8)
            if bubbles.count < 5 { continue }
            walked += 1
            for b in bubbles {
                let read = try await MessageKernel.shared.publicRead(payload: b.payload)
                let text = MessageSummary.line(env: read.env, view: read.view,
                                               events: read.events, leftName: nil)
                let primary = text.components(separatedBy: " · ").first ?? text
                let mine = name(b.actor, read.env)
                let theirs = name(1 - b.actor, read.env)
                checked += 1
                if !primary.contains(mine) {
                    wrong.append("seed \(salt): \"\(primary)\" does not name \(mine)")
                }
                if mine != theirs, primary.contains(theirs) {
                    wrong.append("seed \(salt): \"\(primary)\" names \(theirs), not \(mine)")
                }
            }
        }

        XCTAssertGreaterThan(walked, 3, "too few deals opened from the creator's seat to walk")
        XCTAssertGreaterThan(checked, 15, "the walk did not actually build any chains")
        XCTAssertEqual(wrong, [], "composer lines that describe somebody else's move")
    }

    /// THE OTHER HALF OF THE DIAGNOSIS, and the reason the report survived three
    /// rounds of screenshots: the caption ROW is not a control.
    ///
    /// "SEATZERO is defending" is the same sentence for a bubble and for the one
    /// before it whenever both sit in the same bout - which consecutive bubbles
    /// usually do, since a bout is an attack, a cover, a throw-in, a cover. So a
    /// frame in which the caption row looks right and the summary line looks a
    /// move behind is EXACTLY what a WHOLE-bubble lag produces, and reading that
    /// row as evidence that only the line lagged is reading nothing at all.
    ///
    /// Pinned so the next person to photograph a chain is told this by a test
    /// rather than by a fourth ambiguous shoot.
    func testTheDefenderRowCannotDetectAOneBubbleLag() async throws {
        var blindPairs = 0
        var pairs = 0

        for salt in UInt8(40)...UInt8(45) {
            let bubbles = try await chain(salt: salt, count: 8)
            guard bubbles.count > 1 else { continue }
            for i in 1..<bubbles.count {
                let prev = try await MessageKernel.shared.publicRead(payload: bubbles[i - 1].payload)
                let here = try await MessageKernel.shared.publicRead(payload: bubbles[i].payload)
                guard here.env.phase == 2, prev.env.phase == 2 else { continue }
                pairs += 1
                let a = MessageSummary.caption(env: prev.env, view: prev.view)
                let b = MessageSummary.caption(env: here.env, view: here.view)
                // Different sealers, and yet the row under the picture says the
                // same thing about both.
                if a == b, bubbles[i - 1].actor != bubbles[i].actor { blindPairs += 1 }
            }
        }

        XCTAssertGreaterThan(pairs, 10, "the walk did not actually build any chains")
        XCTAssertGreaterThan(blindPairs, 0,
            "the defender caption row was expected to be blind to a one-bubble lag " +
            "across at least one consecutive pair - if this ever fails the row has " +
            "become a usable control and the note above should be revised")
    }
}
