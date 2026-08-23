// The role marks MOVE (round 16) - the rules, without a board.
//
// Three things decide what a player sees when a role changes, and all three are
// pure: which mark a seat wears, which marks TRAVEL when the roles rotate, and
// which gesture a mark that stays put makes. The motion itself (a `withAnimation`
// over a published `progress`) is SwiftUI's; these are the decisions it plays.
//
// Worth pinning because every one of them is a claim about MEANING that a
// plausible-looking alternative would break silently: a shield that fades out
// here and fades in there is the same information and a worse board, and it
// would never fail a test that only checked which mark each seat ends up with.

import XCTest
@testable import FoolishKit

final class RoleMotionTests: XCTestCase {

    private func pads(_ seats: [Int: CGPoint]) -> [Int: CGRect] {
        seats.mapValues { CGRect(x: $0.x - 20, y: $0.y - 20, width: 40, height: 40) }
    }

    private let ring = [0: CGPoint(x: 180, y: 520),   // me, under my hand
                        1: CGPoint(x: 320, y: 300),
                        2: CGPoint(x: 180, y: 90),
                        3: CGPoint(x: 40,  y: 300)]

    // MARK: - which mark a seat wears

    func testASeatWearsExactlyOneMark() {
        // The kernel rejects a defender's `good` (game.c handle_good) and the
        // board stands the sword down for a seat that has said it, so these are
        // the only combinations reachable - and each has one answer.
        XCTAssertEqual(FSeatBadge(name: "a", handCount: 6, isDefender: true).mark, .shield)
        XCTAssertEqual(FSeatBadge(name: "a", handCount: 6, isAttacker: true).mark, .sword)
        XCTAssertEqual(FSeatBadge(name: "a", handCount: 6, saidGood: true).mark, .check)
        XCTAssertNil(FSeatBadge(name: "a", handCount: 6).mark)
        // A seat that said good is wearing the check, whatever else is set - the
        // check is the newest thing that happened to it.
        XCTAssertEqual(FSeatBadge(name: "a", handCount: 6, isAttacker: true, saidGood: true).mark,
                       .check)
    }

    // MARK: - which marks travel

    func testTheShieldFliesToTheNextDefender() {
        let f = MessageTableView.roleFlights(
            from: .init(defender: 1, firstAttacker: 0),
            to:   .init(defender: 2, firstAttacker: 0),
            pads: pads(ring))
        XCTAssertEqual(f.count, 1, "only the defender changed, so only the shield travels")
        XCTAssertEqual(f[0].kind, .shield)
        XCTAssertEqual(f[0].fromSeat, 1)
        XCTAssertEqual(f[0].toSeat, 2)
        XCTAssertEqual(f[0].from, ring[1]!)
        XCTAssertEqual(f[0].to, ring[2]!)
    }

    func testTheSwordIsHandedToTheNextFirstAttacker() {
        let f = MessageTableView.roleFlights(
            from: .init(defender: 1, firstAttacker: 0),
            to:   .init(defender: 2, firstAttacker: 1),
            pads: pads(ring))
        XCTAssertEqual(f.count, 2, "a bout end moves both")
        XCTAssertEqual(Set(f.map(\.kind)), [.shield, .sword])
        let sword = f.first { $0.kind == .sword }
        XCTAssertEqual(sword?.fromSeat, 0)
        XCTAssertEqual(sword?.toSeat, 1)
    }

    func testTheSwordLeavesTheOldOpenerEvenWhileTheyWearACheck() {
        // The seat that opened the bout has said good, so what is drawn under it
        // right now is a CHECK. What travels is the right to open, not the glyph
        // that happens to be on screen - the sword still takes off from there.
        let f = MessageTableView.roleFlights(
            from: .init(defender: 1, firstAttacker: 0, goodMask: 1 << 0),
            to:   .init(defender: 2, firstAttacker: 1, goodMask: 0),
            pads: pads(ring))
        let sword = f.first { $0.kind == .sword }
        XCTAssertNotNil(sword)
        XCTAssertEqual(sword?.fromSeat, 0)
    }

