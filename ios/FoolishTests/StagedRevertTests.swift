// StagedRevertTests - the X on a staged LOBBY bubble, 1.1(69).
//
// THE HOLE THESE FILL. The reversal shipped in 1.1(68) with no Swift test
// anywhere: no file mentioned `revertStagedSurface`, `StagedOrigin`,
// `surfaceSwap` or `.noGame`. The C half of the rematch reversal was right and
// the Swift caller could not use it, and a whole release went green over that.
// So the two rules the surface applies before it asks the kernel anything are
// now VALUES with names (FoolishKit/Messages/StagedDraft.swift) rather than
// lines inside a private SwiftUI view, and this is where they are pinned.
//
// WHAT THEY FAIL AGAINST (the mutation matrix, every line of it actually run,
// each against a rebuilt binary):
//   StagedDraft.parent8 keyed on `parent8 != nil` (the 1.1(68) rule)
//        -> testAnEditOfACreatedDraftKeepsTheNewGameOrigin
//        -> testARematchDraftKeepsTheResultCardAsItsOrigin
//        -> testAnEditOfACreatedDraftClaimsTheGenesisParent
//   …returning firstEight(digest) instead of the genesis parent
//        -> testAnEditOfACreatedDraftClaimsTheGenesisParent
//   …dropping the record-once early return altogether
//        -> testLaterEditsOfOneDraftDoNotMoveItsOrigin (+ the three above)
//   …dropping `staged` from it (record once EVER)
//        -> testAFreshDraftRecordsTheChainOnScreen
//   …`clear()` leaving the origin behind
//        -> testAClearedDraftOwesNothing
//   StagedRevert.route answering `.delta` whatever the game ids say
//        -> testAnOriginFromAnotherGameIsASwapNotADelta
//        -> testASurfaceThatCannotBeNamedIsASwapRatherThanASilentReturn
//   …answering `.swap` for the same game (the delta arm lost)
//        -> testTheSameGameOneStateEarlierIsADelta
//   …forgetting `staged` -> testNothingStagedIsNothingToUndo
//   …answering `.nothing` for a create -> testACreateGoesBackToTheNewGameScreen
//   c/src/msg_wire.c: deleting the `showing->game_id != arriving->game_id`
//   guard -> testTwoGamesDiffToNothingAtAll (which is WHY the swap arm exists)
//   c/src/anim_plan.c: `anim_surface_swap` pushing a 0ms SNAP instead of a fade
//        -> testASwapIsOneFadeThatLastsLongEnoughToSee
//   …`settle_ms` folding to 0 with the beats it folds
//        -> testAJoinIsNoBeatsAndAWholeRestToReadThem
//
// WHAT THEY DO NOT REACH, said plainly rather than implied: that
// `revertStagedSurface` calls `StagedRevert.route` at all, and that the swap arm
// plays `adopt` inside `fadeSurface`. That is a SwiftUI `.onChange` on a
// rendered surface; LobbyActionOrderTests pins the shape of the call sites, and
// the rest is filmed on a simulator.
import XCTest
@testable import FoolishKit

@MainActor
final class StagedRevertTests: XCTestCase {

    // MARK: - fixtures

    private func freshSeed(_ salt: UInt8) -> Data {
        Data((0..<32).map { UInt8(truncatingIfNeeded: $0 &* 13 &+ Int(salt)) | 1 })
    }

    /// Seal a WAITING lobby with `names` seated 0..<n at capacity 8.
    private func lobby(_ names: [String], seed: UInt8, gameId: UInt64,
                       parent8: Data = Data(repeating: 0, count: 8))
        async throws -> (Data, MessageEnvelope) {
        let k = MessageKernel.shared
        try await k.newGame(seed: freshSeed(seed), players: 8)
        let joins = names.enumerated().map { MessageJoin(seat: $0.offset, name: $0.element) }
        let payload = try await k.seal(phase: 0, lastActorSeat: 0, gameId: gameId,
                                       parent8: parent8, joins: joins)
        return (payload, try await MessageEnvelope.decode(payload: payload, viewer: -1))
    }

    /// A 64-hex digest that is not any real chain's - `route` never looks at a
    /// digest, and `parent8` only needs one it can read bytes out of.
    private func digest(_ n: UInt8) -> String {
        (0..<32).map { _ in String(format: "%02x", n) }.joined()
    }

    /// Opaque chain bytes. The routing compares GAME IDS, which are handed to it
    /// separately, so nothing here has to be a real envelope.
    private func chain(_ n: UInt8) -> Data { Data(repeating: n, count: 40) }

    // MARK: - U11: the draft records its origin ONCE, and a create has one

