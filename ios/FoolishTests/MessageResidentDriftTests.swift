// MessageResidentDriftTests — the board on screen must describe THIS board's
// chain, not whatever game the kernel happens to be holding.
//
// THE REPORT (1.0(21), owner): "she attacked 6 of hearts, I covered 8 of hearts.
// She said good, and it went to the discard. the round transitioned, and then I
// played a 6 of Diamonds. As soon as I hit the send button, the 6 of diamonds
// somehow transformed into a 6 of hearts covered by the 8 of hearts, and the
// pickup button popped back up." And its quieter twin: "pickup button won't
// appear if attack arrives while board is open."
//
// Both are one fault. `MessageTurnController.publish` assigns its `@Published`
// properties back to back - round 16 closed that window deliberately - but it
// READS them across five separate trips into the kernel actor
// (`residentView`, `residentLegal`, `stagedAtomsBefore`, `pickupHold`,
// `residentReplayCode`). The kernel has ONE resident game and decoding ADOPTS,
// so any other task that decodes between two of those trips re-points the game
// underneath the read: the board is painted from one chain and its buttons from
// another, or the whole read lands on a chain this board is not playing.
//
// See the memory note "the resident game is one slot": `resealFromBase` fixed
// exactly this shape on the WRITE side (the phantom 8-seat table) by sealing in
// one uninterruptible actor call. These pin the READ side.

import XCTest
@testable import FoolishKit

@MainActor
final class MessageResidentDriftTests: XCTestCase {

    private let joins = [MessageJoin(seat: 0, name: "Eva"), MessageJoin(seat: 1, name: "Alex")]
    private let zero8 = Data(repeating: 0, count: 8)

    /// Seal whatever is resident as a LIVE bubble from `seat`.
    private func seal(_ seat: Int, gameId: UInt64 = 0xD01) async throws -> Data {
        try await MessageKernel.shared.seal(phase: 2, lastActorSeat: seat, gameId: gameId,
                                            parent8: zero8, joins: joins)
    }

    /// The seat that can play `type` right now, and the move. Asked, never
    /// assumed: the first attacker is whoever holds the lowest trump, so a
    /// hard-coded seat makes the test skip on half of all deals.
    private func mover(_ type: MoveType, of seats: [Int]) async -> (seat: Int, move: Move)? {
        for s in seats {
            if let m = (await MessageKernel.shared.residentLegal(seat: s)).first(where: { $0.type == type }) {
                return (s, m)
            }
        }
        return nil
    }

    /// The reported sequence, played straight through with nothing interleaved:
    /// attack, cover, good (arriving on the open board), my new attack, send.
    /// This is the CONTROL leg - it must pass both before and after the fix, or
    /// the failure below is not about drift at all.
    func testTheSequenceAloneKeepsMyAttackOnTheTable() async throws {
        let k = MessageKernel.shared
        try await k.newGame(seed: Data(repeating: 11, count: 32), players: 2)
        guard let opening = await mover(.attack, of: [0, 1]) else {
            throw XCTSkip("no seat can open this deal")
        }
        let eva = opening.seat, me = 1 - eva
        try await k.apply(seat: eva, move: opening.move)
        let attacked = try await seal(eva)
        let env = try await MessageEnvelope.decode(payload: attacked, viewer: -1)
        let c = MessageTurnController(parentPayload: attacked, parent: env, mySeat: me)
        await c.begin()

        // I cover, and send.
        let cover = try XCTUnwrap(c.legal.first { $0.type == .cover }, "the defender must be able to cover")
        await c.apply(cover)
        let covered = try await c.stagedPayload()
        await c.markSent(payload: covered)

        // Eva says good on her own device; it arrives on my open board.
        _ = try await MessageEnvelope.decode(payload: covered, viewer: -1)
        guard let good = await mover(.good, of: [eva]) else { throw XCTSkip("the cover did not offer a good") }
        try await k.apply(seat: eva, move: good.move)
        let goodPayload = try await seal(eva)
        let goodEnv = try await MessageEnvelope.decode(payload: goodPayload, viewer: -1)
        await c.adopt(payload: goodPayload, parent: goodEnv)

        // The bout closed, so I lead the next one.
        let mine = try XCTUnwrap(c.legal.first { $0.type == .attack },
                                 "the bout closed - I open the next one")
        await c.apply(mine)
        let card = try XCTUnwrap(c.view?.battles.first?.attack, "my attack is on the table")
        let sent = try await c.stagedPayload()
        await c.markSent(payload: sent)

        XCTAssertEqual(c.view?.battles.count, 1, "one bout, mine")
        XCTAssertEqual(c.view?.battles.first?.attack, card, "and it is still the card I played")
        XCTAssertFalse(c.legal.contains { $0.type == .pickup },
                       "I am the ATTACKER now - there is nothing for me to pick up")
    }

