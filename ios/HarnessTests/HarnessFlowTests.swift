// HarnessFlowTests — the FoolishHarness game loop, driven headlessly.
//
// The harness is "just code": HarnessModel is the fake Messages host (transcript,
// staged bubble, which participant I am), and MessageTurnController + MessageKernel
// are the real engine. So a whole iMessage game can be played WITHOUT a simulator
// tap — which is what these tests do. Two things are proven here:
//
//   1. Staging a move never shows "This game link is damaged" (the B4 bug).
//   2. A full 2-player game plays to game-over while switching between players.
//
// SEAT-IDENTITY MOCK: on a real device SeatIdentity.resolve infers "which seat am
// I" from (cache, senderIsLocal, lastActorSeat). Here the test IS the director, so
// it assigns seats directly — participant index == seat in a filled game — and
// hands each MessageTurnController its `mySeat`. No inference, no ambiguity.

import XCTest
import Foundation
@testable import FoolishHarness
import FoolishKit

@MainActor
final class HarnessFlowTests: XCTestCase {

    // Mirror of GameSurface.load()'s first fork (FoolishKit/Messages/MessagesRootView.swift):
    //   startNewGame          -> New game setup
    //   else no payloadURL    -> "This game link is damaged"
    //   else                  -> load + board
    // GameSurface only re-runs this fork when `viewKey` changes — the harness keys
    // MessagesRootView with `.id(model.viewKey)`. So staging must NOT move viewKey,
    // or a live board is destroyed and reloaded straight into this fork.
    enum Screen: Equatable { case setup, damaged, board }
    func screen(startNewGame: Bool, payloadURL: URL?) -> Screen {
        if startNewGame { return .setup }
        return payloadURL == nil ? .damaged : .board
    }
    func screen(_ m: HarnessModel) -> Screen { screen(startNewGame: m.startNewGame, payloadURL: m.payloadURL) }

    // MARK: kernel probes (decode re-adopts the resident game; reads are spectator)

    private let kernel = MessageKernel.shared

    private func isOver(_ payload: Data) async -> Bool {
        _ = try? await kernel.decode(payload: payload, viewer: -1)
        return await kernel.residentView(viewer: -1)?.isOver ?? false
    }

    /// The seat with a real move on `payload` — who the human would switch to next.
    /// This is the seat-identity mock: seats are read straight off the kernel menu.
    private func actionableSeat(_ payload: Data, players n: Int) async -> Int? {
        _ = try? await kernel.decode(payload: payload, viewer: -1)
        for s in 0..<n where await kernel.residentLegal(seat: s).contains(where: { $0.type != .wait }) {
            return s
        }
        return nil
    }

    /// Play one turn as `seat` on `parent`, exactly as MessageTableView does: adopt
    /// the chain, apply the first real legal move if I can act, then seal the staged
    /// bubble. Returns the sealed payload, or nil if this seat has nothing to stage.
    private func stageTurn(parent: Data, seat: Int) async throws -> Data? {
        let env = try await MessageEnvelope.decode(payload: parent, viewer: -1)
        let c = MessageTurnController(parentPayload: parent, parent: env, mySeat: seat)  // mock identity
        await c.begin()
        if c.isOver { return nil }
        if c.iCanAct, let mv = c.legal.first(where: { $0.type != .wait }) { await c.apply(mv) }
        guard c.canStage else { return nil }
        return try await c.stagedPayload()
    }

    private func bytes(_ url: URL?) -> Data {
        guard let url, let b = try? MessageEnvelope.payloadBytes(url: url) else { return Data() }
        return b
    }

