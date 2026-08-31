// SendRebaseTests — what pressing Send may and may not do to the board.
//
// THE REPORT (owner, 1.0(22)). A 6 was on the table; they threw in a second 6
// and hit Send. "at the same moment the undo button disappeared... I see the 10
// of spades appear as if it were on the table and animate into my hand. The 10
// of spades was the card I picked up in the previous bout. At the same time
// another players card count goes down by one, and then after the 10 of spades
// animation, I see a single card deal to that player... the visual appearance
// ends up in the state BEFORE the 6 of clubs was thrown in. I had to close and
// reopen the imessage ext to get it back into the right state."
//
// Every detail of that is one thing: `markSent` rebased the board onto an OLDER
// chain. `base` moves back a bubble and `baseAtomsBefore` goes with it, so the
// board re-animates the PREVIOUS bubble's last move - which was their own
// pickup, hence a card flying from the table into their hand and a refill to
// the seat that drew - over the previous bubble's board. The bytes that
// actually went out were correct, which is exactly why reopening fixed it.
//
// The way in was `didStartSending`'s `payload(of: message) ?? pendingStage?
// .payload` with an unserialised `stage()` behind it (fixed in
// MessagesViewController). These pin the backstop, which is the half that
// cannot rot: Rule P is the kernel's own ordering, so a chain this board has
// already moved past can never be adopted as the one it just sent.

import XCTest
@testable import FoolishKit

@MainActor
final class SendRebaseTests: XCTestCase {

    private let joins = [MessageJoin(seat: 0, name: "Eva"), MessageJoin(seat: 1, name: "Alex")]
    private let zero8 = Data(repeating: 0, count: 8)

    /// A live 2p chain, the controller sitting at the seat that is NOT about to
    /// move, plus the payload of the bubble it opened.
    /// Not every deal lets the defender cover the opening attack, so the DEAL is
    /// searched for rather than assumed - a hard-coded seed makes this file pass
    /// or skip depending on the shuffle, and a test that skips is a test that is
    /// not run.
    private func board() async throws -> (c: MessageTurnController, opened: Data, mover: Int) {
        let k = MessageKernel.shared
        for salt in UInt8(1)...UInt8(60) {
            try await k.newGame(seed: Data(repeating: salt, count: 32), players: 2)
            var opener = -1
            var first: Move?
            for s in 0..<2 {
                if let m = (await k.residentLegal(seat: s)).first(where: { $0.type == .attack }) {
                    opener = s; first = m; break
                }
            }
            guard let atk = first else { continue }
            try await k.apply(seat: opener, move: atk)
            let me = 1 - opener
            guard (await k.residentLegal(seat: me)).contains(where: { $0.type == .cover })
            else { continue }
            let opened = try await k.seal(phase: 2, lastActorSeat: opener, gameId: 0xA11,
                                          parent8: zero8, joins: joins)
            let env = try await MessageEnvelope.decode(payload: opened, viewer: -1)
            let c = MessageTurnController(parentPayload: opened, parent: env, mySeat: me)
            await c.begin()
            return (c, opened, opener)
        }
        throw XCTSkip("no 2p deal in 60 tries let the defender cover the opening attack")
    }

    /// THE BUG. The board has moved on to its own sent chain; a second, STALE
    /// send signal then hands it the bubble it opened. It must stay where it is.
    func testASendCannotRebaseTheBoardOntoAnOlderChain() async throws {
        let (c, opened, _) = try await board()
        let cover = try XCTUnwrap(c.legal.first { $0.type == .cover }, "I can cover")
        await c.apply(cover)
        let mine = try await c.stagedPayload()
        await c.markSent(payload: mine)
        let after = try XCTUnwrap(c.view, "the board after my cover was sent")
        XCTAssertNotNil(after.battles.first?.defense, "my cover is on the table")

        // The stale signal: the chain this board opened, one bubble back.
        await c.markSent(payload: opened)

        XCTAssertEqual(c.basePayload, mine, "the board must stay on the chain it sent")
        XCTAssertEqual(c.view, after, "…and must not re-animate the bubble before it")
    }

