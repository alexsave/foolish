// LiveArrivalNothingBubbleTests - NOTE 6, the BOARD half: the bubble an
// invalidating arrival leaves in the Messages input field is overwritten with
// the Undo's NOTHING bubble, through the board's own `onSend` - the closure
// that reaches MessagesViewController.stage.
//
// LiveArrival4pTests pins the controller half (the kernel's verdict, the debt,
// the bytes, and what sending them does). This mounts the REAL board on that
// controller and records every `onSend` it makes, because the seam that
// matters to the human is the one the extension inserts from: which payload,
// and whether it is staged the way an Undo is (`fromUndo` true - insert at
// once, stay expanded, no collapse) rather than as a fresh move.

import XCTest
import SwiftUI
@testable import FoolishKit

@MainActor
final class LiveArrivalNothingBubbleTests: XCTestCase {

    private var window: UIWindow?

    override func tearDown() async throws {
        window?.isHidden = true
        window?.rootViewController = nil
        window = nil
        try? await Task.sleep(nanoseconds: 300_000_000)
        try await super.tearDown()
    }

    /// Every bubble the board hands the extension, in order.
    @MainActor final class Sends {
        var calls: [(payload: Data, fromUndo: Bool)] = []
    }

    private func mount(_ c: MessageTurnController) throws -> Sends {
        let scene = try XCTUnwrap(UIApplication.shared.connectedScenes
            .compactMap { $0 as? UIWindowScene }.first, "needs the app test host")
        let sends = Sends()
        let w = UIWindow(windowScene: scene)
        w.rootViewController = UIHostingController(
            rootView: MessageTableView(controller: c, onSend: { payload, fromUndo in
                sends.calls.append((payload, fromUndo))
            }))
        w.makeKeyAndVisible()
        window = w
        return sends
    }

    private func waitFor(_ what: String, seconds: Double = 10,
                         _ ok: () -> Bool) async {
        let deadline = Date().addingTimeInterval(seconds)
        while Date() < deadline {
            if ok() { return }
            try? await Task.sleep(nanoseconds: 50_000_000)
        }
        XCTFail("timed out waiting for \(what)")
    }

    /// The staged good, the throw-in that invalidates it, and the board that
    /// overwrites the field: the LAST bubble the board hands over is the
    /// NOTHING reseal of the throw-in - byte for byte the one an Undo-to-empty
    /// on it stages - staged the Undo's way.
    func testAnInvalidatingArrivalStagesTheUndosNothingBubbleThroughOnSend() async throws {
        let t = try await LiveArrival4pTests.staleGood(watching: false)
        let sends = try mount(t.board)
        // The board's mount re-stages what the controller holds - the good.
        await waitFor("the staged good's bubble") { sends.calls.count >= 1 }
        let before = sends.calls.count
        XCTAssertEqual(sends.calls.last?.fromUndo, false, "the good was staged as a move")

        await t.f.arrive(t.x2, at: t.board)
        XCTAssertEqual(t.board.basePayload, t.x2.payload, "the throw-in was adopted")
        await waitFor("the NOTHING bubble") { sends.calls.count > before }

        let last = try XCTUnwrap(sends.calls.last)
        XCTAssertEqual(sends.calls.count, before + 1, "exactly one bubble replaces the stale good")
        XCTAssertTrue(last.fromUndo, "staged the Undo's way: at once, expanded, no collapse")
        let env = try await MessageEnvelope.decode(payload: last.payload, viewer: -1)
        XCTAssertEqual(env.newAtoms, MessageEnvelope.newAtomsNothing, "a no-op, not a move")
        XCTAssertEqual(MessageTurnController.firstEight(hex: env.parent8), t.x2.digest8,
                       "over the arrival")
        let undos = try await LiveArrival4pTests.undosNothingBubble(t.f, seat: t.stager, on: t.x2)
        XCTAssertEqual(last.payload, undos, "the SAME bubble an Undo-to-empty stages")
        XCTAssertFalse(t.board.nothingBubbleOwed, "the debt is paid")
    }

    /// THE CONTROL: an arrival that does not touch the staged move (the chain
    /// it was built on, delivered again) hands the extension nothing new.
    func testARedeliveryHandsTheExtensionNothing() async throws {
        let t = try await LiveArrival4pTests.staleGood(watching: false)
        let sends = try mount(t.board)
        await waitFor("the staged good's bubble") { sends.calls.count >= 1 }
        let before = sends.calls.count
        await t.f.arrive(t.x1, at: t.board)
        try? await Task.sleep(nanoseconds: 1_000_000_000)
        XCTAssertEqual(sends.calls.count, before, "nothing was re-staged")
        XCTAssertEqual(t.board.pending.count, 1, "the staged good stands")
    }
}
