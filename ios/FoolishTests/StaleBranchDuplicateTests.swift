// A DUPLICATE IS NOT A SUPERSESSION - round 42.
//
// THE REPORT (owner, real device, 1.0(40)): "this game state somehow got me
// into an 'an older move - the game has moved on' message. not sure how, but
// the move was perfectly legal. Somehow I do see that there are two bubble
// previews, both saying 'eva attacks with 8 of clubs'."
//
// Two bubbles in the transcript carried the SAME move, and the board then
// refused a legal move as superseded. Both halves are one bug, and only the
// second half is fixable here:
//
//   * a re-seal stamps a fresh send clock, so one move sealed twice is two
//     byte-different payloads describing one identical game state (§10's
//     undo-to-empty overwrite is exactly that, and `conversation.insert` only
//     replaces a draft that has not been SENT, so the re-seal lands as a second
//     bubble rather than a replacement);
//   * RULE P THEN ORDERS THAT PAIR, because Rule P is a TOTAL order and has to
//     be. Same-state siblings tie on phase, round, turn and joins and fall
//     through to msg_rule_p's lexicographic digest tiebreak - a coin flip; a
//     same-state CHILD wins outright on rule 4. Either way the old gate read
//     `preferred(mine, known) > 0` as "the table has moved past you".
//
// "NOT NEWER" IS NOT "OLDER". These drive `StaleBranchGate.rank` with real
// sealed chains from the real kernel - no simulator, no fixture hex frozen into
// the file - and pin that Rule P's verdict is now only a NECESSARY condition.
//
// MUTATION-CHECKED, four mutants, every test in this file caught by one:
//   A  `isAhead` turn `>` loosened to `>=`, i.e. a tie reads as ahead - which
//      IS the 1.0(40) bug: fails testASameStateChildDoesNotSupersede,
//      testTwoSiblingsOfTheSameMoveDoNotSupersedeEitherWay and
//      testTheOrderIsStrictAndRoundOutranksTurn.
//   B  `isAhead` never true, the gate never closes: fails
//      testARealAdvanceStillWins (and A's ordering test).
//   C  the not-ahead branch records the sibling it just declined: fails
//      testTheGateDoesNotRecordAnEqualRankSibling.
//   D  a first sighting is not recorded: fails
//      testNothingOnFileIsPlayableAndRecorded.
// Note what NO mutant here catches: Rule P's half. Dropping the `preferred`
// call leaves every test green, because a chain that `isAhead` is one Rule P
// would have preferred anyway. Rule P is kept as the first of the two
// authorities on its own merits (it is the thread's convergence rule, and it
// reads the joins and the parent chain, which these three fields cannot), not
// because anything below pins it.

import XCTest
@testable import FoolishKit

@MainActor
final class StaleBranchDuplicateTests: XCTestCase {

    private let joins = [MessageJoin(seat: 0, name: "Eva"), MessageJoin(seat: 1, name: "Alex")]
    private let zero8 = Data(repeating: 0, count: 8)
    private let gameId: UInt64 = 0xDEAD

    private var store: MessageGameStore!
    private let suite = "cards.foolish.tests.dup"

    override func setUp() {
        super.setUp()
        UserDefaults(suiteName: suite)?.removePersistentDomain(forName: suite)
        store = MessageGameStore(suiteName: suite)
    }

    /// The first 8 bytes of a digest, as `seal` wants its parent.
    private func parent8(of env: MessageEnvelope) -> Data {
        var d = Data()
        var i = env.digest.startIndex
        for _ in 0..<8 {
            let j = env.digest.index(i, offsetBy: 2)
            d.append(UInt8(env.digest[i..<j], radix: 16)!)
            i = j
        }
        return d
    }

    /// A live 2p game with one attack applied, sealed. The deal is SEARCHED for
    /// rather than assumed so the file cannot pass-or-skip on a shuffle.
    private func openingChain() async throws -> (payload: Data, env: MessageEnvelope, mover: Int) {
        let k = MessageKernel.shared
        for salt in UInt8(1)...UInt8(60) {
            try await k.newGame(seed: Data(repeating: salt, count: 32), players: 2)
            var opener = -1
            var first: Move?
            for s in 0..<2 {
                if let m = (await k.residentLegal(seat: s)).first(where: { $0.type == .attack }) {
                    opener = s; first = m; break
                }
            }
            guard let atk = first else { continue }
            try await k.apply(seat: opener, move: atk)
            let p = try await k.seal(phase: 2, lastActorSeat: opener, gameId: gameId,
                                     parent8: zero8, joins: joins)
            let env = try await MessageEnvelope.decode(payload: p, viewer: -1)
            return (p, env, opener)
        }
        throw XCTSkip("no 2p deal in 60 tries opened with an attack")
    }

