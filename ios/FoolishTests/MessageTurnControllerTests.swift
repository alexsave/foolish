// The LOCAL turn loop, end to end through the kernel — no Messages harness (that
// is the live-only part). Adopt a real mid-game bubble, let a seat play a legal
// move, and prove the sealed chain is one the same rules accept as newer than its
// parent (Rule P), then that undo rebuilds the parent exactly. If this passes,
// the only thing left untested is Apple's insert/send plumbing.
import XCTest
@testable import FoolishKit

@MainActor
final class MessageTurnControllerTests: XCTestCase {

    // 2p, turn 7, round 1 — sealed by the native kernel (the §8.2 gate fixture).
    private let fixtureHex =
        "f7020002efcdab89674523010700000200010000000000000000ae15293755bd748b2919627cd0591ffb42d7f9b2e9b57da5c2839ed47bd7ced7020004416e6e300104416e6e31070003a9cc795118a16a9edd28d516"

    private func bytes(_ hex: String) -> Data {
        var d = Data(); var i = hex.startIndex
        while i < hex.endIndex {
            let j = hex.index(i, offsetBy: 2)
            d.append(UInt8(hex[i..<j], radix: 16)!); i = j
        }
        return d
    }

    func testApplyingALegalMoveSealsAChainThatBeatsItsParent() async throws {
        let k = MessageKernel.shared
        let parentBytes = bytes(fixtureHex)

        // Adopt the parent, then find a seat with a legal move to make.
        let parent = try await MessageEnvelope.decode(payload: parentBytes, viewer: 0)
        XCTAssertEqual(parent.nPlayers, 2)

        var chosen: (seat: Int, move: Move)?
        for seat in 0..<parent.nPlayers {
            let menu = await k.residentLegal(seat: seat)
            if let atk = menu.first(where: { $0.type == .attack }) { chosen = (seat, atk); break }
            if chosen == nil, let any = menu.first(where: { $0.type != .wait }) { chosen = (seat, any) }
        }
        guard let (seat, move) = chosen else {
            return XCTFail("no seat had a legal move in the fixture")
        }

        let c = MessageTurnController(parentPayload: parentBytes, parent: parent, mySeat: seat)
        await c.refresh()
        let beforeHand = c.view?.me?.handCount ?? -1
        XCTAssertGreaterThanOrEqual(beforeHand, 0, "my seat's hand is visible")
        XCTAssertFalse(c.canSend, "nothing staged yet")

        // Play it.
        await c.apply(move)
        XCTAssertEqual(c.pending.count, 1, "one action staged")
        XCTAssertTrue(c.canSend, "Send move enables after one action")
        if move.type == .attack {
            XCTAssertEqual(c.view?.me?.handCount, beforeHand - move.cards.count,
                           "an attack leaves my hand for the table")
        }

        // Seal, and prove the sealed chain is strictly newer than its parent
        // under the SAME rule that resolves races between devices (§7.2).
        let sealed = try await c.stagedPayload()
        let pref = try await k.preferred(parentBytes, sealed)
        XCTAssertGreaterThan(pref, 0, "Rule P ranks the chain-with-my-move above the parent")

        let env = try await MessageEnvelope.decode(payload: sealed, viewer: -1)
        XCTAssertEqual(env.nPlayers, parent.nPlayers)
        XCTAssertEqual(env.joins, parent.joins, "identities survive the seal")
        XCTAssertEqual(env.lastActorSeat, seat, "the bubble records who moved")

        // Undo rebuilds the parent — state is derived, so there is nothing to
        // unwind but the replay (§10).
        await c.undo()
        XCTAssertTrue(c.pending.isEmpty)
        XCTAssertFalse(c.canSend)
        await c.refresh()
        XCTAssertEqual(c.view?.me?.handCount, beforeHand, "undo restored my hand")
    }