    /// Deal a fresh 2p genesis as "You" (seat 0), stage the opening (a move if I'm
    /// the first attacker, else the deal to hand on) and deliver it into `m`.
    private func kickoff(_ m: HarnessModel, seed: UInt8, gameId: UInt64) async throws {
        let g = MessageTurnController(genesisSeed: Data(repeating: seed, count: 32),
                                      players: 2, gameId: gameId, myNickname: "You")
        await g.begin()
        if g.iCanAct, let mv = g.legal.first(where: { $0.type != .wait }) { await g.apply(mv) }
        XCTAssertTrue(g.canStage, "a 2p genesis is always stageable (a move, or the deal to send on)")
        let deal = try await g.stagedPayload()

        // The board auto-stages -> model.stage. THE FIX under test: this must not
        // move viewKey, or `.id(model.viewKey)` tears the live board down.
        let keyBeforeStage = m.viewKey
        m.stage(deal, seat: 0)
        XCTAssertNotNil(m.staged, "the blue Send arrow should light after a move stages")
        XCTAssertEqual(m.viewKey, keyBeforeStage,
                       "staging must not change viewKey (pre-fix it flipped startNewGame)")
        m.deliver()
    }

    // MARK: tests

    /// Reproduce the "This game link is damaged" board, then prove the fix.
    func test_stagingDoesNotDamageTheBoard() async throws {
        // The damaged screen IS the (startNewGame=false, payloadURL=nil) pair. The
        // B4 bug reached it because stage() flipped startNewGame to false while the
        // transcript was still empty (nothing delivered -> payloadURL nil), and the
        // viewKey change forced a reload into exactly this fork.
        XCTAssertEqual(screen(startNewGame: false, payloadURL: nil), .damaged)

        let m = HarnessModel(count: 2)
        XCTAssertEqual(screen(m), .setup, "a fresh launch shows New game")

        try await kickoff(m, seed: 7, gameId: 0xF00D)

        // After delivery the game is real: startNewGame cleared and a bubble exists,
        // so the reload loads the board instead of the damaged fork.
        XCTAssertEqual(screen(m), .board, "the delivered bubble must load, not damage")
        XCTAssertNil(m.staged, "delivering consumes the staged bubble")
    }

    /// The precise mechanism guard: staging must leave viewKey untouched so the live
    /// board is never torn down. (This is the assertion that fails against the old
    /// code and passes against the fix.)
    func test_stagingLeavesViewKeyStable() async throws {
        let m = HarnessModel(count: 2)
        let key = m.viewKey                       // "0-0-true" on a fresh 2p launch

        let g = MessageTurnController(genesisSeed: Data(repeating: 3, count: 32),
                                      players: 2, gameId: 0xCAFE, myNickname: "You")
        await g.begin()
        if g.iCanAct, let mv = g.legal.first(where: { $0.type != .wait }) { await g.apply(mv) }
        m.stage(try await g.stagedPayload(), seat: 0)

        XCTAssertEqual(m.viewKey, key, "a staged move must not alter the harness view identity")
        XCTAssertEqual(screen(startNewGame: m.startNewGame, payloadURL: m.payloadURL), .setup,
                       "startNewGame still true: the live board persists via @State, not a reload")
    }

    /// Switching to a player who has no bubble to open yet must NOT show the
    /// damaged board — there is simply nothing delivered for them to read. (This is
    /// the "game link is broken" a human hits by tapping the other player before the
    /// first move is ever sent.)
    func test_switchingBeforeAnyDeliveryIsNotDamaged() async throws {
        let m = HarnessModel(count: 2)
        XCTAssertTrue(m.transcript.isEmpty)

        m.become(1)                                     // tap "Vera" before anything is sent
        XCTAssertNotEqual(screen(m), .damaged,
                          "no delivered bubble yet -> New game, never a damaged link")

        // Same after You stages a move but never delivers it: switching away drops
        // the undelivered draft, and Vera still has nothing to open.
        let m2 = HarnessModel(count: 2)
        let g = MessageTurnController(genesisSeed: Data(repeating: 5, count: 32),
                                      players: 2, gameId: 0xABCD, myNickname: "You")
        await g.begin()
        if g.iCanAct, let mv = g.legal.first(where: { $0.type != .wait }) { await g.apply(mv) }
        m2.stage(try await g.stagedPayload(), seat: 0)  // staged, NOT delivered
        m2.become(1)
        XCTAssertTrue(m2.transcript.isEmpty, "nothing was delivered")
        XCTAssertNotEqual(screen(m2), .damaged, "an undelivered draft must not damage the switch")
    }

