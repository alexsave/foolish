// SendWindowTests — `markSent` must never leave the board describing a state
// the player has already moved past, not even for one paint.
//
// THE REPORT (owner, 1.0(23), twice). "a king of clubs was covered by a ace of
// clubs... I hit good, and it correctly only showed me the good checkmark role
// transition before I sent it. But then when I sent it, it did some weird
// animation sequence that ended up back with the ace of clubs covering the king
// of clubs again." And: "i just did a cover. whwn i sent it strangely animated
// to the state before the cover."
//
// THIS ONE IS A ROUND-22 REGRESSION, caused by the fix for the previous one.
// `publish` used to read the RESIDENT game, which still had the staged moves
// applied to it - so a controller whose `pending` had been emptied but whose
// `base` had not yet moved still rendered correctly, by accident. Round 22 made
// `base` + `pending` the single source of truth (which is right), and that
// turned the same window lethal:
//
//     pending = []                       <- @Published; a paint may land here
//     await kernel.decode(sent)          <- a suspension
//     base = .continuation(sent)
//
// Between those lines the controller says "no staged moves, on the OLD chain",
// which renders as the board BEFORE the move - and `animAtomsBefore` falls back
// to the old chain's boundary, so the board animates the PREVIOUS bubble's move
// to get there. That is the weird sequence, and it is why closing and reopening
// put it right.
//
// The decode-failure path is the same fault without any race at all: `pending`
// is cleared, the decode fails, `base` is never updated, and the board sits one
// move in the past for good.

import XCTest
@testable import FoolishKit

@MainActor
final class SendWindowTests: XCTestCase {

    private let joins = [MessageJoin(seat: 0, name: "Eva"), MessageJoin(seat: 1, name: "Alex")]
    private let zero8 = Data(repeating: 0, count: 8)

