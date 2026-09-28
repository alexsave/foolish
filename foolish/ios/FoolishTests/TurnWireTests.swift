// THE TURN'S CROSSING - that the kernel's answers arrive here as themselves.
//
// The RULES are C's and are pinned there (c/tests/msg_wire_test.c's
// test_turn_controller, mutation-checked). What only this side can go wrong at
// is the crossing: a bit that means one thing in ios_api.h and another in
// msg_wire.h, an enum case whose raw value drifts so `??` quietly swallows the
// answer, or a controller that reports facts about itself that are not true.
//
// Each of those is a SILENT wrong answer rather than a crash, which is why they
// are worth a file. A `?? .adopt` that fires because a verdict number moved
// would adopt over a staged move instead of retracting it, and nothing would
// say so.
//
// MUTATION-CHECKED, each applied on its own against an 8-test baseline that
// reports 0, and restored afterwards to 0 again:
//   State.held given the same rawValue as .sending           ->  4 failures
//   Arrival.retract renumbered to 9, so the `??` default fires -> 1 failure
//   admit crossing move.cards.count instead of the move type ->  2 failures
//   publish returning showHeldView: false                    ->  1 failure
//   chainState omitting .staged                              ->  2 failures
//   chainState omitting .held                                -> 11 failures
//   humanLegal assigned MoveWire.decode(legalPacked)         ->  1 failure

import XCTest
@testable import FoolishKit

@MainActor
final class TurnWireTests: XCTestCase {

    // The §8.2 gate fixture: 2p, turn 7, round 1, sealed by the native kernel.
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

    // MARK: the bits

    /// Eight facts, eight bits. A collision here is not a compile error and not
    /// a crash - it is one fact silently answering for another, which on this
    /// path is a staged move retracted for a bubble that never conflicted.
    func testTheStateBitsAreEightDistinctFlags() {
        let all: [TurnWire.State] = [.staged, .sending, .ready, .superseded,
                                     .retracting, .boardWatching, .held, .genesis]
        var seen = Set<Int32>()
        for b in all {
            XCTAssertEqual(b.rawValue.nonzeroBitCount, 1, "a state flag is one bit")
            XCTAssertTrue(seen.insert(b.rawValue).inserted,
                          "two facts share a bit - one of them will answer for the other")
        }
        let union = all.reduce(TurnWire.State()) { $0.union($1) }
        for b in all { XCTAssertTrue(union.contains(b)) }
        XCTAssertFalse(TurnWire.State().contains(.staged), "an empty state claims nothing")
    }

    // MARK: every answer arrives as itself

    /// Each verdict the kernel can return must land on a NAMED case. Every one
    /// of these enums has a `??` default at the crossing, so a raw value that
    /// drifted would be swallowed rather than reported - the whole answer space
    /// is walked here so nothing can drift unnoticed.
    func testEveryKernelAnswerDecodesIntoItsOwnCase() {
        let live: TurnWire.State = [.ready]

        XCTAssertEqual(TurnWire.admit(live, move: .pickup, pickupHold: 0), .ok)
        XCTAssertEqual(TurnWire.admit(live.union(.retracting), move: .pickup, pickupHold: 0),
                       .retracting)
        XCTAssertEqual(TurnWire.admit(live.union(.superseded), move: .pickup, pickupHold: 0),
                       .superseded)
        XCTAssertEqual(TurnWire.admit(live, move: .pickup, pickupHold: 9), .heldPickup)

        XCTAssertEqual(TurnWire.arrival(live, sameChain: true), .skip)
        XCTAssertEqual(TurnWire.arrival(live.union(.retracting), sameChain: false), .latch)
        XCTAssertEqual(TurnWire.arrival(live, sameChain: false), .adopt)
        XCTAssertEqual(TurnWire.arrival([.ready, .boardWatching, .staged], sameChain: false),
                       .retract)

        XCTAssertEqual(TurnWire.sentSource(staged: false, host: false, sealed: true), .none)
        XCTAssertEqual(TurnWire.sentSource(staged: false, host: true, sealed: true), .host)
        XCTAssertEqual(TurnWire.sentSource(staged: true, host: true, sealed: true), .sealed)

        XCTAssertEqual(TurnWire.sendVerdict(staged: false, host: true, sealed: true,
                                            hostIsSealed: false, decoded: nil), .foreign)
        XCTAssertEqual(TurnWire.sendVerdict(staged: false, host: false, sealed: false,
                                            hostIsSealed: false, decoded: nil), .noop)
        XCTAssertEqual(TurnWire.sendVerdict(staged: true, host: false, sealed: false,
                                            hostIsSealed: false, decoded: nil), .blind)
        XCTAssertEqual(TurnWire.sendVerdict(staged: true, host: false, sealed: true,
                                            hostIsSealed: false, decoded: nil), .decode)
        XCTAssertEqual(TurnWire.sendVerdict(staged: true, host: false, sealed: true,
                                            hostIsSealed: false, decoded: false), .unreadable)
        XCTAssertEqual(TurnWire.sendVerdict(staged: true, host: false, sealed: true,
                                            hostIsSealed: false, decoded: true), .rebase)
    }

