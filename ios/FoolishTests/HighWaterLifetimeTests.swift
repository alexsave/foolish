// WHEN DOES THE HIGH-WATER MARK DIE? - round 43.
//
// THE REPORT (owner, on `MessageGameStore`'s `fmsg.latest` map): "easy - clear
// when the last move plays."
//
// The map notes the newest chain this device has seen per game, so a board
// built on an older bubble can be recognised as a branch (`StaleBranchGate`).
// Every row is a WHOLE base64 chain, the map is JSON-decoded and re-encoded on
// every read and every write, and it is read every time a bubble is opened -
// and until this round nothing ever removed a row. Every game, in every chat,
// forever, in an extension with a hard memory ceiling that is already under
// investigation for hangs. The contrast was next door in the same file:
// `handOrder` has a cap and evicts, and this map did not.
//
// Two changes are pinned here, and they are INDEPENDENT - the owner can revert
// the second without touching the first:
//
//   1. THE CLEAR. `StaleBranchGate.rank` drops the row when the chain it is
//      ranking says FINISHED. A finished board cannot be staged on, so the
//      mark gates nothing there.
//   2. THE BACKSTOP. A cap with oldest-first eviction on write, exactly
//      `handOrder`'s, because the clear only reclaims games that actually
//      reach game over on this device - a thread that goes quiet at move 4
//      and is never reopened never calls it.
//
// Real sealed chains from the real kernel throughout, no frozen hex: a game is
// played out through `apply` and sealed at phase 3, the way the last move of a
// real game reaches the wire.
//
// MUTATION-CHECKED - see the report at the bottom of this file for which mutant
// each test caught.

import XCTest
@testable import FoolishKit

@MainActor
final class HighWaterLifetimeTests: XCTestCase {

    private let joins = [MessageJoin(seat: 0, name: "Eva"), MessageJoin(seat: 1, name: "Alex")]
    private let zero8 = Data(repeating: 0, count: 8)
    private let chat = "chat.alpha"

    private var store: MessageGameStore!

    override func setUp() {
        super.setUp()
        store = MessageGameStore(defaults: UserDefaults(suiteName: "test.hwl.\(UUID().uuidString)")!)
    }

    // MARK: - building real chains

    /// A game played to game over, with a LIVE chain sealed part way and the
    /// FINISHED chain sealed at the end - the two bubbles a real thread carries
    /// either side of the last move.
    ///
    /// The mid-game seal is taken at the first moment a chain can be sealed at
    /// all rather than at some chosen move number: what these tests need from it
    /// is only that it is a real phase-2 chain of the SAME game, which Rule P
    /// will rank below the finished one.
    private func playedOut(gameId: UInt64) async throws
        -> (live: Data, liveEnv: MessageEnvelope, finished: Data, finishedEnv: MessageEnvelope) {
        let k = MessageKernel.shared
        try await k.newGame(seed: Data((0..<32).map { UInt8($0 &* 7 &+ 3) | 1 }), players: 2)

        // One real action, then a phase-2 seal: the mid-game bubble.
        var opened = false
        for s in 0..<2 where !opened {
            if let m = (await k.residentLegal(seat: s)).first(where: { $0.type == .attack }) {
                try await k.apply(seat: s, move: m); opened = true
            }
        }
        guard opened else { throw XCTSkip("the opening deal had no attack to make") }
        let live = try await k.seal(phase: 2, lastActorSeat: 0, gameId: gameId,
                                    parent8: zero8, joins: joins)
        let liveEnv = try await MessageEnvelope.decode(payload: live, viewer: -1)
        XCTAssertEqual(liveEnv.phase, 2, "the mid-game seal must actually be LIVE")

        // Then drive the same resident game to its end and seal phase 3. The
        // kernel REFUSES a phase-3 header over a game that is not over, and a
        // phase-2 header over one that is (msg_wire.c: `if (over && e->phase !=
        // MSG_PHASE_FINISHED) return MSG_EPHASE;` and its converse), so this
        // seal succeeding is itself the proof the game really finished.
        var guardN = 0
        while (await k.residentView(viewer: -1))?.isOver != true, guardN < 6000 {
            guardN += 1
            var acted = false
            for s in 0..<2 {
                if let m = (await k.residentLegal(seat: s)).first(where: { $0.type != .wait }) {
                    try? await k.apply(seat: s, move: m); acted = true; break
                }
            }
            if !acted { break }
        }
        guard (await k.residentView(viewer: -1))?.isOver == true else {
            throw XCTSkip("the seeded 2p game did not reach game over")
        }
        let finished = try await k.seal(phase: 3, lastActorSeat: 0, gameId: gameId,
                                        parent8: zero8, joins: joins)
        let finishedEnv = try await MessageEnvelope.decode(payload: finished, viewer: -1)
        XCTAssertEqual(finishedEnv.phase, StaleBranchGate.finishedPhase,
                       "phase 3 is MSG_PHASE_FINISHED, and the kernel only stamps it on a game that is over")
        return (live, liveEnv, finished, finishedEnv)
    }

