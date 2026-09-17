import SwiftUI
import XCTest
@testable import FoolishKit

/// A role mark's gesture is timed from the first frame that DRAWS it.
///
/// Owner, filming a pass and its Undo: "the sword of the player who will become
/// the attacker as a result of the pass/undo seems to pop in too early rather
/// than rotating in, thus briefly being shown under the shield" - "sword should
/// rotate in by the way". And on an 8-seat Undo the swords did not turn back to
/// checks: "It definitely does not go to width zero."
///
/// Measured off the rig's film (ink columns of Kate's badge, one entry a frame):
///
///     before   w35 ... w35, then g30 g28 g30 g34                   no collapse
///     after    w35, w19 w14 w0, g7 g16 g22 g26 g30 g32 g34          the whole coin
///
/// while the log had every coin starting a proper flip both times. The coin ran
/// its gestures on the wall clock from the moment the mark changed, and after a
/// heavy update the collapsed board does not draw for a beat - which a 110ms
/// half flip fits inside. A unit window does not reproduce that beat (a main
/// thread stall does not: SwiftUI starts an animation at its commit), so the
/// device measurement is the rig's film, and these hold the pieces: the
/// gesture's shape as a function of drawn time, the clock that starts on the
/// first draw, and the coin using both by default.
@MainActor
final class RoleCoinRotateInTests: XCTestCase {

    /// A flip: the old face narrows, sits edge-on for round 30's settle, and the
    /// new face opens - as a function of time since the first drawn frame.
    /// MUTANTS: the edge-on beat removed; the faces swapped at the wrong half.
    func testAFlipNarrowsSitsEdgeOnAndOpens() {
        let p = RoleCoinPhase(id: 1, from: .sword, to: .check, delay: 0)
        XCTAssertEqual(p.frame(at: 0).face, .sword)
        XCTAssertEqual(p.frame(at: 0).scale, 1, accuracy: 0.01)
        let mid = p.frame(at: roleFlipCollapse / 2)
        XCTAssertEqual(mid.face, .sword)
        XCTAssertTrue(mid.scale > 0.1 && mid.scale < 0.9, "the sword is not seen narrowing: \(mid.scale)")
        let edge = p.frame(at: roleFlipCollapse + roleFlipSettle / 2)
        XCTAssertEqual(edge.face, .sword, "the face swapped before the coin was edge-on")
        XCTAssertLessThan(edge.scale, 0.01)
        let opening = p.frame(at: roleFlipHalf + roleFlipHalf / 2)
        XCTAssertEqual(opening.face, .check)
        XCTAssertTrue(opening.scale > 0.1 && opening.scale < 0.95, "the check is not seen opening: \(opening.scale)")
        XCTAssertEqual(p.frame(at: p.total + 0.01).face, .check)
        XCTAssertEqual(p.frame(at: p.total + 0.01).scale, 1, accuracy: 0.01)
        XCTAssertEqual(p.total, 2 * roleFlipHalf, accuracy: 1e-9)
    }

    /// A pass's previous defender: nothing at the seat while the shield clears
    /// it, then the sword comes round from edge-on - never a full sword first.
    /// MUTANTS: the delay ignored; a rotate-in that starts at full width.
    func testAPassSwordWaitsBlankThenTurnsIn() {
        let d = RoleCoinMotion.passSwordDelay
        let p = RoleCoinPhase(id: 1, from: nil, to: .sword, delay: d)
        XCTAssertNil(p.frame(at: 0).face)
        XCTAssertNil(p.frame(at: d - 0.01).face, "the sword showed while the shield was over the seat")
        let first = p.frame(at: d + 0.001)
        XCTAssertEqual(first.face, .sword)
        XCTAssertLessThan(first.scale, 0.1, "the sword popped in instead of turning in")
        let half = p.frame(at: d + roleFlipHalf / 2)
        XCTAssertTrue(half.scale > 0.1 && half.scale < 0.95)
        XCTAssertEqual(p.total, d + roleFlipHalf, accuracy: 1e-9)
        XCTAssertGreaterThan(d, 0.1, "the shield needs a real beat to clear a 32pt row")
    }

    /// A seat making way for a mark in flight holds its mark, then turns it away.
    func testMakingWayHoldsThenTurnsAway() {
        let p = RoleCoinPhase(id: 1, from: .sword, to: nil, delay: 0.2)
        XCTAssertEqual(p.frame(at: 0.19).face, .sword)
        XCTAssertEqual(p.frame(at: 0.19).scale, 1, accuracy: 0.01)
        XCTAssertNil(p.frame(at: p.total + 0.01).face)
    }

    /// The clock starts on the first draw of a phase, and a new phase restarts it.
    /// MUTANT: every draw moving the start.
    func testTheClockStartsOnTheFirstDrawnFrame() {
        let c = RoleCoinClock()
        let t0 = Date(timeIntervalSince1970: 100)
        XCTAssertNil(c.started(1))
        XCTAssertEqual(c.start(for: 1, at: t0), t0)
        XCTAssertEqual(c.start(for: 1, at: t0.addingTimeInterval(0.5)), t0, "a later frame moved the start")
        XCTAssertEqual(c.started(1), t0)
        XCTAssertEqual(c.start(for: 2, at: t0.addingTimeInterval(1)), t0.addingTimeInterval(1))
        XCTAssertNil(c.started(1))
    }

    /// Ships on, and a coin made without saying otherwise plays on the frame clock.
    /// MUTANTS: `shipping.fromFirstFrame = false`; the coin's default motion not `.live`.
    func testTheFixShipsOnAndCoinsUseIt() {
        XCTAssertTrue(RoleCoinMotion.shipping.fromFirstFrame)
        XCTAssertTrue(RoleCoinMotion.shipping.passDelay)
        #if DEBUG || SOLO_TESTING
        XCTAssertEqual(RoleCoinMotion.live, .shipping, "a debug build with no dev.flags runs what ships")
        #endif
        XCTAssertEqual(FRoleCoin(kind: .sword).motion, .shipping)
    }
}