    /// The admission door is asked about a move TYPE, and the number it takes is
    /// the menu wire's (MOVE_* / MoveWire.wireIndex). A pickup held by the clock
    /// is refused and a cover of the same shape is not - which is only true if
    /// the right number crossed.
    func testAdmitReadsTheMoveTypeAndNotSomethingElse() {
        let live: TurnWire.State = [.ready]
        XCTAssertEqual(MoveWire.wireIndex(.pickup), 3, "the kernel's MOVE_PICKUP")
        XCTAssertEqual(TurnWire.admit(live, move: .pickup, pickupHold: 5), .heldPickup)
        XCTAssertEqual(TurnWire.admit(live, move: Move(type: .cover, cards: [Card(s: 0, v: 7)]),
                                      pickupHold: 5), .ok,
                       "the hold is about picking up, not about playing")
    }

    /// The hold index, and the `good` case that answers 0 rather than an error.
    func testTheHeldStepIsTheOneBeforeTheCutUnlessThereIsNone() {
        XCTAssertEqual(TurnWire.holdState(events: 4, cut: 2), 1)
        XCTAssertEqual(TurnWire.holdState(events: 3, cut: 0), 0,
                       "a good emits no step of its own, so the transition step is the answer")
        XCTAssertNil(TurnWire.holdState(events: 3, cut: nil))
        XCTAssertNil(TurnWire.holdState(events: 3, cut: 3))
    }

    /// Both halves of the withheld settlement, together. The menu half is the
    /// one a suite has passed against the absence of before now.
    func testAWithheldSettlementDoctorsBothTheBoardAndTheMenu() {
        let held = TurnWire.publish([.ready, .staged, .held], baseAtomsBefore: 45,
                                    stagedAtomsBefore: 51, openReplay: 0, viewWouldChange: true)
        XCTAssertTrue(held.showHeldView)
        XCTAssertTrue(held.emptyMenu, "an empty menu is what stops the player acting on the deal")
        XCTAssertEqual(held.animAtomsBefore, 51)

        let plain = TurnWire.publish([.ready], baseAtomsBefore: 45, stagedAtomsBefore: 51,
                                     openReplay: 3, viewWouldChange: true)
        XCTAssertFalse(plain.showHeldView)
        XCTAssertFalse(plain.emptyMenu)
        XCTAssertEqual(plain.animAtomsBefore, 45)
        XCTAssertTrue(plain.raiseVeil)

        XCTAssertFalse(TurnWire.publish([.ready], baseAtomsBefore: 45, stagedAtomsBefore: 51,
                                        openReplay: 3, viewWouldChange: false).raiseVeil,
                       "a veil nothing will take down must never go up")
    }

    // MARK: the controller tells the truth about itself

    /// Every rule above is only as good as the bits handed to it, and those are
    /// this controller's own account of itself. Walked over a real chain.
    func testAControllerReportsItsOwnBitsTruthfully() async throws {
        let parentBytes = bytes(fixtureHex)
        let parent = try await MessageEnvelope.decode(payload: parentBytes, viewer: 0)

        var chosen: (seat: Int, move: Move)?
        for seat in 0..<parent.nPlayers {
            let menu = await MessageKernel.shared.residentHumanMoves(seat: seat)
            if let m = menu.first { chosen = (seat, m); break }
        }
        guard let (seat, move) = chosen else { return XCTFail("no seat had a human move") }

        let c = MessageTurnController(parentPayload: parentBytes, parent: parent, mySeat: seat)
        XCTAssertFalse(c.chainState.contains(.ready), "nothing established before begin()")
        await c.begin()
        XCTAssertTrue(c.chainState.contains(.ready))
        XCTAssertFalse(c.chainState.contains(.staged), "a fresh adopt stages nothing")
        XCTAssertFalse(c.chainState.contains(.genesis), "a continuation is not a genesis")
        XCTAssertFalse(c.canSend)

        await c.apply(move)
        XCTAssertTrue(c.chainState.contains(.staged), "one action staged")
        XCTAssertTrue(c.canSend)
        XCTAssertEqual(c.chainState.contains(.held), c.settlementHeld,
                       "the held bit IS the withheld settlement, not a second opinion")

        c.markSending()
        XCTAssertTrue(c.chainState.contains(.sending))
        XCTAssertFalse(c.canSend, "the send window has already claimed those bytes")

        await c.undo()
        XCTAssertFalse(c.chainState.contains(.staged), "undo left nothing staged")
        XCTAssertFalse(c.chainState.contains(.held), "…and nothing withheld")
    }