    /// A REFUSAL MUST CHANGE NOTHING. Declining the rebase but still dropping
    /// the staged moves would walk the board back by exactly the move the player
    /// just watched themselves make - a smaller version of the same complaint.
    /// So the moves stay, and with them the board they draw.
    ///
    /// Built two chains deep by hand, because the reported shape needs a board
    /// that has already moved PAST the chain it is handed: an attack (chain A),
    /// a cover on top of it (chain B), and then the attacker staging a good on
    /// B and SEALING it. Handing that board chain A - bytes it never sealed -
    /// is the send that must be refused.
    func testARefusedSendLeavesTheStagedMoveOnTheBoard() async throws {
        let k = MessageKernel.shared
        var setup: (a: Data, b: Data, attacker: Int)?
        for salt in UInt8(1)...UInt8(60) {
            try await k.newGame(seed: Data(repeating: salt, count: 32), players: 2)
            var opener = -1
            var first: Move?
            for s in 0..<2 {
                if let m = (await k.residentLegal(seat: s)).first(where: { $0.type == .attack }) {
                    opener = s; first = m; break
                }
            }
            guard let atk = first else { continue }
            try await k.apply(seat: opener, move: atk)
            let chainA = try await k.seal(phase: 2, lastActorSeat: opener, gameId: 0xA11,
                                          parent8: zero8, joins: joins)
            let def = 1 - opener
            guard let cov = (await k.residentLegal(seat: def)).first(where: { $0.type == .cover })
            else { continue }
            try await k.apply(seat: def, move: cov)
            let chainB = try await k.seal(phase: 2, lastActorSeat: def, gameId: 0xA11,
                                          parent8: zero8, joins: joins)
            setup = (chainA, chainB, opener)
            break
        }
        let (chainA, chainB, attacker) = try XCTUnwrap(setup, "no deal gave attack-then-cover in 60 tries")

        let envB = try await MessageEnvelope.decode(payload: chainB, viewer: -1)
        let c = MessageTurnController(parentPayload: chainB, parent: envB, mySeat: attacker)
        await c.begin()
        let good = try XCTUnwrap(c.legal.first { $0.type == .good },
                                 "the table is covered, so the attacker may say good")
        await c.apply(good)
        // SEAL it, which is what gives this controller an opinion about which
        // bytes are its own - the guard abstains until it has made a chain,
        // because a reload can legitimately hand it one it did not build.
        _ = try await c.stagedPayload()
        let staged = c.pending
        let shown = try XCTUnwrap(c.view)
        XCTAssertFalse(staged.isEmpty, "a move is staged")

        c.markSending()
        await c.markSent(payload: chainA)     // not the bytes I sealed - refused

        XCTAssertEqual(c.basePayload, chainB, "the board stays on the chain it is playing")
        XCTAssertEqual(c.pending, staged, "the staged move must survive a refusal")
        XCTAssertEqual(c.view, shown, "…so the board still shows what the player played")
        XCTAssertTrue(c.sending, "nothing may be undone or re-sent while the send is unresolved")
    }

    /// The ordinary case still works, or the guard is just a way to break Send:
    /// a chain that EXTENDS the current one is adopted, and the board moves on
    /// to it.
    func testTheChainIJustSentIsAdopted() async throws {
        let (c, opened, _) = try await board()
        let cover = try XCTUnwrap(c.legal.first { $0.type == .cover })
        await c.apply(cover)
        let mine = try await c.stagedPayload()
        XCTAssertEqual(c.basePayload, opened, "still on the opened chain until Send")
        await c.markSent(payload: mine)
        XCTAssertEqual(c.basePayload, mine, "Send rebases onto the bubble that went out")
        XCTAssertTrue(c.pending.isEmpty, "…and the move is no longer staged")
    }

    /// An undo-to-empty re-seal carries NOTHING new, so it ties under Rule P
    /// rather than winning - and it must still rebase, because it is a real
    /// bubble with a real digest that the next move has to name as its parent.
    /// This is why the guard refuses only a STRICTLY worse chain.
    func testAnUndoToEmptyResealStillRebases() async throws {
        let (c, opened, _) = try await board()
        let cover = try XCTUnwrap(c.legal.first { $0.type == .cover })
        await c.apply(cover)
        await c.undo()
        XCTAssertTrue(c.pending.isEmpty, "undo-to-empty")
        let nothing = try await c.stagedPayload()
        XCTAssertNotEqual(nothing, opened, "a re-seal is new bytes, even carrying no move")
        await c.markSent(payload: nothing)
        XCTAssertEqual(c.basePayload, nothing,
                       "the bubble that went out is the parent the next move must name")
    }