    /// A 2p board where I am the defender and can cover, plus the chain it opened.
    private func board() async throws -> (c: MessageTurnController, opened: Data) {
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
            let opened = try await k.seal(phase: 2, lastActorSeat: opener, gameId: 0xB01,
                                          parent8: zero8, joins: joins)
            let env = try await MessageEnvelope.decode(payload: opened, viewer: -1)
            let c = MessageTurnController(parentPayload: opened, parent: env, mySeat: me)
            await c.begin()
            return (c, opened)
        }
        throw XCTSkip("no 2p deal in 60 tries let the defender cover")
    }

    /// THE DETERMINISTIC HALF. The bytes handed to `markSent` cannot be decoded,
    /// so there is no new base to move to - and the board must therefore stay
    /// exactly where it is. It must NOT drop the staged move and fall back to
    /// the chain underneath it, which is the reported symptom with no race
    /// needed to produce it.
    func testASendWhoseBytesCannotBeReadLeavesTheBoardAlone() async throws {
        let (c, _) = try await board()
        let cover = try XCTUnwrap(c.legal.first { $0.type == .cover })
        await c.apply(cover)
        let shown = try XCTUnwrap(c.view)
        XCTAssertNotNil(shown.battles.first?.defense, "my cover is on the table")

        await c.markSent(payload: Data([0xde, 0xad, 0xbe, 0xef]))

        XCTAssertEqual(c.view, shown, "the board must not walk back to before my cover")
        XCTAssertNotNil(c.view?.battles.first?.defense, "…the cover is still on the table")
    }

    /// THE WINDOW. A refresh landing while `markSent` is suspended must never
    /// see the board go backwards. Run repeatedly, because which suspension the
    /// refresh lands in is the scheduler's choice - the assertion is that NO
    /// interleaving can produce a board older than the one already on screen.
    func testNoRefreshDuringASendCanSeeAnOlderBoard() async throws {
        for _ in 0..<40 {
            let (c, _) = try await board()
            let cover = try XCTUnwrap(c.legal.first { $0.type == .cover })
            await c.apply(cover)
            let shown = try XCTUnwrap(c.view)
            let mine = try await c.stagedPayload()

            let send = Task { await c.markSent(payload: mine) }
            await Task.yield()
            await c.refresh()
            let midway = c.view
            await send.value

            XCTAssertNotNil(midway?.battles.first?.defense,
                            "a refresh mid-send saw the table before my cover")
            XCTAssertEqual(midway?.discardCount, shown.discardCount,
                           "…and a board from another moment of the game")
            XCTAssertNotNil(c.view?.battles.first?.defense, "and it ends correct too")
        }
    }

    /// The BOUNDARY the board animates from has to move with the base. If
    /// `pending` empties while `baseAtomsBefore` still describes the previous
    /// bubble, the board replays THAT bubble's move - the "weird animation
    /// sequence" before the table came back wrong.
    func testTheAnimationBoundaryMovesWithTheBase() async throws {
        let (c, _) = try await board()
        let cover = try XCTUnwrap(c.legal.first { $0.type == .cover })
        await c.apply(cover)
        let staged = c.animAtomsBefore
        let mine = try await c.stagedPayload()
        await c.markSent(payload: mine)
        XCTAssertEqual(c.animAtomsBefore, staged,
                       "the sent bubble starts where the staged move did - not where the one before it did")
    }

    /// AN ARRIVAL HAS THE SAME SHAPE. `adopt` moves `base` to the arriving
    /// chain, and `begin` used to clear `pending` only after suspending on the
    /// rebuild - so in between, the controller read as "the NEW chain, with the
    /// moves I staged against the OLD one still on top", which is what the board
    /// rebuilds from, literally: stale moves replayed onto a parent they were
    /// never legal against.
    ///
    /// Asserted at the SUSPENSION rather than on what a mid-flight board looks
    /// like, because the downstream symptom depends on whether those stale moves
    /// happen to be legal on the new chain - illegal ones make the rebuild throw
    /// and the board merely freeze, which is invisible to an assertion about the
    /// view. The invariant is the thing worth pinning: this controller must
    /// never be suspended in a state that says new base, old staged moves.
    func testAnArrivalClearsStagedMovesBeforeItSuspends() async throws {
        let (c, _) = try await board()
        let cover = try XCTUnwrap(c.legal.first { $0.type == .cover })
        await c.apply(cover)
        XCTAssertEqual(c.pending.count, 1, "a move is staged against the OLD chain")

        // The opponent's reply, arriving.
        let mine = try await c.stagedPayload()
        _ = try await MessageEnvelope.decode(payload: mine, viewer: -1)
        let k = MessageKernel.shared
        let theirs = await k.residentLegal(seat: 1 - c.mySeat)
        let good = try XCTUnwrap(theirs.first { $0.type == .good },
                                 "the covered table lets the attacker say good")
        try await k.apply(seat: 1 - c.mySeat, move: good)
        let arriving = try await k.seal(phase: 2, lastActorSeat: 1 - c.mySeat,
                                        gameId: 0xB01, parent8: zero8, joins: joins)
        let env = try await MessageEnvelope.decode(payload: arriving, viewer: -1)

        let adopt = Task { await c.adopt(payload: arriving, parent: env) }
        await Task.yield()
        XCTAssertTrue(c.pending.isEmpty,
                      "the staged move must be gone before adopt suspends on the rebuild")
        await adopt.value
        XCTAssertTrue(c.pending.isEmpty, "…and stays gone")
        XCTAssertEqual(c.basePayload, arriving, "the board is on the arrived chain")
    }

    /// UNDO, same rule. It used to empty `pending`, rebuild, and re-apply the
    /// survivors one at a time; every step of that was a board a publish could
    /// land on. Undoing one of two staged moves must go straight from two to
    /// one, never through none.
    ///
    /// The DEAL is searched for: it needs an opener who can also throw a second
    /// card in, which not every hand can do. Skipping instead would make this a
    /// test that quietly does not run.
    func testUndoNeverPassesThroughAnEmptyBoard() async throws {
        let k = MessageKernel.shared
        let names3 = (0..<3).map { MessageJoin(seat: $0, name: "P\($0)") }
        var built: MessageTurnController?
        for salt in UInt8(1)...UInt8(120) {
            try await k.newGame(seed: Data(repeating: salt, count: 32), players: 3)
            var opener = -1
            for s in 0..<3 where (await k.residentLegal(seat: s)).contains(where: { $0.type == .attack }) {
                opener = s; break
            }
            guard opener >= 0 else { continue }
            let opened = try await k.seal(phase: 2, lastActorSeat: (opener + 2) % 3,
                                          gameId: 0xB02, parent8: zero8, joins: names3)
            let env = try await MessageEnvelope.decode(payload: opened, viewer: -1)
            let c = MessageTurnController(parentPayload: opened, parent: env, mySeat: opener)
            await c.begin()
            guard let a = c.legal.first(where: { $0.type == .attack }) else { continue }
            await c.apply(a)
            guard let b = c.legal.first(where: { $0.type == .attack }) else { continue }
            await c.apply(b)
            built = c
            break
        }
        let c = try XCTUnwrap(built, "no 3p deal in 120 tries let one seat stage two attacks")
        XCTAssertEqual(c.pending.count, 2)
        let twoUp = try XCTUnwrap(c.view)

        let undo = Task { await c.undo() }
        await Task.yield()
        let midway = c.pending.count
        await undo.value

        XCTAssertGreaterThanOrEqual(midway, 1, "undo must not pass through zero staged moves")
        XCTAssertEqual(c.pending.count, 1, "…and lands at one")
        XCTAssertNotEqual(c.view, twoUp, "…with the second card off the table")
    }
}