    /// A mid-game chain on its own, for the tests that never need an ending.
    private func liveChain(gameId: UInt64) async throws -> (Data, MessageEnvelope) {
        let k = MessageKernel.shared
        try await k.newGame(seed: Data(repeating: 9, count: 32), players: 2)
        for s in 0..<2 {
            if let m = (await k.residentLegal(seat: s)).first(where: { $0.type == .attack }) {
                try await k.apply(seat: s, move: m); break
            }
        }
        let p = try await k.seal(phase: 2, lastActorSeat: 0, gameId: gameId,
                                 parent8: zero8, joins: joins)
        return (p, try await MessageEnvelope.decode(payload: p, viewer: -1))
    }

    // MARK: - 1. the clear

    /// THE OWNER'S ASK. Ranking the chain that ENDED the game drops that game's
    /// row: the last move plays, and the note is gone.
    func testAFinishedChainClearsTheMark() async throws {
        let g = try await playedOut(gameId: 0xF1)

        // The mark stands where the mid-game bubble left it.
        store.setLatestChain(gameId: g.liveEnv.gameId, chatKey: chat, payload: g.live)
        XCTAssertEqual(store.latestChain(gameId: g.liveEnv.gameId, chatKey: chat), g.live,
                       "precondition: the live bubble is on file")

        let v = await StaleBranchGate.rank(payload: g.finished, env: g.finishedEnv,
                                           chatKey: chat, store: store)

        XCTAssertNil(store.latestChain(gameId: g.finishedEnv.gameId, chatKey: chat),
                     "a finished game's high-water row is dead weight and must be gone")
        // AND THE VERDICT IS UNCHANGED. The clear must not cost the board
        // anything: a finished chain is nobody's stale branch.
        XCTAssertFalse(v.superseded, "the chain that ended the game is not a branch off an old bubble")
        XCTAssertNil(v.newest, "with nothing superseded there is nothing for the bar to offer")
    }

    /// The clear is not "clear on any open". A LIVE chain still records, which
    /// is the whole round-20 feature - without it a branch off an old bubble
    /// has nothing to be weighed against.
    func testALiveChainStillRecordsTheMark() async throws {
        let (payload, env) = try await liveChain(gameId: 0xF2)
        let v = await StaleBranchGate.rank(payload: payload, env: env, chatKey: chat, store: store)
        XCTAssertEqual(store.latestChain(gameId: env.gameId, chatKey: chat), payload,
                       "a mid-game chain is still the newest this device has seen")
        XCTAssertFalse(v.superseded)
        XCTAssertNil(v.newest)
    }