    func testSayingGoodTravelsNowhere() {
        // The commonest role change of all: an attacker says good. Nobody
        // received anything, so nothing may fly - the mark turns over where it
        // stands (the coin flip, below).
        let f = MessageTableView.roleFlights(
            from: .init(defender: 1, firstAttacker: 0, goodMask: 0),
            to:   .init(defender: 1, firstAttacker: 0, goodMask: 1 << 2),
            pads: pads(ring))
        XCTAssertTrue(f.isEmpty)
    }

    func testAMarkWithNowhereToTakeOffFromDoesNotFly() {
        // A seat that just went out stops drawing a mark, so its pad never
        // publishes. The roles still change; the mark just changes in place.
        var half = pads(ring); half[1] = nil
        let f = MessageTableView.roleFlights(
            from: .init(defender: 1, firstAttacker: 0),
            to:   .init(defender: 2, firstAttacker: 0),
            pads: half)
        XCTAssertTrue(f.isEmpty)
        // A zero rect is the same thing said differently (a frame that has been
        // published but not laid out yet), and must not be flown to either.
        var zeroed = pads(ring); zeroed[2] = .zero
        XCTAssertTrue(MessageTableView.roleFlights(
            from: .init(defender: 1, firstAttacker: 0),
            to:   .init(defender: 2, firstAttacker: 0),
            pads: zeroed).isEmpty)
    }

    func testTheSwordSpinsAndTheShieldDoesNot() {
        // Not decoration: a full turn says the sword was THROWN to someone, and
        // the shield's lean says it was carried. Swapping them reads wrong.
        let f = MessageTableView.roleFlights(
            from: .init(defender: 1, firstAttacker: 0),
            to:   .init(defender: 2, firstAttacker: 1),
            pads: pads(ring))
        XCTAssertEqual(f.first { $0.kind == .sword }?.spin, 360)
        XCTAssertLessThan(f.first { $0.kind == .shield }?.spin ?? 999, 90)
    }

    // MARK: - the path

    func testAFlightStartsAtOneMarkAndLandsOnTheOther() {
        let f = MessageTableView.roleFlights(
            from: .init(defender: 1, firstAttacker: 0),
            to:   .init(defender: 3, firstAttacker: 0),
            pads: pads(ring))[0]
        XCTAssertEqual(f.point(at: 0), f.from, "a hand-off begins on the mark it takes")
        XCTAssertEqual(f.point(at: 1), f.to, "and ends on the badge it is given to")
    }

    func testTheFlightArcsOverTheTable() {
        // Across the ring, not through it: the midpoint of the path sits ABOVE
        // the straight line between the two seats. Seats 1 and 3 face each other
        // at the same height, so the straight line is flat and any lift shows.
        let f = MessageTableView.roleFlights(
            from: .init(defender: 1, firstAttacker: 0),
            to:   .init(defender: 3, firstAttacker: 0),
            pads: pads(ring))[0]
        let flat = (f.from.y + f.to.y) / 2
        XCTAssertLessThan(f.point(at: 0.5).y, flat - 20, "the throw lifts over the table")
        // Neighbouring seats get a hop, not the same sail: the lift scales with
        // the distance travelled.
        let short = MessageTableView.roleFlights(
            from: .init(defender: 1, firstAttacker: 0),
            to:   .init(defender: 2, firstAttacker: 0),
            pads: pads([1: CGPoint(x: 180, y: 300), 2: CGPoint(x: 220, y: 300)]))[0]
        let shortLift = 300 - short.point(at: 0.5).y
        let longLift = flat - f.point(at: 0.5).y
        XCTAssertLessThan(shortLift, longLift)
    }

    // MARK: - the gesture a mark that stays put makes