    /// THE UNDO PILL. `canSend` is what draws it (MessageTableView.undoSlot), and
    /// it must go the moment the host reports the send - not after the decode
    /// that follows.
    func testTheUndoPillGoesOnTheSendSignalNotOnTheRebase() async throws {
        let (c, _, _) = try await board()
        let cover = try XCTUnwrap(c.legal.first { $0.type == .cover })
        await c.apply(cover)
        XCTAssertTrue(c.canSend, "a staged move is sendable and undoable")

        c.markSending()          // synchronous - no await, no decode
        XCTAssertFalse(c.canSend, "the pill is gone before any rebase runs")
        XCTAssertFalse(c.canStage, "…and nothing can be re-staged into the same bubble")
    }

    /// …and the flag never sticks: a send that turns out to carry nothing still
    /// gives the board back.
    func testTheSendingFlagIsAlwaysGivenBack() async throws {
        let (c, _, _) = try await board()
        c.markSending()
        await c.markSent(payload: nil)   // the early-return path
        XCTAssertFalse(c.sending, "an early return must not strand the board mid-send")
        let cover = try XCTUnwrap(c.legal.first { $0.type == .cover })
        await c.apply(cover)
        XCTAssertTrue(c.canSend, "the board is playable again")
    }

    /// A move played after the send signal also clears it - the human carried on
    /// playing, which is the round-16 "keep the drawer open" flow.
    func testPlayingOnClearsTheSendSignal() async throws {
        let (c, _, _) = try await board()
        c.markSending()
        let cover = try XCTUnwrap(c.legal.first { $0.type == .cover })
        await c.apply(cover)
        XCTAssertFalse(c.sending)
        XCTAssertTrue(c.canSend)
    }

    /// THE FALSE POSITIVE, pinned. 1.0(24) asked Rule P whether the sent chain
    /// outranked the base and refused when it did not - and a sealed CHILD can
    /// rank below its own parent. `msg_rule_p`'s comment says so outright: the
    /// atom fold supersedes pending goods, so "parent + good + cover" can seal
    /// to a LOWER turn than the parent, and only rule 4's parent-digest link
    /// orders such a pair. Caught on the arrival rig playing an ordinary
    /// pickup: base=[t6 r1] sent=[t5 r1], rank=-1, refused - after which the
    /// board kept its staged move, never released the withheld settlement, and
    /// sat stuck mid-send.
    ///
    /// So: whatever a turn seals to, THE CHAIN THIS CONTROLLER SEALED IS THE
    /// ONE IT IS SENDING. Swept over many deals and whatever move each offers,
    /// because the shapes that fold atoms are exactly the ones a hand-built
    /// fixture is least likely to contain.
    func testEverySealedChainIsAcceptedByItsOwnSend() async throws {
        let k = MessageKernel.shared
        var checked = 0
        for salt in UInt8(1)...UInt8(40) {
            try await k.newGame(seed: Data(repeating: salt, count: 32), players: 2)
            var opener = -1
            for s in 0..<2 where (await k.residentLegal(seat: s)).contains(where: { $0.type == .attack }) {
                opener = s; break
            }
            guard opener >= 0 else { continue }
            // Walk a few legal moves onto the kernel so the chain is mid-game,
            // then hand the next one to a controller and let it seal and send.
            var chain: Data?
            var actor = opener
            for _ in 0..<6 {
                let legal = await k.residentLegal(seat: actor)
                guard let m = legal.first(where: { $0.type != .wait }) else {
                    actor = 1 - actor; continue
                }
                try await k.apply(seat: actor, move: m)
                chain = try? await k.seal(phase: 2, lastActorSeat: actor, gameId: 0xC0DE,
                                          parent8: zero8, joins: joins)
                actor = 1 - actor
            }
            guard let parentPayload = chain,
                  let env = try? await MessageEnvelope.decode(payload: parentPayload, viewer: -1)
            else { continue }
            for seat in 0..<2 {
                let c = MessageTurnController(parentPayload: parentPayload, parent: env, mySeat: seat)
                await c.begin()
                guard let m = c.legal.first(where: { $0.type != .wait && $0.type != .pickup })
                else { continue }
                await c.apply(m)
                guard c.pending.count == 1, let sealed = try? await c.stagedPayload() else { continue }
                c.markSending()
                await c.markSent(payload: sealed)
                XCTAssertEqual(c.basePayload, sealed,
                               "salt \(salt) seat \(seat) playing \(m.type): a controller must "
                               + "always adopt the chain it sealed, whatever it seals to")
                XCTAssertTrue(c.pending.isEmpty, "…and the move is no longer staged")
                XCTAssertFalse(c.sending, "…and the board is playable again")
                checked += 1
            }
        }
        XCTAssertGreaterThan(checked, 20, "the sweep must actually have sent something")
    }