    /// A REAL withheld settlement, WALKED TO rather than posed - and then both
    /// halves of what the board is allowed to publish while it stands.
    ///
    /// Posing the bits proves the rule; reaching one proves the controller
    /// reports its own state honestly, which is the half a pure-function test
    /// cannot see. Two seats play a genesis game through, driven by the HUMAN
    /// menu, until an applied move holds a settlement.
    ///
    /// It also counts the boards where the human menu is strictly SMALLER than
    /// the published one, and insists there were some. That is what stops
    /// `humanLegal` being quietly assigned the raw decode: a narrowing that
    /// never narrows anywhere in a 24-game sweep is not a narrowing.
    ///
    /// The sweep asserts it FOUND a settlement, for the same reason. A run that
    /// reaches no bout end is asserting nothing at all, and would pass silently
    /// against any rule.
    func testARealHeldSettlementShowsInTheBitsAndTheDoctoredPair() async throws {
        var found = 0
        var narrowed = 0
        search: for salt in UInt8(1)...UInt8(24) {
            var payload: Data?
            var env: MessageEnvelope?
            for _ in 0..<120 {
                var moved = false
                for seat in 0..<2 {
                    let c: MessageTurnController
                    if let p = payload, let e = env {
                        c = MessageTurnController(parentPayload: p, parent: e, mySeat: seat)
                    } else {
                        c = MessageTurnController(genesisSeed: sweepSeed(salt), players: 2,
                                                  gameId: 91, myNickname: "P\(seat)")
                    }
                    await c.begin()
                    if c.view?.isOver == true { continue search }
                    XCTAssertLessThanOrEqual(c.humanLegal.count, c.legal.count,
                                             "the human menu may only ever narrow")
                    if c.humanLegal.count < c.legal.count { narrowed += 1 }
                    guard let mv = c.humanLegal.first else { continue }
                    let boardBefore = c.view
                    guard await c.apply(mv) else { continue }
                    moved = true

                    if c.settlementHeld {
                        XCTAssertTrue(c.chainState.contains(.held),
                                      "the held bit and the withheld settlement are one fact")
                        XCTAssertEqual(c.legalPacked, MoveWire.emptyMenu,
                                       "the published menu must be EMPTY while a deal is withheld")
                        XCTAssertTrue(c.legal.isEmpty, "…and so must its decode")
                        XCTAssertTrue(c.humanLegal.isEmpty, "…and its narrowing")
                        XCTAssertFalse(c.iCanAct,
                                       "a board offering nothing is not a board this seat can act on")
                        XCTAssertNotEqual(c.view, boardBefore,
                                          "the ACTION half still played - only the deal is held")
                        found += 1
                        continue search
                    }

                    // Send it on, and let the other seat answer. The clock is
                    // backdated so the receiving defender's pickup hold has
                    // already lapsed and a pickup is admissible.
                    let sealed = try await c.stagedPayload(sentAt: MessageKernel.clockNow() - 60)
                    payload = sealed
                    env = try await MessageEnvelope.peek(payload: sealed)
                    break
                }
                if !moved { continue search }
            }
        }
        XCTAssertGreaterThan(found, 0,
                             "no game in the sweep reached a withheld settlement - this test "
                             + "asserted nothing")
        XCTAssertGreaterThan(narrowed, 0,
                             "the human menu equalled the raw one on every board in the sweep - "
                             + "either `humanLegal` is not narrowing, or the sweep never met a "
                             + "`wait` or a `good` over an uncovered attack")
    }

    private func sweepSeed(_ salt: UInt8) -> Data {
        var d = Data(repeating: 0, count: 32)
        for i in 0..<32 { d[i] = salt &+ UInt8(truncatingIfNeeded: i * 11) }
        return d
    }

    /// The human menu is a NARROWING of the published one, never a second menu:
    /// it holds no `wait`, and every entry in it is on the raw menu too. Read
    /// off the same bytes in the same breath, so the two cannot describe
    /// different boards.
    func testTheHumanMenuNarrowsThePublishedOne() async throws {
        let parentBytes = bytes(fixtureHex)
        let parent = try await MessageEnvelope.decode(payload: parentBytes, viewer: 0)
        for seat in 0..<parent.nPlayers {
            let c = MessageTurnController(parentPayload: parentBytes, parent: parent, mySeat: seat)
            await c.begin()
            XCTAssertFalse(c.humanLegal.contains { $0.type == .wait },
                           "seat \(seat): `wait` is not a move a human makes")
            for m in c.humanLegal {
                XCTAssertTrue(c.legal.contains(m),
                              "seat \(seat): the human menu invented \(m.type) - it may only narrow")
            }
            XCTAssertEqual(c.iCanAct, !c.humanLegal.isEmpty,
                           "seat \(seat): acting is having a human move, and nothing else")
        }
    }
}