    /// THE GATE STILL CLOSES, and the clear does not fire from the other side of
    /// the same pair. Opening the OLD mid-game bubble while the FINISHED chain is
    /// on file must still go read-only and still offer the ending - a player who
    /// taps back into a played-out game must not be able to stage onto it, and
    /// must not be stranded there either.
    func testAnOldLiveBubbleIsStillSupersededByTheFinishedChainOnFile() async throws {
        let g = try await playedOut(gameId: 0xF3)
        store.setLatestChain(gameId: g.finishedEnv.gameId, chatKey: chat, payload: g.finished)

        let v = await StaleBranchGate.rank(payload: g.live, env: g.liveEnv,
                                           chatKey: chat, store: store)

        XCTAssertTrue(v.superseded, "the game finished without this board - it is a branch")
        XCTAssertEqual(v.newest, g.finished, "and the bar is offered the chain that ended it")
        XCTAssertEqual(store.latestChain(gameId: g.finishedEnv.gameId, chatKey: chat), g.finished,
                       "the ending stays on file: this rank was of the LIVE chain, not of it")
    }

    /// THE DUPLICATE ROW A RE-KEYED CHAT LEAVES BEHIND. Adding or removing a
    /// group member re-keys `chatKey` mid-game (`seatForBubble`'s doc), and this
    /// map is keyed by the PAIR - so the re-key does not replace the row, it
    /// writes the same game a second time under a second key. A chatKey-scoped
    /// delete would collect today's row and leave yesterday's with nothing that
    /// could ever reach it, which is why `forgetLatestChain` is keyed by game
    /// alone.
    func testTheClearSweepsTheRowARekeyedChatLeftBehind() async throws {
        let g = try await playedOut(gameId: 0xF4)
        let before = "chat.before.the.member.left"
        store.setLatestChain(gameId: g.liveEnv.gameId, chatKey: before, payload: g.live)
        store.setLatestChain(gameId: g.liveEnv.gameId, chatKey: chat, payload: g.live)

        _ = await StaleBranchGate.rank(payload: g.finished, env: g.finishedEnv,
                                       chatKey: chat, store: store)

        XCTAssertNil(store.latestChain(gameId: g.finishedEnv.gameId, chatKey: chat))
        XCTAssertNil(store.latestChain(gameId: g.finishedEnv.gameId, chatKey: before),
                     "the orphan the re-key left behind is collected too, or nothing ever collects it")
    }

    /// The sweep is by GAME, never by prefix: another game's row in the same
    /// chat is not collateral. (`chatKey` is itself a "|"-joined participant
    /// set, so the composite key has many separators - only the last one splits
    /// the pair, and a gameId is all digits.)
    func testTheClearSparesOtherGames() async throws {
        let g = try await playedOut(gameId: 0xF5)
        let (other, otherEnv) = try await liveChain(gameId: 0xF6)
        store.setLatestChain(gameId: otherEnv.gameId, chatKey: chat, payload: other)
        store.setLatestChain(gameId: g.liveEnv.gameId, chatKey: chat, payload: g.live)

        _ = await StaleBranchGate.rank(payload: g.finished, env: g.finishedEnv,
                                       chatKey: chat, store: store)

        XCTAssertNil(store.latestChain(gameId: g.finishedEnv.gameId, chatKey: chat))
        XCTAssertEqual(store.latestChain(gameId: otherEnv.gameId, chatKey: chat), other,
                       "the game still being played keeps its mark")
    }

    // MARK: - 2. the backstop (independently revertable)

    /// ABANDONED GAMES. The clear above only reaches games that actually reach
    /// game over on this device; a thread that goes quiet mid-game and is never
    /// reopened never calls it. The cap is the bound for those, and the victim
    /// is the LEAST RECENTLY WRITTEN row - the game least likely to still be
    /// running.
    ///
    /// The first row is written and then given a clear gap before the rest, so
    /// "oldest" is unambiguous rather than a race between two writes in the same
    /// microsecond.
    func testTheCapEvictsTheOldestRow() async throws {
        let cap = MessageGameStore.latestChainCap
        let payload = Data("not a chain, and never decoded - the cap only counts rows".utf8)

        store.setLatestChain(gameId: "0", chatKey: chat, payload: payload)
        try await Task.sleep(nanoseconds: 5_000_000)
        for i in 1...cap { store.setLatestChain(gameId: "\(i)", chatKey: chat, payload: payload) }

        XCTAssertNil(store.latestChain(gameId: "0", chatKey: chat),
                     "row \(cap + 1) evicts the oldest, or the map grows forever")
        for i in 1...cap {
            XCTAssertEqual(store.latestChain(gameId: "\(i)", chatKey: chat), payload,
                           "only the oldest goes - row \(i) was still in the cap")
        }
    }

