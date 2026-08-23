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
        "f7020002efcdab89674523010800000200020000000000000000ae15293755bd748b2919627cd0591ffb42d7f9b2e9b57da5c2839ed47bd7ced7020004416e6e300104416e6e310800f72719e90cb7ee031bd6af74a3a23a"

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

    /// The open-replay is now the KERNEL's evwire for the last move
    /// (MessageKernel.lastMoveEvents), resolved in begin() from the adopted chain
    /// ALONE - no `prevPayload`/"where I last looked" (owner steer: the kernel
    /// decides the group). So opening a chain yields the events of its last move,
    /// as real GameEvents whose card identities are viewer-correct.
    ///
    /// NOTE this intentionally SUPERSEDES note 13's reopen-idempotency (the old
    /// "an already-seen chain resolves to an EMPTY window"): with no client memory
    /// of what was seen, opening always animates the last move. That is the
    /// deterministic behavior the kernel-source-of-truth design implies, replacing
    /// the previous non-deterministic "plays sometimes, not always".
    func testOpeningAChainYieldsItsLastMoveEvents() async throws {
        let parentBytes = bytes(fixtureHex)
        let parent = try await MessageEnvelope.decode(payload: parentBytes, viewer: 0)

        let c = MessageTurnController(parentPayload: parentBytes, parent: parent, mySeat: 0)
        await c.begin()
        // The fixture is a real played chain, so its last move has events; each is
        // a decoded GameEvent (a kind), proving the kernel->EvWire path is wired.
        XCTAssertTrue(c.openReplayEvents.allSatisfy { $0.kind != nil },
                      "every open-replay entry decodes to a known evwire event type")
    }

    /// The first seat with a legal non-wait move on the currently-resident game,
    /// and that move — the parent must already be decoded/adopted (decode adopts).
    private func firstActingSeatAndMove(_ parent: MessageEnvelope) async -> (seat: Int, move: Move)? {
        let k = MessageKernel.shared
        for seat in 0..<parent.nPlayers {
            let menu = await k.residentLegal(seat: seat)
            if let m = menu.first(where: { $0.type != .wait }) { return (seat, m) }
        }
        return nil
    }

    /// Round-6 bug 4: after the human SENDS, the live controller must forget the
    /// staged move (`markSent`), so `canSend`/`canUndo` go false and the collapsed
    /// drawer's Undo button — which otherwise re-staged and re-sent an already-sent
    /// move — disappears. markSent leaves the move APPLIED (the board still shows
    /// the sent state), it only drops it from `pending`.
    func testMarkSentForgetsTheStagedMoveSoUndoAndSendGoAway() async throws {
        let parentBytes = bytes(fixtureHex)
        let parent = try await MessageEnvelope.decode(payload: parentBytes, viewer: 0)
        guard let (seat, move) = await firstActingSeatAndMove(parent) else {
            return XCTFail("no seat had a legal move in the fixture")
        }

        let c = MessageTurnController(parentPayload: parentBytes, parent: parent, mySeat: seat)
        await c.begin()
        await c.apply(move)
        XCTAssertEqual(c.pending.count, 1, "one action staged")
        XCTAssertTrue(c.canSend, "Undo/Send are live while staged")

        await c.markSent()
        XCTAssertTrue(c.pending.isEmpty, "markSent drops the sent move from pending")
        XCTAssertFalse(c.canSend, "so canSend (and the Undo button gated on it) go false")
        XCTAssertNotNil(c.view, "the sent move stays applied — the board still renders it")

        // Idempotent / safe when nothing is staged.
        await c.markSent()
        XCTAssertTrue(c.pending.isEmpty)
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

    // MARK: round-9 #5 — the send that replayed itself

    /// The durable just-sent marker is exact-match, and survives a MISS.
    ///
    /// ROUND 12 #11 changed this contract. It used to be consumed on every
    /// adopt, match or not, so that a stale marker could never silence a later
    /// real replay - but the marker is byte-exact, so it could only ever silence
    /// the one chain this device sealed, and that is precisely the move its
    /// owner must not be shown again. Clearing on a miss protected nothing and
    /// made the marker a one-shot anybody could spend: one unrelated adopt
    /// between the send and the reopen (a loopback delivery of my own bubble, an
    /// opponent's reply) burned it, and the reopen replayed my own move. It is
    /// now superseded only by the next send, which overwrites it.
    func testJustSentMarkerSurvivesAMissAndClearsOnAMatch() {
        let store = MessageGameStore(defaults: UserDefaults(suiteName: "test.js.\(UUID().uuidString)")!)
        let mine = Data([1, 2, 3]), other = Data([9, 9])

        XCTAssertFalse(store.consumeJustSent(matching: mine), "no marker yet")
        store.markJustSent(payload: mine)
        XCTAssertFalse(store.consumeJustSent(matching: other), "a different chain never matches")
        XCTAssertTrue(store.consumeJustSent(matching: mine),
                      "…and that miss must NOT have spent the marker")
        XCTAssertFalse(store.consumeJustSent(matching: mine), "a match clears it: one send, one quiet open")
    }

    /// A quiet open (my own just-sent chain) must produce NO open-replay - the
    /// last move on it is mine, watched live seconds ago. A normal open of the
    /// same chain still replays it (any other device, or a later revisit).
    func testJustSentReopenSuppressesTheSelfReplay() async throws {
        let parentBytes = bytes(fixtureHex)
        let parent = try await MessageEnvelope.decode(payload: parentBytes, viewer: 0)

        let loud = MessageTurnController(parentPayload: parentBytes, parent: parent, mySeat: 0)
        await loud.begin()
        XCTAssertFalse(loud.openReplayEvents.isEmpty, "a normal open replays the chain's last move")

        let quiet = MessageTurnController(parentPayload: parentBytes, parent: parent, mySeat: 0,
                                          suppressOpenReplay: true)
        await quiet.begin()
        XCTAssertTrue(quiet.openReplayEvents.isEmpty,
                      "the reopen right after MY send plays nothing back at me")
        XCTAssertNotNil(quiet.view, "the board itself still renders the sent state")
    }

    // MARK: round-8 #4 — the persisted hand arrangement

    /// The store half: a per-game arrangement round-trips, clears, and the map
    /// is capped so abandoned games (which never hit the end-of-game clear)
    /// cannot grow it forever.
    func testHandOrderStoreRoundTripClearAndCap() {
        let store = MessageGameStore(defaults: UserDefaults(suiteName: "test.ho.\(UUID().uuidString)")!)

        XCTAssertTrue(store.handOrder(gameId: "g1").isEmpty, "empty until saved")
        store.setHandOrder(["S-6", "H-10", "C-14"], gameId: "g1")
        XCTAssertEqual(store.handOrder(gameId: "g1"), ["S-6", "H-10", "C-14"])
        store.setHandOrder(["H-10", "S-6", "C-14"], gameId: "g1")
        XCTAssertEqual(store.handOrder(gameId: "g1"), ["H-10", "S-6", "C-14"],
                       "a later reorder overwrites")

        store.clearHandOrder(gameId: "g1")
        XCTAssertTrue(store.handOrder(gameId: "g1").isEmpty, "the end-of-game clear empties the row")

        // Cap: write well past it; the newest row survives, the oldest are gone.
        for i in 0..<(MessageGameStore.handOrderCap + 8) {
            store.setHandOrder(["S-\(i)"], gameId: "cap\(i)")
        }
        XCTAssertEqual(store.handOrder(gameId: "cap\(MessageGameStore.handOrderCap + 7)"), ["S-39"],
                       "the newest row is always kept")
        XCTAssertTrue(store.handOrder(gameId: "cap0").isEmpty,
                      "the oldest rows are evicted past the cap")
    }

    /// Opening a FINISHED chain drops that game's stored arrangement (the cache
    /// exists only to survive mid-game reopens); opening a live chain must not.
    func testFinishedChainClearsTheHandArrangementAndALiveOneDoesNot() async throws {
        let store = MessageGameStore(defaults: UserDefaults(suiteName: "test.hoc.\(UUID().uuidString)")!)

        // The live half first: the fixture chain is mid-game.
        let parentBytes = bytes(fixtureHex)
        let parent = try await MessageEnvelope.decode(payload: parentBytes, viewer: 0)
        let live = MessageTurnController(parentPayload: parentBytes, parent: parent,
                                         mySeat: 0, store: store)
        store.setHandOrder(["S-6", "H-10"], gameId: live.gameIdString)
        await live.begin()
        XCTAssertFalse(live.isOver)
        XCTAssertEqual(store.handOrder(gameId: live.gameIdString), ["S-6", "H-10"],
                       "a mid-game open keeps the arrangement - that is the whole point of the cache")

        // Now a finished chain: play a real game out, seal phase 3, open it.
        let k = MessageKernel.shared
        try await k.newGame(seed: Data((0..<32).map { UInt8($0 &* 7 &+ 3) | 1 }), players: 2)
        var guardN = 0
        while (await k.residentView(viewer: -1))?.isOver != true, guardN < 6000 {
            guardN += 1
            var acted = false
            for s in 0..<2 {
                let legal = await k.residentLegal(seat: s)
                if let m = legal.first(where: { $0.type != .wait }) {
                    try? await k.apply(seat: s, move: m); acted = true; break
                }
            }
            if !acted { break }
        }
        let joins = [MessageJoin(seat: 0, name: "A"), MessageJoin(seat: 1, name: "B")]
        let finished = try await k.seal(phase: 3, lastActorSeat: 0, gameId: 4242,
                                        parent8: Data(repeating: 0, count: 8), joins: joins)
        let env = try await MessageEnvelope.decode(payload: finished, viewer: -1)
        store.setHandOrder(["S-6", "H-10"], gameId: "4242")
        let over = MessageTurnController(parentPayload: finished, parent: env,
                                         mySeat: 0, store: store)
        await over.begin()
        XCTAssertTrue(over.isOver, "the played-out chain is finished")
        XCTAssertTrue(store.handOrder(gameId: "4242").isEmpty,
                      "opening a finished chain clears the arrangement")
    }
}