    /// Re-seal the chain the kernel is CURRENTLY sitting on, naming `parent` and
    /// a distinct send clock - the shape a §10 undo-to-empty overwrite takes.
    private func reseal(after mover: Int, parent: Data, clock: Int) async throws -> Data {
        try await MessageKernel.shared.seal(phase: 2, lastActorSeat: mover, gameId: gameId,
                                            parent8: parent, joins: joins, sentAt: clock)
    }

    // MARK: - the bug

    /// THE REPORT, as geometry. The chain on file is a same-state CHILD of the
    /// one being opened: Rule P rule 4 prefers it outright, deterministically,
    /// no coin flip involved. The board must stay playable anyway - the child
    /// carries not one atom more of the game than the board already shows.
    func testASameStateChildDoesNotSupersede() async throws {
        let (mine, env, mover) = try await openingChain()
        let child = try await reseal(after: mover, parent: parent8(of: env), clock: env.sentAt + 1)
        XCTAssertNotEqual(child, mine, "a re-seal must actually produce different bytes")

        let childEnv = try await MessageEnvelope.peek(payload: child)
        XCTAssertEqual(childEnv.phase, env.phase)
        XCTAssertEqual(childEnv.round, env.round)
        XCTAssertEqual(childEnv.turn, env.turn, "the re-seal moved no part of the game")

        let pref = try await MessageKernel.shared.preferred(mine, child)
        XCTAssertGreaterThan(pref, 0, "Rule P must prefer the child - this is the precondition")

        store.setLatestChain(gameId: env.gameId, chatKey: "chat", payload: child)
        let v = await StaleBranchGate.rank(payload: mine, env: env, chatKey: "chat", store: store)
        XCTAssertFalse(v.superseded, "a same-state child is not the game moving on")
        XCTAssertNil(v.newest)
    }

    /// The other half: two SIBLINGS of one move, same parent, different send
    /// clocks. They tie every ranked field and land on msg_rule_p's digest
    /// tiebreak, so which one Rule P names is arbitrary - the gate must answer
    /// the same either way, so both directions are asserted.
    func testTwoSiblingsOfTheSameMoveDoNotSupersedeEitherWay() async throws {
        let (a, envA, mover) = try await openingChain()
        let b = try await reseal(after: mover, parent: zero8, clock: envA.sentAt + 7)
        XCTAssertNotEqual(a, b, "two seals of one move must differ, or there is nothing to rank")

        let envB = try await MessageEnvelope.peek(payload: b)
        XCTAssertEqual([envB.phase, envB.round, envB.turn], [envA.phase, envA.round, envA.turn])
        let rank = try await MessageKernel.shared.preferred(a, b)
        XCTAssertNotEqual(rank, 0, "a total order must name a winner - that is the trap")

        for (payload, env) in [(a, envA), (b, envB)] {
            let other = payload == a ? b : a
            store.setLatestChain(gameId: env.gameId, chatKey: "chat", payload: other)
            let v = await StaleBranchGate.rank(payload: payload, env: env,
                                               chatKey: "chat", store: store)
            XCTAssertFalse(v.superseded, "whichever sibling Rule P prefers, neither is ahead")
        }
    }

    /// The high-water mark must NOT be overwritten by an equal-rank sibling.
    /// Rule P prefers what is on file; replacing it with a tie would let the
    /// mark flip-flop with every tap, and a flip-flopping mark cannot catch the
    /// branch this gate exists for.
    func testTheGateDoesNotRecordAnEqualRankSibling() async throws {
        let (mine, env, mover) = try await openingChain()
        let child = try await reseal(after: mover, parent: parent8(of: env), clock: env.sentAt + 1)
        store.setLatestChain(gameId: env.gameId, chatKey: "chat", payload: child)

        _ = await StaleBranchGate.rank(payload: mine, env: env, chatKey: "chat", store: store)
        XCTAssertEqual(store.latestChain(gameId: env.gameId, chatKey: "chat"), child,
                       "the preferred chain stays the mark even though it did not supersede")
    }