    /// THE FOLD ITSELF, built on purpose. `msg_rule_p`'s comment names the
    /// shape: "TWO pending goods are two atoms, and the cover that follows them
    /// supersedes both - the child seals to a turn LOWER than its parent's".
    /// Three seats, an attack left uncovered, two goods, then the defender's
    /// cover. That cover's bubble is a legitimate send whose chain ranks BELOW
    /// the chain it was built on, and the board must still adopt it.
    ///
    /// The random sweep above does not reach this - it plays whatever comes
    /// first and rarely leaves two goods pending - which is exactly why 1.0(24)
    /// shipped a guard that broke on it.
    func testACoverThatSupersedesTwoPendingGoodsIsStillMyOwnSend() async throws {
        let k = MessageKernel.shared
        let names3 = (0..<3).map { MessageJoin(seat: $0, name: "P\($0)") }
        var built: (parent: Data, defender: Int)?
        for salt in UInt8(1)...UInt8(80) {
            try await k.newGame(seed: Data(repeating: salt, count: 32), players: 3)
            var atk = -1
            for s in 0..<3 where (await k.residentLegal(seat: s)).contains(where: { $0.type == .attack }) {
                atk = s; break
            }
            guard atk >= 0 else { continue }
            let opening = await k.residentLegal(seat: atk)
            guard let a = opening.first(where: { $0.type == .attack }) else { continue }
            try await k.apply(seat: atk, move: a)
            let def = (atk + 1) % 3, third = (atk + 2) % 3
            // Two goods over an UNCOVERED table: each is a pending atom, neither
            // closes the bout.
            var goods = 0
            for s in [third, atk] {
                let legal = await k.residentLegal(seat: s)
                if let g = legal.first(where: { $0.type == .good }) {
                    try await k.apply(seat: s, move: g); goods += 1
                }
            }
            guard goods == 2 else { continue }
            guard let parent = try? await k.seal(phase: 2, lastActorSeat: atk, gameId: 0xF01D,
                                                 parent8: zero8, joins: names3) else { continue }
            let canCover = await k.residentLegal(seat: def)
            guard canCover.contains(where: { $0.type == .cover }) else { continue }
            built = (parent, def)
            break
        }
        let (parentPayload, defender) = try XCTUnwrap(built,
            "no 3p deal in 80 gave attack + two pending goods + a coverable table")

        let env = try await MessageEnvelope.decode(payload: parentPayload, viewer: -1)
        let c = MessageTurnController(parentPayload: parentPayload, parent: env, mySeat: defender)
        await c.begin()
        let cover = try XCTUnwrap(c.legal.first { $0.type == .cover }, "the defender covers")
        await c.apply(cover)
        let sealed = try await c.stagedPayload()

        // The fold, measured rather than assumed: this child really does rank
        // at or below its own parent, which is what made Rule P the wrong tool.
        let parentEnv = try await MessageKernel.shared.peek(payload: parentPayload)
        let sentEnv = try await MessageKernel.shared.peek(payload: sealed)
        XCTAssertLessThanOrEqual(sentEnv.turn, parentEnv.turn,
            "the cover superseded the pending goods, so its turn did not grow")

        c.markSending()
        await c.markSent(payload: sealed)
        XCTAssertEqual(c.basePayload, sealed, "my own send must be adopted regardless")
        XCTAssertTrue(c.pending.isEmpty)
        XCTAssertFalse(c.sending, "…and the board is not left stuck mid-send")
    }
}
