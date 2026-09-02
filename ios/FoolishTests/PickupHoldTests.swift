// PickupHoldTests.swift — round 16: the defender may not pick up within 15
// seconds of an attack, so the attackers get a chance to throw more in.
//
// The RULE is C (msg_pickup_hold_remaining, pinned mutation-by-mutation in
// c/tests/msg_wire_test.c). What this file pins is the half C cannot see: that
// the clock actually rides the wire out of a real seal and back in through a
// real decode, that the controller asks about the chain it is playing on, and
// that a held pickup is refused rather than merely un-drawn.

import XCTest
@testable import FoolishKit

@MainActor
final class PickupHoldTests: XCTestCase {

    private func freshSeed(_ salt: UInt8) -> Data {
        var d = Data(repeating: 0, count: 32)
        for i in 0..<32 { d[i] = salt &+ UInt8(i &* 7) }
        return d
    }

    /// A 2-player chain where seat 0 has attacked and seat 1 is the defender
    /// looking at it — the exact situation the hold exists for.
    private func attackedDefender(_ salt: UInt8 = 20) async throws
        -> (payload: Data, env: MessageEnvelope) {
        for s in salt...(salt &+ 40) {
            let creator = MessageTurnController(genesisSeed: freshSeed(s), players: 2,
                                                gameId: 77, myNickname: "A")
            await creator.begin()
            guard let attack = creator.legal.first(where: { $0.type == .attack }) else { continue }
            await creator.apply(attack)
            let p = try await creator.stagedPayload()
            let e = try await MessageEnvelope.decode(payload: p, viewer: -1)
            return (p, e)
        }
        throw XCTSkip("no 2p deal in the search made seat 0 the first attacker")
    }

    // MARK: the clock on the wire

    func testASealStampsTheClockAndItSurvivesTheRoundTrip() async throws {
        let (_, env) = try await attackedDefender()
        XCTAssertNotEqual(env.sentAt, 0, "a sealed bubble carries no send clock")
        XCTAssertEqual(env.sentAt, env.sentAt & 0xffff, "the clock is not 16-bit")
        // Within a minute of this machine's own clock, in the same wrapped unit.
        let now = MessageKernel.clockNow()
        let delta = Int(UInt16(truncatingIfNeeded: now - env.sentAt))
        XCTAssertLessThan(delta, 60, "the stamp is not this device's clock (delta \(delta)s)")
    }

    /// BACKWARD COMPATIBILITY, which is the whole reason format 2 still decodes:
    /// a bubble sealed by a build that predates the clock must still open, and
    /// must hold nobody.
    func testAClocklessChainStillOpensAndHoldsNobody() async throws {
        var found: MessageTurnController?
        for s in UInt8(20)...UInt8(60) {
            let c = MessageTurnController(genesisSeed: freshSeed(s), players: 2,
                                          gameId: 77, myNickname: "A")
            await c.begin()
            if c.legal.contains(where: { $0.type == .attack }) { found = c; break }
        }
        let creator = try XCTUnwrap(found, "no 2p deal made seat 0 the first attacker")
        await creator.apply(try XCTUnwrap(creator.legal.first { $0.type == .attack }))

        // Seal with NO clock — byte for byte what 1.0(15) emits.
        let payload = try await creator.stagedPayload(sentAt: 0)
        let env = try await MessageEnvelope.decode(payload: payload, viewer: -1)
        XCTAssertEqual(env.sentAt, 0, "an unstamped seal produced a clock")

        let defender = MessageTurnController(parentPayload: payload, parent: env, mySeat: 1)
        await defender.begin()
        XCTAssertEqual(defender.pickupHold, 0,
                       "an old bubble held the defender - old clients would be unplayable")
    }

    // MARK: the hold, through the controller

    func testTheDefenderIsHeldRightAfterAnAttack() async throws {
        let (payload, env) = try await attackedDefender()
        let defender = MessageTurnController(parentPayload: payload, parent: env, mySeat: 1)
        await defender.begin()
        XCTAssertGreaterThan(defender.pickupHold, 0, "the defender was not held after a fresh attack")
        XCTAssertLessThanOrEqual(defender.pickupHold, 15, "held longer than the rule allows")
    }

    /// The ATTACKER is not held: they cannot pick up at all, and a hold on them
    /// would be a bug that only ever showed up as a missing button.
    func testTheAttackerIsNotHeld() async throws {
        let (payload, env) = try await attackedDefender()
        let attacker = MessageTurnController(parentPayload: payload, parent: env, mySeat: 0)
        await attacker.begin()
        XCTAssertEqual(attacker.pickupHold, 0, "the attacking seat was held")
    }

    /// Owner: "if the player chooses a card in the mean time, this shouldn't
    /// cause any issues. The timer should just not do anything." Covering ends
    /// the hold, because the last action is no longer an attack.
    func testCoveringEndsTheHold() async throws {
        let (payload, env) = try await attackedDefender()
        let defender = MessageTurnController(parentPayload: payload, parent: env, mySeat: 1)
        await defender.begin()
        try XCTSkipIf(defender.pickupHold == 0, "fixture: this deal did not hold")
        guard let cover = defender.legal.first(where: { $0.type == .cover }) else {
            throw XCTSkip("this deal gave the defender nothing to cover with")
        }
        await defender.apply(cover)
        XCTAssertEqual(defender.pickupHold, 0, "still held after covering the attack")
    }

    /// The kernel half of "guarded by the kernel, as well as by the UI": the
    /// move is refused even when nothing drew a button.
    func testAHeldPickupIsRefusedNotJustHidden() async throws {
        let (payload, env) = try await attackedDefender()
        let defender = MessageTurnController(parentPayload: payload, parent: env, mySeat: 1)
        await defender.begin()
        try XCTSkipIf(defender.pickupHold == 0, "fixture: this deal did not hold")

        let ticks = defender.rejectTick
        await defender.apply(Move(type: .pickup, cards: []))
        XCTAssertTrue(defender.pending.isEmpty, "a held pickup was staged anyway")
        XCTAssertEqual(defender.rejectTick, ticks + 1, "a held pickup was silently dropped")
        XCTAssertGreaterThan(defender.pickupHold, 0, "the refusal cleared the hold")
    }
}