    // MARK: - what must NOT change

    /// THE GATE STILL CLOSES. A chain that really carries more of the game -
    /// the defender's cover on top of the opening attack - supersedes the
    /// bubble it grew from, which is the whole reason this gate exists.
    func testARealAdvanceStillWins() async throws {
        let k = MessageKernel.shared
        var found: (old: Data, oldEnv: MessageEnvelope, new: Data)?
        for salt in UInt8(1)...UInt8(60) {
            try await k.newGame(seed: Data(repeating: salt, count: 32), players: 2)
            var opener = -1
            var first: Move?
            for s in 0..<2 {
                if let m = (await k.residentLegal(seat: s)).first(where: { $0.type == .attack }) {
                    opener = s; first = m; break
                }
            }
            guard let atk = first else { continue }
            try await k.apply(seat: opener, move: atk)
            let old = try await k.seal(phase: 2, lastActorSeat: opener, gameId: gameId,
                                       parent8: zero8, joins: joins)
            let oldEnv = try await MessageEnvelope.decode(payload: old, viewer: -1)
            let me = 1 - opener
            guard let cover = (await k.residentLegal(seat: me)).first(where: { $0.type == .cover })
            else { continue }
            try await k.apply(seat: me, move: cover)
            let new = try await k.seal(phase: 2, lastActorSeat: me, gameId: gameId,
                                       parent8: parent8(of: oldEnv), joins: joins)
            found = (old, oldEnv, new)
            break
        }
        guard let f = found else {
            throw XCTSkip("no 2p deal in 60 tries let the defender cover the opening attack")
        }
        let newEnv = try await MessageEnvelope.peek(payload: f.new)
        XCTAssertTrue(StaleBranchGate.isAhead(.init(newEnv), of: .init(f.oldEnv)),
                      "a cover on top of an attack shows more of the game")

        store.setLatestChain(gameId: f.oldEnv.gameId, chatKey: "chat", payload: f.new)
        let v = await StaleBranchGate.rank(payload: f.old, env: f.oldEnv,
                                           chatKey: "chat", store: store)
        XCTAssertTrue(v.superseded, "a board branching off the covered bout is read-only")
        XCTAssertEqual(v.newest, f.new, "and the bar is offered the chain that beat it")
    }

    /// FAILS OPEN, unchanged: nothing on file trusts the bubble it is given, and
    /// records it.
    func testNothingOnFileIsPlayableAndRecorded() async throws {
        let (mine, env, _) = try await openingChain()
        let v = await StaleBranchGate.rank(payload: mine, env: env, chatKey: "chat", store: store)
        XCTAssertFalse(v.superseded)
        XCTAssertEqual(store.latestChain(gameId: env.gameId, chatKey: "chat"), mine)
    }

    // MARK: - the ordering itself

    /// `isAhead` is lexicographic over (phase, round, turn) and STRICT. Round is
    /// asked ABOVE turn on purpose: turn counts atoms and the atom stream is
    /// re-derived at every seal, so a bout-closing action folds that bout's
    /// pending goods into the one round_end atom that replaces them - a chain
    /// can complete a round carrying the same atom count as its parent, or
    /// fewer. Round is monotonic where turn is not.
    func testTheOrderIsStrictAndRoundOutranksTurn() {
        let base = StaleBranchGate.Progress(phase: 2, round: 1, turn: 9)
        XCTAssertFalse(StaleBranchGate.isAhead(base, of: base), "a tie is not ahead")
        XCTAssertTrue(StaleBranchGate.isAhead(.init(phase: 2, round: 1, turn: 10), of: base))
        XCTAssertFalse(StaleBranchGate.isAhead(.init(phase: 2, round: 1, turn: 8), of: base))
        // A round completed with FEWER atoms is still ahead.
        XCTAssertTrue(StaleBranchGate.isAhead(.init(phase: 2, round: 2, turn: 4), of: base))
        // And a finished game outranks any amount of a live one.
        XCTAssertTrue(StaleBranchGate.isAhead(.init(phase: 3, round: 1, turn: 0), of: base))
    }
}
