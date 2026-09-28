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

    /// The reported bug, as a scenario: Alex opens a lobby, Vera and Boris join,
    /// Dima starts it at four. Boris - a JOINER, not the sender, and the person
    /// in the screenshot who was left staring at a three-name lobby - opens the
    /// started bubble. It must give him the BOARD.
    ///
    /// This used to seed "Boris's device has his own lobby reseal cached" and
    /// name itself after it. That seeding went inert in round 7 and has now been
    /// deleted with the cache it wrote to, so the test never built the scenario
    /// it named: it was passing on the plain case (nothing cached at all). The
    /// assertion is the half that was always real and is kept - a started chain
    /// is phase 2 and phase is the whole of what picks lobby vs board, so a
    /// LOBBY-CAPACITY 8-seat game started at four still resolves to a board.
    /// Nothing seeds a preference any more because there is nothing left that
    /// could hold one: the router reads the tapped bubble and nothing else.
    func testAJoinerOpeningTheStartedGameGetsTheBoard() async throws {
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
        }

        let lobbyEnv = try await MessageEnvelope.decode(payload: newest, viewer: -1)
        joins.append(MessageJoin(seat: 3, name: "Dima"))
        let live = try await k.startFromLobby(
            lobbyPayload: newest, gameId: gid, actingSeat: 3,
            parent8: MessageTurnController.firstEight(hex: lobbyEnv.digest), joins: joins)

        let screen = await MessageSurfaceRouter.resolve(payload: live, startNewGame: false,
                                                        chatKey: chat, store: store)
        XCTAssertEqual(screen, .board(payload: live),
                       "a joiner opening the started game must get the BOARD, not a lobby")
    }

    /// Round 7 deliberately changed the other direction (was note 15's Rule P):
    /// tapping a STALE invite now renders that invite. Rule P - which used to
    /// prefer a started chain over the tapped bubble - is gone with the
    /// preferred-chain cache, so the router shows exactly the bubble you tapped
    /// (owner: "the last text has everything we need").
    ///
    /// The staleness here is REAL, not seeded: the same game is started off this
    /// very lobby, which leaves the resident kernel holding the LIVE game (a
    /// decode adopts, §7.3). The pair of assertions is the whole point - the
    /// same router, moments apart, calls the live chain a board and the older
    /// lobby chain a lobby. Which bubble you hand it is the only input; nothing
    /// about the newer chain existing changes the older one's answer.
    ///
    /// This test used to seed a "cache" for the started game here and claim the
    /// tapped lobby beat it. That write went inert in round 7 and is now
    /// deleted, so the premise is stated the way it actually holds.
    func testAStaleLobbyBubbleRendersAsTheTappedLobby() async throws {
        let k = MessageKernel.shared
        let gid: UInt64 = 4343
        try await k.newGame(seed: Data(repeating: 11, count: 32), players: 8)
        let joins = [MessageJoin(seat: 0, name: "Alex"), MessageJoin(seat: 1, name: "Vera")]
        let lobby = try await k.seal(phase: 0, lastActorSeat: 0, gameId: gid,
                                     parent8: Data(repeating: 0, count: 8), joins: joins)
        // The game really does start, off this very lobby: the invite left in
        // the transcript is now genuinely stale.
        let lobbyEnv = try await MessageEnvelope.decode(payload: lobby, viewer: -1)
        let live = try await k.startFromLobby(
            lobbyPayload: lobby, gameId: gid, actingSeat: 1,
            parent8: MessageTurnController.firstEight(hex: lobbyEnv.digest), joins: joins)

        let started = await MessageSurfaceRouter.resolve(payload: live, startNewGame: false,
                                                         chatKey: chat, store: store)
        XCTAssertEqual(started, .board(payload: live),
                       "the started chain for this game does have a board to show")

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
