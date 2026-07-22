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

    // Mirror of GameSurface.load()'s routing (FoolishKit/Messages/MessagesRootView.swift):
    //   no payloadURL  -> New game setup (no game in the thread yet)
    //   decodable URL  -> load + board
    // (`damaged` is reserved for a link that fails to decode, not reachable from
    // these inputs.) GameSurface only re-runs this when `viewKey` changes — the
    // harness keys MessagesRootView with `.id(model.viewKey)` — so staging must NOT
    // move viewKey, or a live board is destroyed and reloaded straight into it.
    enum Screen: Equatable { case setup, damaged, board }
    func screen(startNewGame: Bool, payloadURL: URL?) -> Screen {
        payloadURL == nil ? .setup : .board
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
        await m.stage(deal, seat: 0)
        XCTAssertNotNil(m.staged, "the blue Send arrow should light after a move stages")
        XCTAssertEqual(m.viewKey, keyBeforeStage,
                       "staging must not change viewKey (pre-fix it flipped startNewGame)")
        m.deliver()
    }

    // MARK: tests

    /// Staging a move must not tear the board down (the old "damaged"/setup flicker).
    func test_stagingDoesNotDamageTheBoard() async throws {
        // No bubble in the thread reads as the New-game setup, never a broken link.
        XCTAssertEqual(screen(startNewGame: false, payloadURL: nil), .setup)

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
        let key = m.viewKey                       // "0-0-0-true" on a fresh 2p launch (chat-scoping
                                                   // added a currentChat segment — see HarnessModel)

        let g = MessageTurnController(genesisSeed: Data(repeating: 3, count: 32),
                                      players: 2, gameId: 0xCAFE, myNickname: "You")
        await g.begin()
        if g.iCanAct, let mv = g.legal.first(where: { $0.type != .wait }) { await g.apply(mv) }
        await m.stage(try await g.stagedPayload(), seat: 0)

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
        await m2.stage(try await g.stagedPayload(), seat: 0)  // staged, NOT delivered
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

    /// A delivered move is committed, so re-adopting our OWN just-sent chain must not
    /// falsely toast "your move was superseded" — Rule R would replay a move the
    /// chain already contains. deliver() clears the pending ledger, exactly as the
    /// extension does on didStartSending.
    func test_deliveredMoveIsCommitted_notSuperseded() async throws {
        let m = HarnessModel(count: 2)
        let gid: UInt64 = 0xD00D
        let g = MessageTurnController(genesisSeed: Data(repeating: 4, count: 32),
                                      players: 2, gameId: gid, myNickname: "You")
        await g.begin()
        if g.iCanAct, let mv = g.legal.first(where: { $0.type != .wait }) {
            await g.apply(mv)
            XCTAssertFalse(MessageGameStore.shared.pending(gameId: String(gid)).isEmpty,
                           "applying a move writes the pending ledger (what Rule R would replay)")
        }
        await m.stage(try await g.stagedPayload(), seat: 0)
        m.deliver()
        XCTAssertTrue(MessageGameStore.shared.pending(gameId: String(gid)).isEmpty,
                      "delivering commits the move -> ledger cleared -> re-adopting our own chain can't supersede it")
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
            await m.stage(next, seat: s)
            XCTAssertEqual(m.viewKey, keyBeforeStage, "turn \(turn): staging must not move viewKey")
            m.deliver()
            XCTAssertEqual(screen(m), .board, "turn \(turn): the delivered bubble must load")
            moves += 1
        }

        XCTAssertTrue(ended, "the game should reach game-over within the turn budget")
        XCTAssertGreaterThan(moves, 4, "a real game plays several turns before ending")
        XCTAssertGreaterThan(m.transcript.count, 4, "every delivered move is a transcript bubble")
    }

    // MARK: - chat scoping (the cross-chat leak fix)

    /// The exact production bug, reproduced through the harness's own two-chat
    /// model: cache a row for "You" in Chat A (mirroring what GameSurface.cache()
    /// writes on adopt — done directly here since GameSurface is a SwiftUI view
    /// the headless tests above don't render, per this file's MOCK note), then
    /// switch to Chat B AS THE SAME PARTICIPANT. `switchChat` deliberately does
    /// NOT rebind the store (see its doc) — Chat A and Chat B share one App
    /// Group suite, exactly like one iPhone's two conversations — so this only
    /// passes if `MessageGameStore`'s chatKey scoping, not device/store
    /// isolation, is what is keeping Chat B blind to Chat A's game. Before the
    /// fix (`games`/`record`/`seat` took no chatKey and the map was read
    /// unscoped) this test fails: Chat B would see Chat A's row.
    func test_chatSwitchDoesNotLeakACachedGameAcrossChats() throws {
        let m = HarnessModel(count: 2)
        let chatAKey = m.chatKey
        MessageGameStore.shared.put(MessageGameRecord(
            gameId: "leak-check", chatKey: chatAKey, mySeat: 0, nPlayers: 2, round: 1, turn: 3,
            phase: 2, finished: false, names: [0: "You", 1: "Vera"],
            payloadBase32: "AAAA", updatedAt: 100))
        XCTAssertEqual(MessageGameStore.shared.games(chatKey: chatAKey).map(\.gameId), ["leak-check"])

        m.switchChat(1)
        XCTAssertNotEqual(m.chatKey, chatAKey, "each simulated chat has its own conversation identity")
        XCTAssertTrue(MessageGameStore.shared.games(chatKey: m.chatKey).isEmpty,
                      "Chat B must not see Chat A's cached game, even on the SAME device/store suite")
        XCTAssertNil(MessageGameStore.shared.record(gameId: "leak-check", chatKey: m.chatKey),
                     "and a direct gameId lookup must not cross chats either")

        m.switchChat(0)
        XCTAssertEqual(m.chatKey, chatAKey, "switching back restores Chat A's identity")
        XCTAssertEqual(MessageGameStore.shared.record(gameId: "leak-check", chatKey: m.chatKey)?.mySeat, 0,
                       "and Chat A's own row is unaffected by the round trip")
    }
    // MARK: - the drawer must never strand the screen

    /// Dismiss the drawer, then touch anything that opens the game ("New", a
    /// player swap, a chat switch): the screen used to end up with NO drawer
    /// (dismissed) and NO compose bar (nominally expanded), and since the
    /// compose bar's "+" is the only way back to a dismissed drawer, that was a
    /// dead end. Owner: "after I fully collapsed the extension view so that it
    /// wouldn't appear, it was just gone."
    ///
    /// `stageIsExpanded` is what the chrome hides the compose bar on, so
    /// asserting it is false while dismissed IS asserting the compose bar is
    /// reachable.
    func testADismissedDrawerNeverHidesTheComposeBar() async throws {
        let m = HarnessModel(count: 2)
        m.dismissDrawer()
        XCTAssertTrue(m.drawerDismissed)
        XCTAssertFalse(m.stageIsExpanded, "a dismissed drawer is not an expanded one")

        // Each of these sets presentation = .expanded; none may hide the "+".
        m.newGame()
        XCTAssertFalse(m.drawerDismissed, "New game brings the drawer back")

        m.dismissDrawer()
        m.become(1)
        XCTAssertFalse(m.drawerDismissed, "swapping player brings the drawer back")

        m.dismissDrawer()
        m.switchChat(1)
        XCTAssertFalse(m.drawerDismissed, "switching chat brings the drawer back")

        // And the "+" itself still works from a plain dismissed state.
        m.dismissDrawer()
        XCTAssertFalse(m.stageIsExpanded, "compose bar stays reachable while dismissed")
        m.reopenDrawer()
        XCTAssertFalse(m.drawerDismissed)
    }

    /// Tapping a transcript bubble opens the extension ON THAT MESSAGE, like a
    /// phone — and brings a dismissed drawer back, since with the drawer gone
    /// the bubbles are the only thing left on screen to tap.
    func testTappingABubbleSelectsItAndReopensTheDrawer() async throws {
        let m = HarnessModel(count: 2)
        try await kickoff(m, seed: 31, gameId: 0xB0B)
        let first = try XCTUnwrap(m.transcript.first)

        m.dismissDrawer()
        m.openBubble(first)
        XCTAssertFalse(m.drawerDismissed, "tapping a game bubble means show me the game")
        XCTAssertEqual(m.payloadURL, first.url, "the tapped bubble is what the extension opens")
        XCTAssertEqual(m.senderIsLocal, first.senderId == m.localId,
                       "sender inference follows the SELECTED bubble, not the transcript tail")
    }
    /// The double animation, pinned at its source: sending my own move must not
    /// rebuild the board. `viewKey` drives `.id(...)` on the live
    /// MessagesRootView, so a change there tears the surface down and reloads it
    /// from the bubble I just sent — whose last move is the one I just watched
    /// myself play, which the open-replay then plays again.
    func testDeliveringMyOwnMoveDoesNotRebuildTheBoard() async throws {
        let m = HarnessModel(count: 2)
        try await kickoff(m, seed: 41, gameId: 0xD00D)   // stages AND delivers
        let afterFirst = m.viewKey

        // Stage and send a second move as the same player: still the same board.
        let parent = bytes(m.payloadURL)
        XCTAssertFalse(parent.isEmpty)
        if let next = try await stageTurn(parent: parent, seat: 0) {
            let beforeStage = m.viewKey
            await m.stage(next, seat: 0)
            XCTAssertEqual(m.viewKey, beforeStage, "staging must not rebuild the board")
            m.deliver()
            XCTAssertEqual(m.viewKey, beforeStage,
                           "and neither must sending — that reload is the second animation")
        }
        XCTAssertEqual(m.viewKey, afterFirst, "nothing about my own send rebuilds the board")

        // But becoming another player (a real device receiving) DOES.
        m.become(1)
        XCTAssertNotEqual(m.viewKey, afterFirst, "another player is another board")
    }
}