    /// The gate that decides whether to ask a joiner for their name: a fresh store
    /// has none, a set one is remembered so later games never re-ask (§B3).
    func test_hasSetNickname_defaultVsChosen() {
        let s = MessageGameStore(suiteName: "fmsg.test.\(UUID().uuidString)")
        XCTAssertFalse(s.hasSetNickname, "a fresh device has no chosen name -> ask once")
        s.nickname = "Vera"
        XCTAssertTrue(s.hasSetNickname, "once chosen, the name is remembered (no re-ask)")
    }

    /// The name a joiner (like 2p Vera) chooses must be sealed into the game's
    /// `joins`, so the other players see "Vera", not "Me"/"Seat 2". This is the
    /// propagation the name gate feeds: gate -> store nickname -> sealJoins.
    func test_joinerChosenNameIsSealedIntoJoins() async throws {
        // "You" (named) deals the genesis and stages the opening.
        let you = MessageGameStore(suiteName: "fmsg.test.\(UUID().uuidString)")
        you.nickname = "You"; MessageGameStore.shared = you
        let g = MessageTurnController(genesisSeed: Data(repeating: 9, count: 32),
                                      players: 2, gameId: 0x1234, myNickname: "You")
        await g.begin()
        if g.iCanAct, let mv = g.legal.first(where: { $0.type != .wait }) { await g.apply(mv) }
        var latest = try await g.stagedPayload()

        // Seat 1 is "Vera"; play until she seals a bubble, then check her name is in it.
        let vera = MessageGameStore(suiteName: "fmsg.test.\(UUID().uuidString)")
        vera.nickname = "Vera"

        var veraSealed = false
        for _ in 0..<20 {
            guard let s = await actionableSeat(latest, players: 2) else { break }
            MessageGameStore.shared = (s == 1) ? vera : you   // seal uses the actor's own store
            guard let next = try await stageTurn(parent: latest, seat: s) else { break }
            latest = next
            if s == 1 {
                let env = try await MessageEnvelope.decode(payload: latest, viewer: -1)
                XCTAssertTrue(env.joins.contains { $0.seat == 1 && $0.name == "Vera" },
                              "seat 1's chosen name must be sealed into joins")
                veraSealed = true
                break
            }
        }
        XCTAssertTrue(veraSealed, "seat 1 should get a turn to seal her name")
    }

    /// Play a whole 2-player game by switching between the two players every turn,
    /// asserting the board never damages and the game actually ends.
    func test_fullGame_switchingPlayers_reachesGameOver() async throws {
        let m = HarnessModel(count: 2)
        try await kickoff(m, seed: 11, gameId: 0xBEEF)

        var ended = false
        var moves = 0
        for turn in 0..<400 {
            let latest = bytes(m.payloadURL)
            XCTAssertFalse(latest.isEmpty, "turn \(turn): a delivered bubble must decode")
            if await isOver(latest) { ended = true; break }

            guard let s = await actionableSeat(latest, players: 2) else {
                XCTFail("turn \(turn): game is not over but no seat can act"); break
            }

            // Switch to that player (seat-identity mock: participant index == seat).
            m.become(s)
            XCTAssertNotEqual(screen(m), .damaged, "turn \(turn): switching players must not damage")

            let keyBeforeStage = m.viewKey
            guard let next = try await stageTurn(parent: latest, seat: s) else { ended = true; break }
            m.stage(next, seat: s)
            XCTAssertEqual(m.viewKey, keyBeforeStage, "turn \(turn): staging must not move viewKey")
            m.deliver()
            XCTAssertEqual(screen(m), .board, "turn \(turn): the delivered bubble must load")
            moves += 1
        }

        XCTAssertTrue(ended, "the game should reach game-over within the turn budget")
        XCTAssertGreaterThan(moves, 4, "a real game plays several turns before ending")
        XCTAssertGreaterThan(m.transcript.count, 4, "every delivered move is a transcript bubble")
    }
}