    /// THE DEFECT, in one sequence: create, toggle passing, X.
    ///
    /// The create records `.noGame` and no parent - a brand new chain has no
    /// earlier link in the thread to claim. The toggle is the next edit of the
    /// SAME unsent draft, and the 1.1(68) rule asked "have I recorded a PARENT
    /// yet", found none, and re-recorded the origin as the lobby the create had
    /// just made. The X then landed on that lobby: no Start, no Leave, no Join.
    func testAnEditOfACreatedDraftKeepsTheNewGameOrigin() {
        var d = StagedDraft()
        d.created(from: .noGame)                      // createWaiting
        _ = d.parent8(staged: true, digest: digest(0xa1), threadChain: chain(7))
        XCTAssertEqual(d.origin, .noGame,
                       "a create's origin is recorded; the next edit of that draft must not replace it")
    }

    /// The other half of the same fall-through, and a live wire bug in its own
    /// right: the edit sealed claiming the CREATED LOBBY as its parent. That
    /// bubble was never sent - `conversation.insert` replaced it - so the claim
    /// names a link no device in the thread has ever seen. The honest claim is
    /// the one the create itself makes: the genesis parent.
    func testAnEditOfACreatedDraftClaimsTheGenesisParent() {
        var d = StagedDraft()
        d.created(from: .noGame)
        let p = d.parent8(staged: true, digest: digest(0xa1), threadChain: chain(7))
        XCTAssertEqual(p, StagedDraft.genesisParent8)
        XCTAssertNotEqual(p, MessageTurnController.firstEight(hex: digest(0xa1)),
                          "that digest belongs to a draft that replaced itself and was never sent")
    }

    /// A REMATCH records a chain rather than `.noGame`, and is otherwise the
    /// same shape - so the same fall-through moved its origin off the result
    /// card and onto the rematch lobby.
    func testARematchDraftKeepsTheResultCardAsItsOrigin() {
        let card = chain(0x5c)
        var d = StagedDraft()
        d.created(from: .chain(card))                 // createRematchLobby
        _ = d.parent8(staged: true, digest: digest(0xb2), threadChain: chain(0x99))
        XCTAssertEqual(d.origin, .chain(card))
    }

    /// THE RULE THE DRAFT ALWAYS HAD, and it still holds: a Join, a rules move
    /// and a Start staged together revert as ONE, because only the first of them
    /// records where the draft started.
    func testLaterEditsOfOneDraftDoNotMoveItsOrigin() {
        let onScreen = chain(1), intermediate = chain(2)
        var d = StagedDraft()
        let first = d.parent8(staged: false, digest: digest(0xc3), threadChain: onScreen)
        XCTAssertEqual(d.origin, .chain(onScreen))

        let second = d.parent8(staged: true, digest: digest(0xd4), threadChain: intermediate)
        XCTAssertEqual(second, first, "an edit of a staged draft keeps the thread's parent")
        XCTAssertEqual(d.origin, .chain(onScreen), "…and the surface it started from")
    }

    /// RECORD ONCE PER DRAFT, NOT ONCE EVER. Nothing is staged, so this is a new
    /// draft over whatever is on screen now - a rule that only looked at "have I
    /// recorded before" would strand every later action on the first lobby this
    /// surface ever showed.
    func testAFreshDraftRecordsTheChainOnScreen() {
        var d = StagedDraft()
        _ = d.parent8(staged: false, digest: digest(0xc3), threadChain: chain(1))
        _ = d.parent8(staged: false, digest: digest(0xd4), threadChain: chain(2))
        XCTAssertEqual(d.origin, .chain(chain(2)))
    }

    /// A send, or a surface rebuild: the draft is over and nothing is owed.
    func testAClearedDraftOwesNothing() {
        var d = StagedDraft()
        _ = d.parent8(staged: false, digest: digest(0xc3), threadChain: chain(1))
        d.clear()
        XCTAssertNil(d.origin)
        XCTAssertNil(d.parent8)
        XCTAssertEqual(StagedRevert.route(staged: true, origin: d.origin,
                                          showingGameId: "1", baseGameId: "1"), .nothing)
    }

    // MARK: - U2: an origin is not always a delta

    /// THE DEFECT. A rematch mints a fresh random game id, so the result card it
    /// was created over is a DIFFERENT game - and two different games cannot be
    /// diffed (see `testTwoGamesDiffToNothingAtAll`). Routed as a delta it came
    /// back with a zero settle, which the caller reads as the owner's no-op rule
    /// and returns from having done nothing, leaving a control-less orphan lobby
    /// with the bubble already deleted. It is a whole-surface SWAP.
    func testAnOriginFromAnotherGameIsASwapNotADelta() {
        let card = chain(0x5c)
        XCTAssertEqual(StagedRevert.route(staged: true, origin: .chain(card),
                                          showingGameId: "9999", baseGameId: "4242"),
                       .swap(card))
    }

