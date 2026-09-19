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
        // LIKE A COIN: width = cos of an angle swept at a steady rate. Owner:
        // "it should be like a cosine wave shape, right?"
        for u in [0.25, 0.5, 0.75] {
            let f = p.frame(at: roleFlipCollapse * u)
            XCTAssertEqual(f.face, .sword)
            XCTAssertEqual(Double(f.scale), cos(u * .pi / 2), accuracy: 0.03,
                           "the coin is not turning like a coin at \(u): \(f.scale)")
        }
        let mid = p.frame(at: roleFlipCollapse / 2)
        XCTAssertTrue(mid.scale > 0.1 && mid.scale < 0.9, "the sword is not seen narrowing: \(mid.scale)")
        let edge = p.frame(at: roleFlipCollapse + roleFlipSettle / 2)
        XCTAssertEqual(edge.face, .sword, "the face swapped before the coin was edge-on")
        XCTAssertLessThan(edge.scale, 0.01)
        let opening = p.frame(at: roleFlipHalf + roleFlipHalf / 2)
        XCTAssertEqual(opening.face, .check)
        XCTAssertEqual(Double(opening.scale), sin(0.5 * .pi / 2), accuracy: 0.03,
                       "the check is not opening like a coin")
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

    /// The clock counts DRAWN frames: it starts at zero on a phase's first draw,
    /// and a gap where the board drew nothing is worth at most two frames.
    /// MUTANTS: the gap counted in full (the 35 -> 10 -> 0 jump); the cap
    /// applied to an ordinary 16ms frame.
    func testTheClockCountsDrawnFramesAndCapsAGap() {
        let c = RoleCoinClock()
        let t0 = Date(timeIntervalSince1970: 100)
        XCTAssertEqual(c.tick(1, at: t0), 0, "a phase starts at zero on its first drawn frame")
        XCTAssertEqual(c.tick(1, at: t0.addingTimeInterval(1.0 / 60)), 1.0 / 60, accuracy: 1e-4)
        // The board stopped drawing for 200ms: the turn waits, it does not skip.
        XCTAssertEqual(c.tick(1, at: t0.addingTimeInterval(1.0 / 60 + 0.2)),
                       1.0 / 60 + RoleCoinClock.cap, accuracy: 1e-4)
        XCTAssertLessThanOrEqual(RoleCoinClock.cap, 1.0 / 60 + 1e-9,
                                 "a drawn frame may carry at most one frame of the turn")
        XCTAssertLessThan(RoleCoinClock.cap, roleFlipCollapse / 2,
                          "a capped gap must not swallow half the collapse")
        XCTAssertEqual(c.tick(2, at: t0.addingTimeInterval(5)), 0, "a new phase starts again")
        XCTAssertNil(c.elapsed(1))
    }

    /// Ships on, and a coin made without saying otherwise plays on the frame clock.
    /// MUTANTS: `shipping.fromFirstFrame = false`; the coin's default motion not `.live`.
    /// The flag's other state keeps the old quadratic ease-in, which is close to
    /// the coin's cosine but not it - the blink the owner saw was the frames the
    /// board never drew (see RoleCoinClock), not this curve.
    func testTheFlagsOtherStateIsTheOldEaseIn() {
        let eased = RoleCoinPhase(id: 1, from: .sword, to: .check, delay: 0, coin: false)
        XCTAssertEqual(Double(eased.frame(at: roleFlipCollapse * 0.5).scale), 0.75, accuracy: 0.01)
        let coin = RoleCoinPhase(id: 1, from: .sword, to: .check, delay: 0)
        XCTAssertEqual(Double(coin.frame(at: roleFlipCollapse * 0.5).scale), cos(Double.pi / 4), accuracy: 0.01)
    }

    func testTheFixShipsOnAndCoinsUseIt() {
        XCTAssertTrue(RoleCoinMotion.shipping.fromFirstFrame)
        XCTAssertTrue(RoleCoinMotion.shipping.passDelay)
        XCTAssertTrue(RoleCoinMotion.shipping.syncFlightSeats)
        XCTAssertTrue(RoleCoinMotion.shipping.coinTurn)
        #if DEBUG || SOLO_TESTING
        XCTAssertEqual(RoleCoinMotion.live, .shipping, "a debug build with no dev.flags runs what ships")
        #endif
        XCTAssertEqual(FRoleCoin(kind: .sword).motion, .shipping)
    }
}

/// A seat a mark is FLYING TO shows nothing until it lands - never a second
/// copy of the mark that is still in the air.
///
/// Owner, watching a pass on the simulator: "at one point there are two shields.
/// Wtf" - and the same on its Undo. The roles change ~10ms BEFORE the flight is
/// planned (the board publishes the view, then plans the hand-off), so the
/// receiving seat starts an ordinary flip to the shield and stands it up about
/// 220ms later, while the ghost is still sailing across a 400ms flight.
@MainActor
final class RoleCoinArrivalTests: XCTestCase {

