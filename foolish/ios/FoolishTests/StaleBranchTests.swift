// A board branching off an OLD bubble is read-only (round 20).
//
// The owner: "prevent offline players from staging moves. They might be trying
// to cheat by holding an older state and branching from it instead of live
// game." A device that has already seen a newer chain for a game knows the table
// has moved past whatever old bubble is being tapped, and a move played there
// forks the thread.
//
// Two halves, and both are here because either one alone is a hole:
//   • the CONTROLLER refuses - `iCanAct` / `canStage` stand down, and `apply`
//     rejects, so a drag that never asks a button still gets nowhere;
//   • the STORE remembers - Messages cannot enumerate a transcript, so the only
//     possible answer to "is this the latest?" is a note of what has already
//     been through this device.
//
// The comparison itself (Rule P) is the kernel's and is tested there; what these
// pin is that the gate is WIRED to it and, just as importantly, that it FAILS
// OPEN. A false positive is a game nobody can play, which is a far worse defect
// than the fork it prevents.

import XCTest
@testable import FoolishKit

@MainActor
final class StaleBranchTests: XCTestCase {

    // 2p, turn 7, round 1 — the same §8.2 gate fixture the turn-loop tests use.
    private let fixtureHex =
        "f7020002efcdab89674523010800000200020000000000000000ae15293755bd748b2919627cd0591ffb42d7f9b2e9b57da5c2839ed47bd7ced7020004416e6e300104416e6e310800f72719e90cb7ee031bd6af74a3a23a"

    private func bytes(_ hex: String) -> Data {
        var d = Data(); var i = hex.startIndex
        while i < hex.endIndex {
            let j = hex.index(i, offsetBy: 2)
            d.append(UInt8(hex[i..<j], radix: 16)!); i = j
        }
        return d
    }

    /// A seat with something to do, on a freshly adopted board.
    private func liveBoard() async throws -> (MessageTurnController, Move) {
        let parentBytes = bytes(fixtureHex)
        let parent = try await MessageEnvelope.decode(payload: parentBytes, viewer: 0)
        var chosen: (seat: Int, move: Move)?
        for seat in 0..<parent.nPlayers {
            let menu = await MessageKernel.shared.residentLegal(seat: seat)
            if let m = menu.first(where: { $0.type != .wait }) { chosen = (seat, m); break }
        }
        guard let (seat, move) = chosen else {
            throw XCTSkip("no seat had a legal move in the fixture")
        }
        let c = MessageTurnController(parentPayload: parentBytes, parent: parent, mySeat: seat)
        await c.refresh()
        return (c, move)
    }

    /// The board the gate is FOR: everything it offers is withdrawn, and the
    /// move is refused even when something reaches past the buttons.
    func testAStaleBoardOffersNothingAndAcceptsNothing() async throws {
        let (c, move) = try await liveBoard()
        XCTAssertTrue(c.iCanAct, "the fixture seat can act before the gate closes")

        c.setSuperseded(true)
        XCTAssertFalse(c.iCanAct, "a branch off an old bubble may not act")
        XCTAssertFalse(c.canStage, "and it may not stage a bubble either")

        // Past the buttons - a drag, the harness, a shortcut. `apply` is the one
        // door all of them come through, and it rejects rather than staging.
        let ticks = c.rejectTick
        await c.apply(move)
        XCTAssertTrue(c.pending.isEmpty, "a stale board staged a move")
        XCTAssertGreaterThan(c.rejectTick, ticks, "the refusal must be reported, not silent")
        XCTAssertFalse(c.canSend)
    }

    /// A GENESIS whose creator holds no legal move can normally still stage (it
    /// is the only way the deal reaches the first attacker) - `canStage` says so
    /// through `!iCanAct`, which is exactly the term the gate flips. So the gate
    /// has to stand `canStage` down on its own account, or closing it would turn
    /// that branch ON and hand a stale board a Send button it never had.
    func testTheGateDoesNotAccidentallyEnableTheGenesisSend() async throws {
        let (c, _) = try await liveBoard()
        c.setSuperseded(true)
        XCTAssertFalse(c.iCanAct)
        XCTAssertFalse(c.canStage,
                       "!iCanAct must not be readable as 'a genesis with nothing to play'")
    }

    /// It comes back. The newest bubble arriving on a stale board hands it the
    /// right to play again - re-tapping must not be the only way out, or an
    /// arrival would leave the board correct and dead.
    func testTheGateOpensAgainWhenTheBoardCatchesUp() async throws {
        let (c, _) = try await liveBoard()
        c.setSuperseded(true)
        XCTAssertFalse(c.iCanAct)
        c.setSuperseded(false)
        XCTAssertTrue(c.iCanAct, "a board that caught up is playable again")
    }

    /// FAILS OPEN. A device that has never seen this game trusts the bubble it
    /// is given, which is what every build before this round did - and the new
    /// default has to stay that way, or a lost App Group would lock a thread.
    func testABoardWithNothingOnFileIsPlayable() async throws {
        let (c, _) = try await liveBoard()
        XCTAssertFalse(c.superseded, "the gate is closed only by evidence")
        XCTAssertTrue(c.iCanAct)
    }

    // MARK: - the note the gate reads

    /// The store keeps the chain per game AND per chat, and hands back exactly
    /// what it was given. It is the caller that runs Rule P (this type does no
    /// async work), so the one thing to pin here is that nothing is mangled on
    /// the way through and that a different chat cannot answer for this one.
    func testTheHighWaterMarkIsPerGameAndPerChat() {
        let store = MessageGameStore(suiteName: "cards.foolish.tests.stale")
        let a = bytes(fixtureHex)
        let b = a + Data([0xAB, 0xCD])

        XCTAssertNil(store.latestChain(gameId: "g1", chatKey: "chatA"))
        store.setLatestChain(gameId: "g1", chatKey: "chatA", payload: a)
        XCTAssertEqual(store.latestChain(gameId: "g1", chatKey: "chatA"), a,
                       "the bytes must survive the round trip - Rule P reads whole chains")
        // Another chat, and another game in the same chat, are different rows.
        XCTAssertNil(store.latestChain(gameId: "g1", chatKey: "chatB"))
        XCTAssertNil(store.latestChain(gameId: "g2", chatKey: "chatA"))

        // And it is a MARK, not a log: the newest write is what is read back.
        store.setLatestChain(gameId: "g1", chatKey: "chatA", payload: b)
        XCTAssertEqual(store.latestChain(gameId: "g1", chatKey: "chatA"), b)

        UserDefaults(suiteName: "cards.foolish.tests.stale")?
            .removePersistentDomain(forName: "cards.foolish.tests.stale")
    }
}