    /// The ordinary lobby action, unchanged: same game, one state earlier, so
    /// the kernel is asked what moved and it is played backwards.
    func testTheSameGameOneStateEarlierIsADelta() {
        let before = chain(0x11)
        XCTAssertEqual(StagedRevert.route(staged: true, origin: .chain(before),
                                          showingGameId: "77", baseGameId: "77"),
                       .delta(before))
    }

    /// A create: the draft MADE the game, so discarding it takes the game with
    /// it and the surface goes back to the New game screen.
    func testACreateGoesBackToTheNewGameScreen() {
        XCTAssertEqual(StagedRevert.route(staged: true, origin: .noGame,
                                          showingGameId: "77", baseGameId: nil),
                       .newGame)
    }

    /// A cancel over a BOARD is the move-level retraction and belongs to
    /// `MessageTableView.cancelStagedBubble`; an origin left behind by a local
    /// chain that staged nothing (`addSoloSeat`, in DEBUG) is not mine to undo
    /// either. Both arrive here as "nothing was staged".
    func testNothingStagedIsNothingToUndo() {
        XCTAssertEqual(StagedRevert.route(staged: false, origin: .chain(chain(1)),
                                          showingGameId: "77", baseGameId: "77"),
                       .nothing)
        XCTAssertEqual(StagedRevert.route(staged: true, origin: nil,
                                          showingGameId: "77", baseGameId: "77"),
                       .nothing)
    }

    /// A surface whose header will not read cannot be diffed against anything.
    /// The swap is the honest answer; the silent return that used to happen here
    /// is what U2 WAS.
    func testASurfaceThatCannotBeNamedIsASwapRatherThanASilentReturn() {
        let back = chain(3)
        XCTAssertEqual(StagedRevert.route(staged: true, origin: .chain(back),
                                          showingGameId: nil, baseGameId: "77"),
                       .swap(back))
    }

    // MARK: - the kernel facts the routing exists because of

    /// WHY THE SWAP ARM EXISTS. `msg_surface_delta` returns on
    /// `showing->game_id != arriving->game_id` - deliberately, and correctly:
    /// two different games share no line of continuity and animating between
    /// them would be a lie. What that means for a CALLER is the part 1.1(68)
    /// missed: no beats AND a zero settle, which is byte-for-byte what "these
    /// two chains describe the same table" answers. A reversal cannot tell them
    /// apart from the plan, so it must not try.
    func testTwoGamesDiffToNothingAtAll() async throws {
        let (a, _) = try await lobby(["Alex", "Vera"], seed: 3, gameId: 7001)
        let (b, _) = try await lobby(["Alex", "Vera", "Kate"], seed: 4, gameId: 7002)
        let plan = await MessageKernel.shared.surfacePlan(showing: a, arriving: b)
        XCTAssertTrue(plan.beats.isEmpty, "a different game is a switch, not a continuation")
        XCTAssertEqual(plan.settle, 0,
                       "and it is indistinguishable from the no-op - which is the whole trap")
    }

    /// …and what the swap arm plays instead: the kernel's own whole-surface
    /// beat, one fade with a real duration, the same one the create and its
    /// mirror already use.
    func testASwapIsOneFadeThatLastsLongEnoughToSee() async {
        let plan = await MessageKernel.shared.surfaceSwap(passing: true)
        XCTAssertEqual(plan.beats.count, 1)
        XCTAssertEqual(plan.beats.first?.transition, .fade)
        XCTAssertEqual(plan.beats.first?.kind, .lobby)
        XCTAssertGreaterThan(plan.beats.first?.duration ?? 0, 0)
        XCTAssertGreaterThan(plan.settle, 0, "a surface that swapped has to be looked at")
    }

    /// U12's half of the same argument: a JOIN has an answer too, and it is the
    /// awkward one - NO beats (the adopt is the snap) and a whole REST to read
    /// it in. `joinLobby` never asked, so it staged (which collapses the drawer)
    /// before the roster had moved. Nothing here can see that ordering;
    /// LobbyActionOrderTests does. This pins the number it spends.
    func testAJoinIsNoBeatsAndAWholeRestToReadThem() async throws {
        let gid: UInt64 = 7100
        let (before, env) = try await lobby(["Alex"], seed: 5, gameId: gid)
        let (after, _) = try await lobby(["Alex", "Vera"], seed: 5, gameId: gid,
                                         parent8: MessageTurnController.firstEight(hex: env.digest))
        let plan = await MessageKernel.shared.surfacePlan(showing: before, arriving: after)
        XCTAssertTrue(plan.beats.isEmpty, "the adopt IS the snap")
        XCTAssertGreaterThan(plan.settle, 0, "…and it still has to be on screen to be seen")
    }
}
