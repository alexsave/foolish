// StagedCancelTests — the X on the staged bubble (1.0(37)).
//
// THE BUG. Playing a move auto-stages it into Messages' input field, and the
// bubble Messages inserts carries an X. Pressing it deletes the bubble, and the
// board went on believing it had a move to send: the blue send-hint arrow kept
// bobbing over a compose bar with nothing in it, the Undo pill stayed, and every
// play button stayed disabled (`acting = iCanAct && !canSend`), so the only way
// out was to press an Undo the player had no reason to look for. Owner: "if I
// stage then X the staged bubble, the 'send' hint arrow doesn't go away."
//
// The host callback was never the missing piece - `didCancelSending` was
// implemented and already bumped a `cancelToken`. What that token reached was
// `GameSurface.surfaceStaged`, which is the LOBBY's half of the hint (join /
// invite / start bubbles). The BOARD's half is `controller.canSend`, off the
// controller's own pending list, and nothing cleared it.
//
// WHAT THE CANCEL MEANS (owner): "X-ing the staged bubble should be the SAME as
// hitting the undo button. If the player has ALREADY undone via the button, then
// X-ing the bubble is a NO-OP." The rule is the kernel's (`msg_turn_cancel`,
// c/src/msg_wire.c); these tests drive it through the controller that performs
// it, so a green run means the two agree.
//
// WHAT THESE FAIL AGAINST (the mutation matrix, every line of it actually run):
//   the .noop arm undoes anyway and answers .clear      -> testXing…AfterAnUndo
//   cancel undoes EVERYTHING staged, not one move       -> testXingWithTwoMoves…
//   the last staged move answers .restage (bubble back) -> testXingTheStagedBubble…
//   the kernel gate bypassed: always undo, always clear -> both of the above
//
// WHAT THEY DO NOT REACH, said plainly rather than implied: the view wiring that
// carries the host's `cancelToken` into `MessageTableView.cancelStagedBubble`.
// That is a SwiftUI `.onChange` on a rendered board, and nothing here renders
// one - it is proved in the harness app instead (FoolishHarness: stage a move,
// press the X on the staged bubble, watch the arrow and the Undo pill go).
import XCTest
@testable import FoolishKit

@MainActor
final class StagedCancelTests: XCTestCase {

    // The same 2p mid-game chain MessageTurnControllerTests adopts (turn 7,
    // round 1), sealed by the native kernel.
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

    /// A controller sitting on the fixture, seated at a seat that can attack,
    /// plus that seat's opening attack.
    private func attacker() async throws -> (MessageTurnController, Move, Int) {
        let parentBytes = bytes(fixtureHex)
        let parent = try await MessageEnvelope.decode(payload: parentBytes, viewer: 0)
        for seat in 0..<parent.nPlayers {
            let menu = await MessageKernel.shared.residentLegal(seat: seat)
            if let atk = menu.first(where: { $0.type == .attack }) {
                let c = MessageTurnController(parentPayload: parentBytes,
                                              parent: parent, mySeat: seat)
                await c.refresh()
                return (c, atk, seat)
            }
        }
        throw XCTSkip("no seat in the fixture could attack")
    }

    // MARK: - the symptom

    /// STAGE -> X. The move comes back off the chain and the send hint goes out
    /// with it, because the hint is drawn off exactly this `canSend`.
    func testXingTheStagedBubbleTakesTheMoveBackAndPutsTheSendHintOut() async throws {
        let (c, attack, _) = try await attacker()
        let handBefore = c.view?.me?.handCount ?? -1
        XCTAssertGreaterThanOrEqual(handBefore, 0)

        await c.apply(attack)
        XCTAssertEqual(c.pending.count, 1, "the played move is staged")
        XCTAssertTrue(c.canSend, "…and the send hint is up, which is the whole point of a stage")

        let outcome = await c.cancelStage()

        XCTAssertEqual(outcome, .clear,
                       "nothing is staged now, so NO bubble goes back into the input field - "
                       + "the human just deleted the one there was")
        XCTAssertTrue(c.pending.isEmpty, "the staged move was taken back, exactly as Undo takes it")
        XCTAssertFalse(c.canSend, "THE SEND HINT IS OUT - this is the reported bug")
        await c.refresh()
        XCTAssertEqual(c.view?.me?.handCount, handBefore, "and the cards are back in my hand")
    }

