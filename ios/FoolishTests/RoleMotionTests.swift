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
        // Round 20: and the seat that OPENS the bout wears the tinted one.
        XCTAssertEqual(FSeatBadge(name: "a", handCount: 6, isAttacker: true,
                                  opensBout: true).mark, .leadSword)
        // The tint is an attacker's business only: a defender who happens to
        // have opened the last bout is wearing a shield, not a red sword.
        XCTAssertEqual(FSeatBadge(name: "a", handCount: 6, isDefender: true,
                                  opensBout: true).mark, .shield)
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
        XCTAssertEqual(Set(f.map(\.kind)), [.shield, .leadSword])
        let sword = f.first { $0.kind == .leadSword }
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
        let sword = f.first { $0.kind == .leadSword }
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

    /// …AND BACK AGAIN. Owner, 1.0(17): "when we tap good, sword rotates to
    /// checkmarks. But they should also rotate back to swords in the case that
    /// someone plays a card (all goods are cleared)."
    ///
    /// A throw-in genuinely clears every good - `handle_attack` sets
    /// `good_players_mask = 0` (c/src/game.c) - so the mark a seat wears goes
    /// check -> sword, which is one mark BECOMING another at the same seat and
    /// therefore the coin flip, not a fade and not a flight. Pinned in both
    /// directions because the pair is the gesture: a sword that turns into a
    /// check and never turns back is a board that stops telling you who may
    /// still attack.
    func testTheCheckTurnsBackIntoASwordWhenAThrowInClearsTheGoods() {
        let saidGood = MessageTableView.RoleState(defender: 1, firstAttacker: 0,
                                                  goodMask: (1 << 0) | (1 << 2))
        let thrownIn = MessageTableView.RoleState(defender: 1, firstAttacker: 0, goodMask: 0)

        // Nothing changed hands, so nothing flies - in EITHER direction.
        XCTAssertTrue(MessageTableView.roleFlights(from: saidGood, to: thrownIn,
                                                   pads: pads(ring)).isEmpty,
                      "clearing the goods threw a mark across the table")
        // And the gesture each seat makes is the flip back.
        XCTAssertEqual(RoleGesture.between(.check, .sword), .flip)
        XCTAssertEqual(RoleGesture.between(.sword, .check), .flip)
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

    /// ROUND 21, the owner: "the first attacker sword fully spins around, but
    /// the shield kinda turns a little bit then turns back. Make the shield spin
    /// all the way around too."
    ///
    /// A WHOLE number of turns for both, and the shield's old 24-degree lean is
    /// what that clause was describing: the ghost is taken away the moment it
    /// lands and the receiving badge draws its mark upright, so any final angle
    /// that is not a multiple of 360 snaps back on the hand-over. Asserted as
    /// "a multiple of 360, and not zero" rather than "== 360", because the
    /// property that matters is landing square, not the particular count.
    func testBothMarksTurnAWholeNumberOfTurns() {
        let f = MessageTableView.roleFlights(
            from: .init(defender: 1, firstAttacker: 0),
            to:   .init(defender: 2, firstAttacker: 1),
            pads: pads(ring))
        XCTAssertEqual(f.count, 2, "both marks changed seats, so both fly")
        for flight in f {
            XCTAssertGreaterThan(flight.spin, 0, "\(flight.kind) does not turn at all")
            XCTAssertEqual(flight.spin.truncatingRemainder(dividingBy: 360), 0,
                           "\(flight.kind) lands at \(flight.spin) degrees and snaps upright")
        }
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

    /// ROUND 20, the owner: "instead of fading the sword in or out when
    /// attackers become eligible or ineligible, make it spin but go to width
    /// zero." A role that simply begins or ends now makes HALF the coin flip,
    /// which is what makes every gesture in the file one gesture.
    func testARoleThatSimplyEndsTurnsEdgeOnRatherThanFading() {
        XCTAssertEqual(RoleGesture.between(.sword, nil), .rotateOut)
        XCTAssertEqual(RoleGesture.between(.check, nil), .rotateOut)
        XCTAssertEqual(RoleGesture.between(nil, .sword), .rotateIn)
        // The throw-in attacker the owner described by name: "all other
        // attackers rotate in", then rotate out again when the bout closes.
        XCTAssertEqual(RoleGesture.between(nil, .leadSword), .rotateIn)
        XCTAssertEqual(RoleGesture.between(.leadSword, nil), .rotateOut)
    }

    /// The opener's sword and a throw-in attacker's sword are DIFFERENT marks,
    /// so the board can tint one of them - and so a seat that gains the opening
    /// move turns its plain sword over into the tinted one rather than sitting
    /// there looking unchanged.
    func testTheOpenersSwordIsItsOwnMark() {
        XCTAssertNotEqual(RoleMarkKind.sword, .leadSword)
        XCTAssertEqual(RoleGesture.between(.sword, .leadSword), .flip)
        XCTAssertEqual(RoleGesture.between(.leadSword, .check), .flip)
        // Same drawn size, though: the tint is the only difference, or the row
        // would jump as the opening move changed hands.
        XCTAssertEqual(RoleMarkKind.leadSword.size, RoleMarkKind.sword.size)
    }

    /// What flies at a round end is the OPENER's sword, tinted - that is the
    /// whole reason the tint exists ("the first attack sword flies to next first
    /// attacker"), and a plain sword crossing the table would say the wrong
    /// thing about which seat it came from.
    func testTheSwordThatFliesIsTheOpenersOwn() {
        let f = MessageTableView.roleFlights(
            from: .init(defender: 1, firstAttacker: 0),
            to:   .init(defender: 2, firstAttacker: 1),
            pads: pads(ring))
        XCTAssertEqual(f.filter { $0.kind == .leadSword }.count, 1)
        XCTAssertTrue(f.allSatisfy { $0.kind != .sword }, "the plain sword never travels")
    }

    /// A seat expecting a mark turns its own away so the arriving one lands ON
    /// it - the owner's "they should rotate out AND the sword will land on
    /// them" / "the shield flies onto their sword". Both sentences are this one
    /// number: the make-way collapse has to FINISH as the ghost touches down,
    /// which means starting it a half-flip before the flight ends, not at the
    /// moment it begins.
    func testASeatMakesWayJustAsTheGhostArrives() {
        XCTAssertEqual(roleMakeWayDelay + roleFlipHalf, roleFlightTime, accuracy: 0.0001,
                       "the make-way turn must land on the same frame the ghost does")
        XCTAssertGreaterThan(roleMakeWayDelay, 0, "it waits; it does not blank on take-off")
    }

    func testNothingHappeningIsNotAGesture() {
        XCTAssertEqual(RoleGesture.between(.sword, .sword), .none)
        XCTAssertEqual(RoleGesture.between(nil, nil), .none)
        XCTAssertEqual(RoleGesture.resolve(shown: .sword, next: .sword, settled: true), .none)
        XCTAssertEqual(RoleGesture.resolve(shown: nil, next: nil, settled: true), .none)
    }

    /// THE SWORD THAT NEVER LANDED (found on the simulator, 1.0(17)): the new
    /// first attacker's badge came up bare after a bout end, while the board's
    /// own trace insisted the seat was wearing a sword.
    ///
    /// A bout end empties the live table one paint BEFORE the sweep grid stands
    /// up in its place, and in that single frame no attacker has any reason to
    /// wear a sword. So the mark went `sword -> nil -> sword` across two paints.
    /// The old guard compared the incoming kind against the mark still DRAWN -
    /// which was still the sword, because the fade-out had only just started -
    /// answered "nothing to do", and left the fade running; its task then blanked
    /// the seat for good. The seat that had a DIFFERENT mark afterwards (the new
    /// defender's shield) survived the same blink, which is exactly why only the
    /// sword was ever missing.
    func testAMarkThatBlinksOffAndBackIsPutBack() {
        XCTAssertEqual(RoleGesture.resolve(shown: .sword, next: .sword, settled: false), .restore,
                       "a mark that came back mid-fade must cancel it, not ignore it")
        XCTAssertEqual(RoleGesture.resolve(shown: .shield, next: .shield, settled: false), .restore)
        // A mark that is genuinely still going somewhere is not a restore - the
        // gesture it asks for is the one it would have asked for anyway.
        XCTAssertEqual(RoleGesture.resolve(shown: .sword, next: .check, settled: false), .flip)
        XCTAssertEqual(RoleGesture.resolve(shown: .sword, next: nil, settled: false), .rotateOut)
        XCTAssertEqual(RoleGesture.resolve(shown: nil, next: .sword, settled: false), .rotateIn)
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