    /// §12 funnel, revised by batch 6 item B: the FINISHED bubble itself now
    /// links to a normal `/m/` payload (MessagesViewController.stage — a bare
    /// `replayLink` cannot be re-decoded by `payloadBytes(url:)`, so a receiver
    /// tapping it got the damaged-link screen instead of the final board). The
    /// replay funnel moved one hop out to the web `/m/` page, which decodes the
    /// FINISHED payload itself and derives the code there. What this test still
    /// proves is the underlying KERNEL capability the web page's derivation
    /// mirrors: a genesis game played to the end yields a REPLAY code
    /// (`residentReplayCode`) that decodes to a finished game, and
    /// `MessageEnvelope.replayLink` still builds the funnel URL from a code —
    /// both remain real, just no longer the bubble's own URL. Drives to
    /// completion through MessageKernel.apply — the same path a turn uses.
    func testFinishedGameProducesAReplayFunnelLink() async throws {
        let k = MessageKernel.shared
        let seed = Data((0..<32).map { UInt8(($0 &* 5 &+ 1) | 1) })
        try await k.newGame(seed: seed, players: 2)

        // Lowest-eligible-seat, first legal move, until the game is over.
        for _ in 0..<2000 {
            if let v = await k.residentView(viewer: -1), v.isOver { break }
            var applied = false
            for seat in 0..<2 {
                let legal = await k.residentLegal(seat: seat)
                if let m = legal.first(where: { $0.type != .wait }) {
                    try await k.apply(seat: seat, move: m)
                    applied = true
                    break
                }
            }
            if !applied { break }
        }
        let over = await k.residentView(viewer: -1)
        XCTAssertEqual(over?.isOver, true, "the driven game reached game-over")

        guard let code = await k.residentReplayCode() else {
            return XCTFail("a finished game must produce a replay code")
        }
        XCTAssertFalse(code.isEmpty)
        XCTAssertEqual(MessageEnvelope.replayLink(code: code).absoluteString,
                       "https://foolish.cards/" + code, "the funnel link is /<code>, not /m/")

        // The code decodes to a FINISHED game (fool known) — decode doesn't touch
        // the resident game, so this is a clean read.
        let decoded = try await EngineC().replayDecode(code: code)
        XCTAssertEqual(decoded.nPlayers, 2)
        XCTAssertTrue(decoded.isComplete, "the replay knows its fool")
    }

    /// A joiner names themselves the first time they act (§5.2): seat 0 creates a
    /// 2p game (joins=[0:Alice]); seat 1 replies and their nickname is appended, so
    /// the opponent stops rendering as "Seat 2".
    func testAJoinerAppendsTheirNicknameOnFirstReply() async throws {
        let k = MessageKernel.shared

        // Seat 0's device creates the game.
        MessageGameStore.shared.nickname = "Alice"
        let seed = Data((0..<32).map { UInt8(truncatingIfNeeded: $0 &* 11 &+ 3) | 1 })
        let creator = MessageTurnController(genesisSeed: seed, players: 2, gameId: 42, myNickname: "Alice")
        await creator.begin()
        guard let firstAttack = creator.legal.first(where: { $0.type != .wait }) else {
            return XCTFail("the creator (first attacker) has no legal move")
        }
        await creator.apply(firstAttack)
        let p0 = try await creator.stagedPayload()
        let e0 = try await MessageEnvelope.decode(payload: p0, viewer: -1)
        XCTAssertEqual(e0.joins, [MessageJoin(seat: 0, name: "Alice")], "creator seats only themselves")

        // Seat 1's device (same singleton store, different nickname) adopts + replies.
        MessageGameStore.shared.nickname = "Bob"
        let joiner = MessageTurnController(parentPayload: p0, parent: e0, mySeat: 1)
        await joiner.begin()
        guard let reply = joiner.legal.first(where: { $0.type != .wait }) else {
            return XCTFail("the joiner (defender) has no legal move")
        }
        await joiner.apply(reply)
        let p1 = try await joiner.stagedPayload()
        let e1 = try await MessageEnvelope.decode(payload: p1, viewer: -1)

        XCTAssertEqual(e1.joins.count, 2, "both seats are now named")
        XCTAssertEqual(e1.joins.first { $0.seat == 0 }?.name, "Alice", "the creator's name survived")
        XCTAssertEqual(e1.joins.first { $0.seat == 1 }?.name, "Bob", "the joiner named themselves")
    }

    /// parent8 is the first 8 bytes of the parent digest, zero-padded — the exact
    /// tag the next chain points back with (§7.4).
    func testParent8IsFirstEightDigestBytes() {
        let d = MessageTurnController.firstEight(hex: "aabbccddeeff00112233")
        XCTAssertEqual([UInt8](d), [0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00, 0x11])
        // Short/odd digests never overrun — they zero-pad to 8.
        XCTAssertEqual([UInt8](MessageTurnController.firstEight(hex: "aabb")),
                       [0xaa, 0xbb, 0, 0, 0, 0, 0, 0])
    }
}
