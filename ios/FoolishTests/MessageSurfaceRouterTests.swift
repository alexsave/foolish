// MessageSurfaceRouterTests — "the extension is stuck on the lobby while a game
// is in play", driven end to end without a simulator or a single tap.
//
// The screenshot: an 8-person thread whose last bubble is a started game, and
// Boris — a player IN that game — looking at a three-name lobby. Every earlier
// fix for this shape was aimed at a symptom, because the decision lived inside
// the view's @State and could not be asked directly. It can now.
import XCTest
@testable import FoolishKit

@MainActor
final class MessageSurfaceRouterTests: XCTestCase {

    private let chat = "chat-A"
    private var store: MessageGameStore!

    override func setUp() {
        super.setUp()
        let suite = "test.router.\(UUID().uuidString)"
        let d = UserDefaults(suiteName: suite)!
        d.removePersistentDomain(forName: suite)
        store = MessageGameStore(defaults: d)
    }

    /// What a device caches when it acts on a chain (GameSurface.cache).
    private func cache(_ payload: Data, env: MessageEnvelope, seat: Int, at t: Double) {
        store.put(MessageGameRecord(
            gameId: env.gameId, chatKey: chat, mySeat: seat, nPlayers: env.nPlayers,
            round: env.round, turn: env.turn, phase: env.phase, finished: env.phase == 3,
            names: [:], payloadBase32: Base32.encode(payload), updatedAt: t))
    }

    /// The reported bug, as a scenario: Alex opens a lobby, Vera and Boris join,
    /// Dima starts it at four. Boris's device has his own lobby reseal cached —
    /// which is exactly what he was left staring at. Opening the started bubble
    /// must give him the BOARD.
    func testAJoinerOpeningTheStartedGameGetsTheBoardNotTheirCachedLobby() async throws {
        let k = MessageKernel.shared
        let gid: UInt64 = 4242
        try await k.newGame(seed: Data(repeating: 9, count: 32), players: 8)

        var joins = [MessageJoin(seat: 0, name: "Alex")]
        var newest = try await k.seal(phase: 0, lastActorSeat: 0, gameId: gid,
                                      parent8: Data(repeating: 0, count: 8), joins: joins)
        for (seat, name) in [(1, "Vera"), (2, "Boris")] {
            let env = try await MessageEnvelope.decode(payload: newest, viewer: -1)
            _ = try await k.decode(payload: newest, viewer: -1)
            joins.append(MessageJoin(seat: seat, name: name))
            newest = try await k.seal(phase: 0, lastActorSeat: seat, gameId: gid,
                                      parent8: MessageTurnController.firstEight(hex: env.digest),
                                      joins: joins)
            // Boris's device caches his own reseal — the lobby he was stuck on.
            if seat == 2 {
                let mine = try await MessageEnvelope.decode(payload: newest, viewer: -1)
                cache(newest, env: mine, seat: 2, at: 100)
            }
        }

        let lobbyEnv = try await MessageEnvelope.decode(payload: newest, viewer: -1)
        joins.append(MessageJoin(seat: 3, name: "Dima"))
        let live = try await k.startFromLobby(
            lobbyPayload: newest, gameId: gid, actingSeat: 3,
            parent8: MessageTurnController.firstEight(hex: lobbyEnv.digest), joins: joins)

        let screen = await MessageSurfaceRouter.resolve(payload: live, startNewGame: false,
                                                        chatKey: chat, store: store)
        XCTAssertEqual(screen, .board(payload: live),
                       "a joiner opening the started game must get the BOARD, not the lobby they cached")
    }

    /// Round 7 deliberately changed the other direction (was note 15's Rule P):
    /// tapping a STALE invite now renders that invite. Rule P — which used to
    /// prefer a started chain the device had cached over the tapped bubble — is
    /// gone with the preferred-chain cache, so the router always shows exactly the
    /// bubble you tapped (owner: "the last text has everything we need"). A tapped
    /// lobby therefore renders as a lobby, cache or no cache.
    func testAStaleLobbyBubbleRendersAsTheTappedLobby() async throws {
        let k = MessageKernel.shared
        let gid: UInt64 = 4343
        try await k.newGame(seed: Data(repeating: 11, count: 32), players: 8)
        let joins = [MessageJoin(seat: 0, name: "Alex"), MessageJoin(seat: 1, name: "Vera")]
        let lobby = try await k.seal(phase: 0, lastActorSeat: 0, gameId: gid,
                                     parent8: Data(repeating: 0, count: 8), joins: joins)
        // Even having "cached" a started game for this id, the tapped lobby wins.
        let lobbyEnv = try await MessageEnvelope.decode(payload: lobby, viewer: -1)
        let live = try await k.startFromLobby(
            lobbyPayload: lobby, gameId: gid, actingSeat: 1,
            parent8: MessageTurnController.firstEight(hex: lobbyEnv.digest), joins: joins)
        let liveEnv = try await MessageEnvelope.decode(payload: live, viewer: -1)
        cache(live, env: liveEnv, seat: 1, at: 200)          // inert now (no preferred-chain cache)

        let screen = await MessageSurfaceRouter.resolve(payload: lobby, startNewGame: false,
                                                        chatKey: chat, store: store)
        XCTAssertEqual(screen, .lobby(payload: lobby),
                       "with Rule P removed, a tapped lobby renders as that lobby")
    }

    /// A genuine lobby is still a lobby — nothing above may turn every invite
    /// into a board.
    func testAFreshInviteIsALobby() async throws {
        let k = MessageKernel.shared
        try await k.newGame(seed: Data(repeating: 13, count: 32), players: 8)
        let lobby = try await k.seal(phase: 0, lastActorSeat: 0, gameId: 4444,
                                     parent8: Data(repeating: 0, count: 8),
                                     joins: [MessageJoin(seat: 0, name: "Alex")])
        let screen = await MessageSurfaceRouter.resolve(payload: lobby, startNewGame: false,
                                                        chatKey: chat, store: store)
        XCTAssertEqual(screen, .lobby(payload: lobby))
    }

    /// Round 7: with the preferred-chain cache gone there is nothing to reopen when
    /// no bubble is selected, so the surface offers New game (setup). (It used to
    /// reopen this chat's newest cached game.) New game still always wins.
    func testNoSelectionShowsSetup() async throws {
        let none = await MessageSurfaceRouter.resolve(payload: nil, startNewGame: false,
                                                      chatKey: chat, store: store)
        XCTAssertEqual(none, .setup, "no bubble + no cache -> New game")

        let asked = await MessageSurfaceRouter.resolve(payload: nil, startNewGame: true,
                                                       chatKey: chat, store: store)
        XCTAssertEqual(asked, .setup, "New game always wins")
    }

    /// Bytes that are not a chain are damaged, not a blank board.
    func testGarbageIsDamaged() async throws {
        let screen = await MessageSurfaceRouter.resolve(payload: Data([1, 2, 3]),
                                                        startNewGame: false,
                                                        chatKey: chat, store: store)
        XCTAssertEqual(screen, .damaged)
    }
}