    /// Under the cap, nothing is evicted. (The guard against a cap that is
    /// somehow off by one, or an eviction that fires unconditionally.)
    func testUnderTheCapNothingIsEvicted() {
        let cap = MessageGameStore.latestChainCap
        let payload = Data("row".utf8)
        for i in 0..<cap { store.setLatestChain(gameId: "\(i)", chatKey: chat, payload: payload) }
        for i in 0..<cap {
            XCTAssertEqual(store.latestChain(gameId: "\(i)", chatKey: chat), payload)
        }
    }

    // MARK: - the store's own contract

    /// `forgetLatestChain` at the store level: by game, across every chat, and a
    /// no-op (not a crash, not a rewrite) when there is nothing to collect.
    func testForgetLatestChainIsByGameAcrossChats() {
        let a = Data("chain-a".utf8), b = Data("chain-b".utf8)
        store.setLatestChain(gameId: "7", chatKey: "chatA", payload: a)
        store.setLatestChain(gameId: "7", chatKey: "chatB", payload: a)
        store.setLatestChain(gameId: "17", chatKey: "chatA", payload: b)

        store.forgetLatestChain(gameId: "7")

        XCTAssertNil(store.latestChain(gameId: "7", chatKey: "chatA"))
        XCTAssertNil(store.latestChain(gameId: "7", chatKey: "chatB"))
        XCTAssertEqual(store.latestChain(gameId: "17", chatKey: "chatA"), b,
                       "game 17 must not be swept by a suffix match on game 7")

        store.forgetLatestChain(gameId: "999")
        XCTAssertEqual(store.latestChain(gameId: "17", chatKey: "chatA"), b)
    }
}

// MUTATION REPORT - every mutant below was reintroduced into the shipping
// source, the suite RUN, and the named tests observed to FAIL for the named
// reason, then reverted. Nothing here is claimed from reading.
//
//   A  `StaleBranchGate.record` writes unconditionally - the pre-round-43 body,
//      i.e. THE BUG ITSELF: fails testAFinishedChainClearsTheMark,
//      testTheClearSweepsTheRowARekeyedChatLeftBehind (both halves) and
//      testTheClearSparesOtherGames.
//   B  `record` clears unconditionally (the guard dropped, so the mark is never
//      written at all): fails testALiveChainStillRecordsTheMark - and, next
//      door, StaleBranchDuplicateTests.testNothingOnFileIsPlayableAndRecorded,
//      which is the round-20 feature this must not cost.
//   C  `forgetLatestChain` narrowed to one chatKey (the obvious alternative
//      design, and the one that would leave the re-key orphan behind): fails
//      testForgetLatestChainIsByGameAcrossChats and
//      testTheClearSweepsTheRowARekeyedChatLeftBehind.
//   D  the sweep matching `hasSuffix(gameId)` without the "|" separator: fails
//      testForgetLatestChainIsByGameAcrossChats - game "17" ends in "7".
//   E  the cap's eviction call removed (the map grows forever again): fails
//      testTheCapEvictsTheOldestRow.
//   F  eviction picking `max(by:)`, i.e. newest-first - it would throw away the
//      game being played and keep the abandoned ones: fails
//      testTheCapEvictsTheOldestRow on both assertions.
//   G  the superseded path RECORDS the board it just rejected (an easy thing
//      for a later edit to "tidy" into place; it would overwrite the ending
//      with the stale bubble): fails
//      testAnOldLiveBubbleIsStillSupersededByTheFinishedChainOnFile.
//
// What NO mutant here catches, said plainly: nothing pins that the clear is
// keyed on the phase of the chain being RANKED rather than the one on file.
// The two only differ in fork geometries these tests do not build (two endings
// of one game), and in every one of them both chains are finished anyway.