    /// The same X, reached by the OTHER order the owner named: stage, press the
    /// Undo pill, then X the base bubble the pill had to leave behind. Nothing
    /// of mine is staged by then, so the cancel must not reach into the game -
    /// and the hint must still be out afterwards.
    func testXingTheBubbleAfterAnUndoIsANoOpAndLeavesTheHintOut() async throws {
        let (c, attack, _) = try await attacker()
        let handBefore = c.view?.me?.handCount ?? -1

        await c.apply(attack)
        await c.undo()                      // the pill
        XCTAssertTrue(c.pending.isEmpty)
        XCTAssertFalse(c.canSend)
        await c.refresh()
        let handAfterUndo = c.view?.me?.handCount ?? -1
        XCTAssertEqual(handAfterUndo, handBefore, "the pill already took the move back")

        let outcome = await c.cancelStage()

        XCTAssertEqual(outcome, .noop,
                       "IDEMPOTENT: the bubble the human deleted carried the BASE state, so the "
                       + "cancel owes the input field nothing and owes the game nothing. A "
                       + ".clear here is a cancel that ran an undo it was not entitled to run, "
                       + "and it would send the board back to `onUnstage` for a bubble that is "
                       + "already gone.")
        XCTAssertTrue(c.pending.isEmpty, "still nothing staged")
        XCTAssertFalse(c.canSend, "the hint stays out")
        await c.refresh()
        XCTAssertEqual(c.view?.me?.handCount, handAfterUndo,
                       "and the board is EXACTLY where the undo left it - no second move came back")
    }

    /// TWO ACTIONS STAGED AT ONCE - the defender of a two-card attack, covering
    /// one card and then the other. One X takes back ONE of them, and the
    /// shorter chain that survives still needs a bubble: the answer is
    /// `.restage`, not `.clear`, and the hint stays UP because there is still
    /// something to send.
    func testXingWithTwoMovesStagedTakesBackExactlyOneAndAsksForARestage() async throws {
        let parentBytes = bytes(fixtureHex)
        let parent = try await MessageEnvelope.decode(payload: parentBytes, viewer: 0)

        // Search rather than hard-code the seat: what is being pinned is the
        // DEPTH of the staged list, not which seat in this fixture happens to
        // afford it. A hard-coded seat that stops affording two actions turns
        // this test into a skip, and a skip is a green run that proved nothing.
        var found: (MessageTurnController, Int)?
        outer: for seat in 0..<parent.nPlayers {
            let menu = await MessageKernel.shared.residentLegal(seat: seat)
            for first in menu where first.type != .wait {
                let c = MessageTurnController(parentPayload: parentBytes,
                                              parent: parent, mySeat: seat)
                await c.refresh()
                await c.apply(first)
                guard let second = c.legal.first(where: { $0.type != .wait && $0.type != .good })
                else { continue }
                await c.apply(second)
                if c.pending.count == 2 { found = (c, seat); break outer }
            }
        }
        guard let (c, _) = found else {
            return XCTFail("no seat in the fixture could stage two actions - this test needs one")
        }
        XCTAssertTrue(c.canSend)

        let outcome = await c.cancelStage()

        XCTAssertEqual(outcome, .restage,
                       "the first cover is still staged, and Apple gives no way to leave the "
                       + "field empty for it - the shorter chain needs a bubble")
        XCTAssertEqual(c.pending.count, 1,
                       "EXACTLY ONE move came back. A cancel that undoes everything staged is a "
                       + "second walk back that has to agree with the pill's, and would not.")
        XCTAssertTrue(c.canSend, "so the send hint is still up, over the bubble the restage puts in")
    }

    // MARK: - the kernel's own gate, through the bridge

    /// The rule lives in C (`msg_turn_cancel`) and these are the answers the
    /// controller switches on, asked through the real bridge rather than
    /// re-derived here. A cancel is refused inside the send window and inside a
    /// retraction for the same reasons `undo()` and `canSend` refuse them: the
    /// bytes are already on their way, and a retraction IS an undo of everything
    /// staged that is already in flight.
    func testTheCancelGateIsTheKernels() {
        let live: TurnWire.State = [.ready]
        XCTAssertEqual(TurnWire.cancel(live.union(.staged), pending: 1), .clear)
        XCTAssertEqual(TurnWire.cancel(live.union(.staged), pending: 3), .restage)
        XCTAssertEqual(TurnWire.cancel(live, pending: 0), .noop)
        XCTAssertEqual(TurnWire.cancel([.ready, .staged, .sending], pending: 1), .noop,
                       "the send window has already claimed those bytes")
        XCTAssertEqual(TurnWire.cancel([.ready, .staged, .retracting], pending: 2), .noop,
                       "a retraction is an undo of everything staged, already flying")
        XCTAssertEqual(TurnWire.cancel([.ready, .genesis], pending: 0), .noop,
                       "a genesis deal is stageable with nothing pending, and X-ing it is still "
                       + "not an undo of a move nobody made")
    }
}
