// SummarySweepTests.swift — round 16: "sometimes in the iMessage chat you just
// have 'X sent Foolish message' and that's it. I have seen this for 'good' moves
// and for 'attack' moves."
//
// That line is what Messages renders when a bubble's summaryText is missing, so
// this sweeps a whole game per seed through the EXACT path the composer takes
// (MessagesViewController.stage: decode the sealed payload, ask the kernel for
// the last move's events with viewer -1, hand them to MessageSummary.move) and
// reports every move whose caption comes back empty, or as the generic
// "tap to play" line that names no move at all.
//
// A sweep and not a fixture on purpose: the report says the bug is occasional
// and depends on WHICH move, which is exactly what a per-move census can answer
// and a hand-built state cannot.

import XCTest
@testable import FoolishKit

@MainActor
final class SummarySweepTests: XCTestCase {

    private func seed(_ salt: UInt8) -> Data {
        var d = Data(repeating: 0, count: 32)
        for i in 0..<32 { d[i] = salt &+ UInt8(truncatingIfNeeded: i * 11) }
        return d
    }

    /// The composer's own summary, for the chain `payload` describes.
    private func summaryFor(payload: Data) async throws -> String {
        let env = try await MessageEnvelope.decode(payload: payload, viewer: -1)
        let events = await MessageKernel.shared.lastMoveEvents(viewer: -1)
        let view = await MessageKernel.shared.residentView(viewer: -1)
        return MessageSummary.move(events: events, names: [0: "Alex", 1: "Ann"],
                                   view: view, actor: env.lastActorSeat)
    }

    func testEveryMoveInAWholeGameGetsACaptionThatNamesIt() async throws {
        let generic = FStrings.t("ios.msg.tap")
        var census: [String: Int] = [:]      // move type -> count of bad captions
        var examples: [String] = []
        var moves = 0
        var longest = ""

        for salt in UInt8(1)...UInt8(12) {
            // Seat 0 deals; the two seats then alternate through the kernel,
            // sealing after every single action exactly as a turn does.
            var payload: Data?
            var env: MessageEnvelope?
            var controllers: [Int: MessageTurnController] = [:]

            for step in 0..<60 {
                let seat = step % 2
                let c: MessageTurnController
                if let p = payload, let e = env {
                    c = MessageTurnController(parentPayload: p, parent: e, mySeat: seat)
                } else {
                    c = MessageTurnController(genesisSeed: seed(salt), players: 2,
                                              gameId: 42, myNickname: seat == 0 ? "Alex" : "Ann")
                }
                controllers[seat] = c
                await c.begin()
                if c.view?.isOver == true { break }
                guard let move = c.legal.first(where: { $0.type != .wait }) else {
                    // Neither seat can act only when the game is stuck; try the
                    // other seat before giving up on this seed.
                    continue
                }
                await c.apply(move)
                guard !c.pending.isEmpty else { continue }   // e.g. a held pickup
                // Stamp it a minute ago so round 16's pickup hold never blocks
                // the sweep - this test is about captions, not about timing.
                let sealed = try await c.stagedPayload(sentAt: MessageKernel.clockNow() - 60)
                let text = try await summaryFor(payload: sealed)
                moves += 1

                if text.count > longest.count { longest = text }
                let kind = "\(move.type)"
                if text.isEmpty || text == generic {
                    census[kind, default: 0] += 1
                    if examples.count < 12 {
                        examples.append("seed \(salt) step \(step) seat \(seat) \(kind): " +
                                        (text.isEmpty ? "<EMPTY>" : "<generic>"))
                    }
                }
                payload = sealed
                env = try await MessageEnvelope.decode(payload: sealed, viewer: -1)
            }
        }

        print("SUMMARY SWEEP: \(moves) moves captioned; bad by type: \(census)")
        print("SUMMARY SWEEP longest caption = \(longest.count) chars: \(longest)")
        for e in examples { print("SUMMARY SWEEP  \(e)") }
        XCTAssertGreaterThan(moves, 50, "the sweep did not actually play any games")
        XCTAssertEqual(census, [:], "moves whose bubble carries no description of itself")
    }
}