    func testAMarkThatBecomesAnotherMarkFlips() {
        // The owner's ask, exactly: a sword that says good turns over into the
        // check rather than dissolving into it.
        XCTAssertEqual(RoleGesture.between(.sword, .check), .flip)
        XCTAssertEqual(RoleGesture.between(.shield, .sword), .flip)
    }

    func testARoleThatSimplyEndsFades() {
        XCTAssertEqual(RoleGesture.between(.sword, nil), .fadeOut)
        XCTAssertEqual(RoleGesture.between(.check, nil), .fadeOut)
        XCTAssertEqual(RoleGesture.between(nil, .sword), .fadeIn)
    }

    func testNothingHappeningIsNotAGesture() {
        XCTAssertEqual(RoleGesture.between(.sword, .sword), .none)
        XCTAssertEqual(RoleGesture.between(nil, nil), .none)
    }
}

// The bubble's own picture is drawn by ImageRenderer, which does NOT run
// appearance callbacks - and the role marks are now behind a view that holds
// state (FRoleCoin, so it can show one face for the first half of a flip and
// the other for the second). A mark seeded in `onAppear` would have been
// missing from every bubble in the transcript and from nowhere else, which is
// exactly the kind of defect this codebase already carries scars from: the
// marks are hand-drawn rather than SF Symbols for the same reason.
//
// So: render the real bubble picture with and without a shield and count the
// ink. This is a claim about the RENDERER, not about layout, so it asserts a
// difference rather than an image.
import SwiftUI
import UIKit

final class RoleMarkSnapshotTests: XCTestCase {

    /// Pixels that are not the black ground. The name and the mini fan are
    /// identical in every badge below, so the DIFFERENCE from a bare one is the
    /// mark and nothing else - which is what makes this a claim about whether
    /// the mark was drawn at all, rather than about how it looks.
    private func ink(_ image: UIImage?) -> Int {
        guard let cg = image?.cgImage else { return 0 }
        let w = cg.width, h = cg.height
        var bytes = [UInt8](repeating: 0, count: w * h * 4)
        guard let ctx = CGContext(data: &bytes, width: w, height: h, bitsPerComponent: 8,
                                  bytesPerRow: w * 4,
                                  space: CGColorSpaceCreateDeviceRGB(),
                                  bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)
        else { return 0 }
        ctx.draw(cg, in: CGRect(x: 0, y: 0, width: w, height: h))
        var n = 0
        for i in stride(from: 0, to: bytes.count, by: 4) where
            bytes[i] > 40 || bytes[i + 1] > 40 || bytes[i + 2] > 40 { n += 1 }
        return n
    }

    @MainActor
    private func render(_ badge: FSeatBadge) -> UIImage? {
        let r = ImageRenderer(content: badge.frame(width: 120, height: 120)
                                          .background(Color.black)
                                          .environment(\.colorScheme, .dark))
        r.scale = 2
        return r.uiImage
    }

    @MainActor
    func testTheRoleMarkSurvivesTheBubbleSnapshot() {
        let bare = ink(render(FSeatBadge(name: "Vera", handCount: 6)))
        let shield = ink(render(FSeatBadge(name: "Vera", handCount: 6, isDefender: true)))
        let sword = ink(render(FSeatBadge(name: "Vera", handCount: 6, isAttacker: true)))
        let check = ink(render(FSeatBadge(name: "Vera", handCount: 6, saidGood: true)))
        // Each mark is tens of points across at 2x, so a drawn one adds
        // thousands of pixels and a missing one adds none. 300 is far below the
        // smallest of them and far above any antialiasing wobble.
        XCTAssertGreaterThan(shield, bare + 300, "the shield did not reach the bubble picture")
        XCTAssertGreaterThan(sword, bare + 300, "the sword did not reach the bubble picture")
        XCTAssertGreaterThan(check, bare + 300, "the check did not reach the bubble picture")
    }
}
