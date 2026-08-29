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
}
