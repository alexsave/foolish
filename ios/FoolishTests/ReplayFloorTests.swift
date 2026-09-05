// ReplayFloorTests.swift - a bubble may not ask this board to re-animate a move
// it has already shown.

import XCTest
@testable import FoolishKit

@MainActor
final class ReplayFloorTests: XCTestCase {

    private let zero8 = Data(repeating: 0, count: 8)
    private var joins: [MessageJoin] { [MessageJoin(seat: 0, name: "A"), MessageJoin(seat: 1, name: "B")] }

    /// Build a 2p game where ONE seat makes two covers in a row, and seal it two
    /// ways: after the first cover, and after the second. The pair is the whole
    /// fixture - `oneCover` is what a healthy sender's first bubble looks like,
    /// and `bothCovers` is what the SECOND bubble looks like when that sender's
    /// own rebase failed and it re-sealed from the older base.
    private func twoCoversInARow() async throws -> (oneCover: Data, bothCovers: Data) {
        let k = MessageKernel.shared
        for salt in UInt8(1)...UInt8(80) {
            try await k.newGame(seed: Data(repeating: salt, count: 32), players: 2)
            var opener = -1
            // Two attacks, so the defender has two separate covers to make.
            var placed = 0
            for s in 0..<2 {
                if let m = (await k.residentLegal(seat: s)).first(where: { $0.type == .attack }) {
                    opener = s
                    try await k.apply(seat: s, move: m); placed += 1
                    break
                }
            }
            guard opener >= 0 else { continue }
            if let m2 = (await k.residentLegal(seat: opener)).first(where: { $0.type == .attack }) {
                try await k.apply(seat: opener, move: m2); placed += 1
            }
            guard placed == 2 else { continue }
            let def = 1 - opener
            guard let c1 = (await k.residentLegal(seat: def)).first(where: { $0.type == .cover })
            else { continue }
            try await k.apply(seat: def, move: c1)
            let oneCover = try await k.seal(phase: 2, lastActorSeat: def, gameId: 0xF10,
                                            parent8: zero8, joins: joins)
            guard let c2 = (await k.residentLegal(seat: def)).first(where: { $0.type == .cover })
            else { continue }
            try await k.apply(seat: def, move: c2)
            let bothCovers = try await k.seal(phase: 2, lastActorSeat: def, gameId: 0xF10,
                                              parent8: zero8, joins: joins)
            return (oneCover, bothCovers)
        }
        throw XCTSkip("no deal in 80 tries gave one seat two covers in a row")
    }

    /// THE BUG. `atomsBefore` is `turn - newAtoms`, and `newAtoms` is stamped by
    /// whoever SEALED the bubble - so a sender a bubble behind claims a boundary
    /// that re-includes a move this board has already animated. Owner, on two
    /// separate single-cover bubbles: "when they sent the J of spades cover, I
    /// saw the Q of hearts animate IN PARALLEL with the J of spades!"
    func testABubbleCannotAskForAMoveAlreadyAnimated() async throws {
        let (oneCover, bothCovers) = try await twoCoversInARow()
        let k = MessageKernel.shared

        let first = try await k.openChain(payload: oneCover, viewer: 0)
        let alreadyShown = first.env.turn
        XCTAssertFalse(first.events.isEmpty, "the first bubble animates its own cover")

        // Unclamped: the second bubble drags the first cover back in.
        let loose = try await k.openChain(payload: bothCovers, viewer: 0)
        let clamped = try await k.openChain(payload: bothCovers, viewer: 0, floor: alreadyShown)

        let covers = { (evs: [GameEvent]) in evs.filter { $0.kind == .cover }.count }
        XCTAssertEqual(covers(loose.events), 2,
                       "fixture check: unclamped, this bubble really does replay both covers")
        XCTAssertEqual(covers(clamped.events), 1,
                       "the board re-animated a cover it had already shown")
    }

    /// AND THE FLOOR MUST NOT EAT A COLD OPEN. A board that has adopted nothing
    /// has a floor of -1, and must still animate the whole bubble - that is
    /// "close the bubble I just sent and open it again" (owner, round 22).
    func testAColdOpenIsUnclamped() async throws {
        let (_, bothCovers) = try await twoCoversInARow()
        let k = MessageKernel.shared
        let cold = try await k.openChain(payload: bothCovers, viewer: 0, floor: -1)
        let bare = try await k.openChain(payload: bothCovers, viewer: 0)
        XCTAssertEqual(cold.events.count, bare.events.count,
                       "the default floor changed what a cold open animates")
        XCTAssertFalse(cold.events.isEmpty, "a cold open must still animate the bubble")
    }
}