    /// THE BUG. Another task decodes a different chain - which is all a bubble
    /// snapshot, a Rule-P comparison or a surface reload is - and the very next
    /// refresh paints THAT game onto this board.
    func testARefreshDescribesMyChainEvenAfterSomethingElseDecoded() async throws {
        let k = MessageKernel.shared
        try await k.newGame(seed: Data(repeating: 23, count: 32), players: 2)
        guard let opening = await mover(.attack, of: [0, 1]) else {
            throw XCTSkip("no seat can open this deal")
        }
        let eva = opening.seat, me = 1 - eva
        try await k.apply(seat: eva, move: opening.move)
        let attacked = try await seal(eva)
        let env = try await MessageEnvelope.decode(payload: attacked, viewer: -1)
        let c = MessageTurnController(parentPayload: attacked, parent: env, mySeat: me)
        await c.begin()
        let cover = try XCTUnwrap(c.legal.first { $0.type == .cover })
        await c.apply(cover)
        let mineNow = try XCTUnwrap(c.view, "the board I am looking at")
        XCTAssertNotNil(mineNow.battles.first?.defense, "my cover is on the table")

        // A DIFFERENT game, decoded by somebody else between paints. Any second
        // chain does; a fresh 4-player deal is simply the loudest.
        try await k.newGame(seed: Data(repeating: 99, count: 32), players: 4)
        let other = try await MessageKernel.shared.seal(
            phase: 2, lastActorSeat: 0, gameId: 0xBEEF, parent8: zero8,
            joins: (0..<4).map { MessageJoin(seat: $0, name: "P\($0)") })
        _ = try await MessageEnvelope.decode(payload: other, viewer: -1)

        await c.refresh()

        XCTAssertEqual(c.view?.numPlayers, 2,
                       "my board is a 2-player game - another chain's decode must not repaint it")
        XCTAssertEqual(c.view, mineNow, "…and it is the same board it was before that decode")
    }

    /// The half that reaches the buttons: the view and the legal menu are read
    /// on separate trips into the actor, so a decode landing BETWEEN them shows
    /// one game's table with another game's moves. This is "pickup button won't
    /// appear if attack arrives while board is open" - the arrival's own decode
    /// is the interloper.
    func testTheMenuAndTheTableComeFromTheSameGame() async throws {
        let k = MessageKernel.shared
        try await k.newGame(seed: Data(repeating: 5, count: 32), players: 2)
        guard let opening = await mover(.attack, of: [0, 1]) else {
            throw XCTSkip("no seat can open this deal")
        }
        let eva = opening.seat, me = 1 - eva
        try await k.apply(seat: eva, move: opening.move)
        let attacked = try await seal(eva)
        let env = try await MessageEnvelope.decode(payload: attacked, viewer: -1)
        let c = MessageTurnController(parentPayload: attacked, parent: env, mySeat: me)
        await c.begin()
        XCTAssertTrue(c.legal.contains { $0.type == .pickup },
                      "an uncovered attack against me offers a pickup")

        // The interloper decodes a chain in which I am NOT the defender, then
        // this board refreshes. Nothing about my board changed.
        try await k.newGame(seed: Data(repeating: 77, count: 32), players: 3)
        let other = try await MessageKernel.shared.seal(
            phase: 2, lastActorSeat: 0, gameId: 0xCAFE, parent8: zero8,
            joins: (0..<3).map { MessageJoin(seat: $0, name: "Q\($0)") })
        _ = try await MessageEnvelope.decode(payload: other, viewer: -1)
        await c.refresh()

        XCTAssertTrue(c.legal.contains { $0.type == .pickup },
                      "the attack is still on my table, so the pickup is still mine to take")
        XCTAssertEqual(c.view?.numPlayers, 2, "and the table is still my 2-player game")
    }
}