    private final class Seat: ObservableObject {
        @Published var kind: RoleMarkKind?
        @Published var arriving = false
        init(_ kind: RoleMarkKind?) { self.kind = kind }
    }

    private struct Host: View {
        @ObservedObject var seat: Seat
        var body: some View {
            FRoleCoin(kind: seat.kind, arriving: seat.arriving)
                .frame(width: 60, height: 60)
                .background(Color.black)
        }
    }

    /// (white columns, green columns): a shield is white, a check green - so the
    /// seat's OWN mark and the arriving one can be told apart.
    private func ink(_ window: UIWindow) -> (white: Int, green: Int) {
        let image = UIGraphicsImageRenderer(size: CGSize(width: 60, height: 60)).image { _ in
            window.drawHierarchy(in: window.bounds, afterScreenUpdates: false)
        }
        guard let cg = image.cgImage else { return (0, 0) }
        let w = cg.width, h = cg.height
        var px = [UInt8](repeating: 0, count: w * h * 4)
        guard let ctx = CGContext(data: &px, width: w, height: h, bitsPerComponent: 8,
                                  bytesPerRow: w * 4, space: CGColorSpaceCreateDeviceRGB(),
                                  bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)
        else { return (0, 0) }
        ctx.draw(cg, in: CGRect(x: 0, y: 0, width: w, height: h))
        var white = 0, green = 0
        for x in 0..<w {
            var sawW = false, sawG = false
            for y in 0..<h {
                let i = (y * w + x) * 4
                let r = Int(px[i]), g = Int(px[i + 1]), b = Int(px[i + 2])
                if r > 190 && g > 190 && b > 190 { sawW = true }
                if g > 100 && r < 140 && b < 140 && g - r > 30 { sawG = true }
            }
            white += sawW ? 1 : 0; green += sawG ? 1 : 0
        }
        return (white, green)
    }

    /// MUTANT: the arriving seat turning away the mark the interrupted flip was
    /// heading TO (which is the arriving mark itself, so it stands up instead).
    func testTheSeatAShieldIsFlyingToDrawsNothingWhileItFlies() throws {
        let scene = try XCTUnwrap(UIApplication.shared.connectedScenes
            .compactMap { $0 as? UIWindowScene }.first, "needs the app test host")
        // The seat is wearing a CHECK (green) and a shield (white) is flying to
        // it, so a second shield is white ink and the seat's own mark is not.
        let seat = Seat(.check)
        let window = UIWindow(windowScene: scene)
        window.frame = CGRect(x: 0, y: 0, width: 60, height: 60)
        window.rootViewController = UIHostingController(rootView: Host(seat: seat))
        window.makeKeyAndVisible()
        defer { window.isHidden = true }
        RunLoop.current.run(until: Date().addingTimeInterval(0.4))
        XCTAssertGreaterThan(ink(window).green, 3, "no check to start with")

        // The board's order: the roles publish, THEN the flight is planned.
        seat.kind = .shield
        RunLoop.current.run(until: Date().addingTimeInterval(0.012))
        seat.arriving = true

        // Half the flight later - long past a flip - the seat must be empty: the
        // shield on screen is the one in the air.
        var seen: [(Int, Int)] = []
        let end = Date().addingTimeInterval(roleFlightTime * 0.75)
        while Date() < end {
            RunLoop.current.run(until: Date().addingTimeInterval(1.0 / 60))
            seen.append(ink(window))
        }
        XCTAssertEqual(seen.map(\.0).max(), 0,
                       "a second shield stood up while the first was still flying: \(seen)")

        // The ghost lands: the real shield stands up in that frame.
        seat.arriving = false
        RunLoop.current.run(until: Date().addingTimeInterval(0.05))
        XCTAssertGreaterThan(ink(window).white, 3, "the shield never landed")
    }

    /// ONE update starts the whole hand-off: the ghost and both seats. Owner, on
    /// the frame he caught at the release: "THERE SHOULD ALWAYS BE EXACTLY ONE
    /// SHIELD... it seems to blink out for a single frame when we release the
    /// card that was dragged" - the departing seat had been blanked an update
    /// before the ghost existed, so the board had no shield on it at all.
    /// MUTANT: the ghost set in the Task, the seats set here.
    func testTheGhostAndTheSeatsAreOneUpdate() throws {
        let board = try BoardSource.text()
        let begin = try XCTUnwrap(board.range(of: "private func beginRoleFlights"))
        let body = String(board[begin.lowerBound...].prefix(600))
        for line in ["roleDepartingSeats = Set(f.map(", "roleArrivingSeats = Set(f.map(",
                     "roleFlights = f", "roleProgress = 0"] {
            XCTAssertTrue(body.contains(line), "the hand-off is split up again: \(line) left beginRoleFlights")
        }
        XCTAssertTrue(board.contains("if RoleCoinMotion.live.syncFlightSeats { beginRoleFlights(flights) }"),
                      "the hand-off no longer begins in the update that changed the roles")
    }
}
