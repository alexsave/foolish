// FRoleMotion.swift - the role marks MOVE.
//
// Round 16, the owner: "animate the status transitions with more moving. Have
// the shield fly across the table to the next defender. When a player says
// good, the sword should spin like a coin to the checkbox. Most swords can
// fade, but maybe the first attacker sword could fly to the next first
// attacker."
//
// ROUND 20 took the fade out of it entirely, and spelled the whole choreography
// out: "instead of fading the sword in or out when attackers become eligible or
// ineligible, make it spin but go to width zero. First attacker sword and
// defense shield should still fly across the table. So I want to see this:
// first attacker starts with sword, defender starts with shield. First attacker
// attacks. All other attackers 'rotate in'. Do their attacks. Rotate to good as
// necessary. Then at round end, the first attack sword flies to next first
// attacker. Shield flies to next defender. For everyone else, the sword
// 'rotates out'. You might be asking what about the next first attacker? Well
// they should rotate out AND the sword will land on them. Maybe make the first
// attacker sword have a slight dark red tint to make it a bit special. What if
// we have a pass? What to do with the shield? For the next defender, the shield
// flies onto their sword. For the previous defender, the shield flies away and
// their sword rotates in."
//
// Until round 16 a role mark was a fact printed under a seat: the shield was
// simply somewhere else on the next paint, and a sword became a check between
// two frames. Nothing carried the eye from the seat that had the role to the
// seat that has it now, which at eight seats is the difference between reading
// the board and hunting for it.
//
// THREE MOTIONS, and each says a different thing:
//
//   THE COIN FLIP - one mark BECOMES another at the same seat. Its width
//   collapses to nothing, the mark is swapped at the invisible frame, and it
//   opens back out: an object turning over, not two objects swapped. This is
//   what a sword saying "good" does, and it is why the flip is a width scale
//   about the centre rather than a cross-fade (a cross-fade is two things
//   dissolving; a flip is one thing with two faces).
//
//   THE HALF FLIP - a role simply BEGINS or ENDS where it stands (an attacker
//   who becomes eligible when the bout opens, whose sword then ends when it
//   closes). Same coin, one half of it: it turns edge-on and is gone, or comes
//   round from edge-on and is there. Round 20 replaced a cross-fade here, and
//   the reason is the same one that made the full flip a flip - a mark that
//   fades is a mark dissolving into the board, where a mark that turns away is
//   the same object leaving. It also means every gesture in this file is now
//   the same gesture, which is what makes them read as one language.
//
//   THE FLIGHT - a role LEAVES one seat for another. The shield sails across
//   the table to the next defender; the sword hands the opening move to the
//   next first attacker. Both turn a whole revolution on the way (round 21 -
//   see `RoleFlight.spin` for why a part-turn cannot work here). The seat it
//   left blanks INSTANTLY (the ghost in the overlay is that mark now, and two
//   of it on screen is a glitch), while the seat it is going to turns its own
//   mark away in the last moments of the flight, so the arriving mark lands ON
//   it rather than into a gap that has been sitting empty. That last beat is
//   the owner's two hardest sentences - "they should rotate out AND the sword
//   will land on them", and "the shield flies onto their sword" - and it is one
//   rule, not two.
//
// Reduce Motion turns all of them into an instant swap.

import SwiftUI

/// The role marks, as a value - so one view can hold "whichever mark this seat
/// is wearing" and animate between them. The board decides which is primary (a
/// seat is never two of these at once: the kernel rejects a defender's `good`,
/// and `showsSword` already stands down for both).
public enum RoleMarkKind: Equatable, Sendable {
    case shield      // defending
    case sword       // may attack
    case leadSword   // opens the bout - the same sword, tinted (round 20)
    case check       // said good

    /// Each mark's own drawn size (FRoleMark), which differ because a sword on
    /// the shared 24x24 grid is rotated 45 degrees and needs a bigger box to
    /// read the same size as a shield.
    var size: CGFloat {
        switch self {
        case .shield: return FRoleMark.shield
        case .sword, .leadSword: return FRoleMark.sword
        case .check:  return FRoleMark.check
        }
    }
}

/// One role mark, drawn. The ONE place the kind→view mapping lives, so a badge,
/// the local player's own indicator and a flight ghost cannot end up drawing
/// three different swords.
public struct RoleMarkView: View {
    public let kind: RoleMarkKind
    public init(_ kind: RoleMarkKind) { self.kind = kind }
    public var body: some View {
        switch kind {
        case .shield: FShield(size: FRoleMark.shield)
        case .sword:  FSword(size: FRoleMark.sword)
        // ROUND 20 ("maybe make the first attacker sword have a slight dark red
        // tint to make it a bit special"): the SAME sword, drawn by the same
        // path, with the fill tinted. Not a second glyph - the seat that opens
        // the bout is wearing the same object as everyone else, and a different
        // shape would say it was a different role.
        case .leadSword: FSword(size: FRoleMark.sword, fill: FRoleInk.lead)
        case .check:  FCheck(size: FRoleMark.check)
        }
    }
}

/// Where each seat's role mark sits, in `boardSpace` - the take-off and landing
/// pads for a flight. Published by every seat badge that is told its seat, and
/// by the local player's own indicator, so a shield can fly from an opponent to
/// me and back.
public struct RoleMarkFramesKey: PreferenceKey {
    public static let defaultValue: [Int: CGRect] = [:]
    public static func reduce(value: inout [Int: CGRect], nextValue: () -> [Int: CGRect]) {
        value.merge(nextValue()) { _, new in new }
    }
}

/// A little under a card flight. The role hand-off is the closing beat of a
/// bout - the cards have already been swept and dealt - so it wants to feel
/// quick and deliberate rather than ceremonial. Scales with HARNESS_SLOWMO like
/// every other duration, via `flightTime`.
public var roleFlightTime: Double { flightTime * 0.8 }

/// Half a coin flip: collapse, swap, open. Two of these back to back is a full
/// turn, one on its own is a mark arriving or leaving, and the pair deliberately
/// comes in under a card's motion - the mark is a caption on the move, not the
/// move.
public var roleFlipHalf: Double { flightTime * 0.22 }

/// ROUND 30, the owner: "for the sword -> check rotation, the width doesn't
/// QUITE go to zero during rotate out before swapping to the other glyph and
/// rotating in. We just need to get it to zero width before expanding back
/// out."
///
/// The swap is scheduled on a timer that starts the instant `withAnimation` is
/// called, but the animation itself does not start until the next display
/// refresh - so the timer fires about one frame EARLY. That frame is 15% of a
/// half-flip (110ms shipping), and `easeIn` spends its last frames moving
/// fastest: the coin is still a quarter of its width across when the face is
/// swapped, which is exactly the sliver the owner can see.
///
/// So the collapse is given a deadline slightly BEFORE the swap, and the
/// difference is a genuine edge-on beat. Taken out of the collapse rather than
/// added to the flip, so the gesture's total length - which is tuned against
/// the card flights it captions - does not change at all. Proportional at the
/// low end so HARNESS_SLOWMO and a hypothetical short flip cannot invert it.
public var roleFlipSettle: Double { min(0.03, roleFlipHalf * 0.3) }

/// How long the collapse itself is given: a settle short of the swap.
public var roleFlipCollapse: Double { max(0.01, roleFlipHalf - roleFlipSettle) }

/// How long a seat expecting an arrival waits before turning its own mark away,
/// so the collapse FINISHES as the ghost touches down (owner: "the shield flies
/// onto their sword"). Clamped at zero for the degenerate case where a flight is
/// shorter than half a flip.
public var roleMakeWayDelay: Double { max(0, roleFlightTime - roleFlipHalf) }

/// A role mark in flight between two seats, in `boardSpace`.
public struct RoleFlight: Identifiable, Equatable {
    public let id: String
    public let kind: RoleMarkKind
    public let from: CGPoint
    public let to: CGPoint
    /// The seat it LEFT (blanks instantly - the ghost is that mark now) and the
    /// seat it is GOING TO (turns its own mark away as the ghost arrives).
    public let fromSeat: Int
    public let toSeat: Int
    /// Total degrees turned over the flight, and in practice a WHOLE number of
    /// turns.
    ///
    /// It reads as "this was thrown to somebody", which is what a hand-off is.
    /// But the reason it cannot be a fraction of a turn is mechanical: the ghost
    /// is taken away the instant it lands and the receiving badge draws the real
    /// mark upright, so any final angle other than a multiple of 360 snaps
    /// straight on the hand-over. The shield used to lean 24 degrees and did
    /// exactly that - the owner, round 21: "the shield kinda turns a little bit
    /// then turns back."
    public let spin: Double

    public init(id: String, kind: RoleMarkKind, from: CGPoint, to: CGPoint,
                fromSeat: Int, toSeat: Int, spin: Double) {
        self.id = id; self.kind = kind; self.from = from; self.to = to
        self.fromSeat = fromSeat; self.toSeat = toSeat; self.spin = spin
    }

    /// The bow in the path. A mark thrown across a table travels over it, not
    /// through it, so the arc lifts toward the top of the board - scaled to the
    /// distance so neighbouring seats get a hop and opposite seats get a sail,
    /// and capped so it can never leave the board on a tall drawer.
    var control: CGPoint {
        let dx = to.x - from.x, dy = to.y - from.y
        let dist = (dx * dx + dy * dy).squareRoot()
        return CGPoint(x: (from.x + to.x) / 2, y: (from.y + to.y) / 2 - min(56, dist * 0.28))
    }

    func point(at p: Double) -> CGPoint {
        // Quadratic bezier through `control`.
        let q = 1 - p
        let c = control
        return CGPoint(x: q * q * from.x + 2 * q * p * c.x + p * p * to.x,
                       y: q * q * from.y + 2 * q * p * c.y + p * p * to.y)
    }
}

/// The overlay a role flight is drawn in, above the board and above the cards -
/// the hand-off is the thing being read at that moment, and a shield passing
/// behind a badge would read as a glitch. `progress` is animated by the board
/// with one `withAnimation`, exactly like `FlyingCardsLayer`.
public struct RoleFlightsLayer: View {
    public let flights: [RoleFlight]
    public let progress: Double
    public init(flights: [RoleFlight], progress: Double) {
        self.flights = flights; self.progress = progress
    }

    public var body: some View {
        ZStack {
            ForEach(flights) { f in
                let p = progress
                let pt = f.point(at: p)
                RoleMarkView(f.kind)
                    .rotationEffect(.degrees(f.spin * p))
                    // Bigger in the middle of the throw, like a card's flight -
                    // the mark comes off the table toward the viewer and settles
                    // back down onto the badge it lands on.
                    .scaleEffect(1 + 0.45 * (1 - abs(p - 0.5) * 2))
                    .shadow(color: .black.opacity(0.45 * (1 - abs(p - 0.5) * 2)),
                            radius: 8 * (1 - abs(p - 0.5) * 2), y: 6 * (1 - abs(p - 0.5) * 2))
                    .position(pt)
            }
        }
        .allowsHitTesting(false)
    }
}

/// WHICH MOTION a mark makes when it changes, as a value - the rule on its own,
/// away from the `@State` that plays it.
public enum RoleGesture: Equatable, Sendable {
    /// One mark becomes another at the same seat: the whole coin, both halves.
    case flip
    /// The role ended and nothing took it over: turn edge-on and be gone.
    /// ROUND 20 - this was a cross-fade until the owner asked for "spin but go
    /// to width zero", which is the flip's first half on its own.
    case rotateOut
    /// A role arrived at a seat that was bare: come round from edge-on. The
    /// flip's SECOND half, and the mirror of `rotateOut`.
    case rotateIn
    /// The mark came back to the one it was already wearing while a gesture was
    /// still playing: take that gesture back and stand the mark up where it is.
    case restore
    /// Nothing to say.
    case none

    /// A mark that TRAVELS is not a gesture this view makes at all - the board
    /// blanks the seat it left and the ghost carries it (see `FRoleCoin`), so a
    /// flight never reaches here.
    public static func between(_ shown: RoleMarkKind?, _ next: RoleMarkKind?) -> RoleGesture {
        switch (shown, next) {
        case let (a?, b?): return a == b ? .none : .flip
        case (.some, .none): return .rotateOut
        case (.none, .some): return .rotateIn
        case (.none, .none): return .none
        }
    }

    /// WHICH gesture to make, including the case `between` cannot see.
    ///
    /// A mark can go away and come straight BACK to what it was, within one
    /// paint: a bout end empties the live table before the sweep grid stands up
    /// in its place, and for that single frame every attacker's sword has no
    /// reason to exist. Asking `between` alone answers `.none` both times - so
    /// the exit started by the first change was never taken back, and its own
    /// task then blanked a seat that had never stopped wearing its mark.
    /// (That is the sword that vanished instead of landing on the next opener:
    /// the shield survived the same blink only because its kind changed again
    /// afterwards, which gave it a `.rotateIn`.)
    ///
    /// `settled` is "this view is at rest showing `shown`" - no half-played
    /// gesture, and none scheduled. Same mark and settled is genuinely nothing
    /// to do; same mark and UNsettled is the blink, and the answer is to put it
    /// back.
    public static func resolve(shown: RoleMarkKind?, next: RoleMarkKind?,
                               settled: Bool) -> RoleGesture {
        if shown == next { return settled ? .none : .restore }
        return between(shown, next)
    }
}

/// A seat's role mark, with the motion between one mark and the next.
///
/// Holds its OWN displayed mark rather than rendering `kind` directly, because
/// a flip has an invisible frame in the middle where the two faces swap - the
/// view has to be showing the old mark for the first half and the new one for
/// the second, which a stateless view cannot do. `shown` is seeded from `kind`
/// at init (NOT in `onAppear`): this view is also rendered by ImageRenderer for
/// the bubble snapshot, where appearance callbacks do not run, and a mark that
/// waited for `onAppear` would be missing from every snapshot.
public struct FRoleCoin: View {
    public let kind: RoleMarkKind?
    /// This seat's mark is IN THE AIR as a flight ghost. Draw nothing where it
    /// stood, and do not animate it away - the ghost already is that motion, and
    /// a rotate-out here would show the mark leaving twice.
    public let departing: Bool
    /// A mark is flying TO this seat. Whatever is worn here turns away in the
    /// last moments of the flight, and the arriving mark is stood up the instant
    /// the ghost lands (`RoleFlightsLayer` is what the eye is following, so the
    /// hand-over must be a swap, not a second animation).
    public let arriving: Bool

    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @State private var shown: RoleMarkKind?
    @State private var flip: CGFloat = 1
    /// A gesture is scheduled but has not started (the make-way delay). Counts
    /// as UNsettled - see `RoleGesture.resolve`.
    @State private var waiting = false
    /// Claims the half-finished gesture. A mark can change again mid-flip (a
    /// bout that ends on the beat after a good), and the older Task must not
    /// wake up and finish INTO the mark the newer one already swapped past -
    /// which is the one way this could leave a seat wearing the wrong role.
    /// A token rather than re-reading `kind`, because the Task captured this
    /// view's value and its `kind` is frozen at the moment it was made.
    @State private var gesture = 0
    /// The inputs as of the last `advance`. All three are read together, because
    /// which gesture to make depends on which of them MOVED - and SwiftUI
    /// delivers one `onChange` per property in an order this view must not
    /// depend on (the board sets the roles and the flying seats in the same
    /// tick, so a per-property reaction would see half a hand-off).
    @State private var last: Input

    private struct Input: Equatable {
        var kind: RoleMarkKind?
        var departing: Bool
        var arriving: Bool
        /// What the seat should be WEARING for this input: nothing while a mark
        /// is on its way here, since the ghost is that mark until it lands.
        var target: RoleMarkKind? { arriving ? nil : kind }
    }

    public init(kind: RoleMarkKind?, departing: Bool = false, arriving: Bool = false) {
        self.kind = kind
        self.departing = departing
        self.arriving = arriving
        let seed = Input(kind: kind, departing: departing, arriving: arriving)
        _shown = State(initialValue: seed.departing ? nil : seed.target)
        _last = State(initialValue: seed)
    }

    public var body: some View {
        ZStack {
            if let k = shown { RoleMarkView(k) }
        }
        // A CONSTANT box, always present, whether or not this seat wears a mark:
        // the row then has nothing to re-lay-out when a mark arrives or leaves
        // (the badge above it must not twitch), and - the reason it matters here
        // - it always has a frame to publish, so a flight can take off from a
        // seat that is not currently wearing anything.
        .frame(width: FRoleMark.rowHeight, height: FRoleMark.rowHeight)
        .scaleEffect(x: flip, y: 1, anchor: .center)
        .onChange(of: Input(kind: kind, departing: departing, arriving: arriving)) { now in
            advance(to: now)
        }
    }

    /// At rest showing `shown`: no half-played gesture, and none waiting to start.
    private var settled: Bool { flip == 1 && !waiting }

    /// Claim whatever gesture is in flight, so an older task cannot wake up and
    /// blank a mark this call has decided to keep.
    private func claim() -> Int {
        gesture += 1
        waiting = false
        return gesture
    }

    private func advance(to now: Input) {
        let was = last
        last = now
        guard !reduceMotion else {
            _ = claim()
            shown = now.departing ? nil : now.target
            flip = 1
            return
        }

        // THE GHOST TOOK IT. Instant, never animated: the mark this seat was
        // wearing is now the one sailing across the table, and easing it away
        // here would be the same object leaving twice at two different speeds.
        // A seat that is losing one mark and gaining another in the same beat
        // (2p: the defender becomes the opener and the opener becomes the
        // defender) blanks here and stays blank, because `target` is nil while
        // `arriving` holds.
        if now.departing && !was.departing {
            let mine = claim()
            shown = nil
            flip = 1
            // A pass's PREVIOUS defender: the shield flies away and a sword
            // rotates in behind it, in the same beat. The owner asked for these
            // two to happen together, and they read as cause and effect.
            if let t = now.target { rotateIn(t, mine) }
            return
        }

        // THE GHOST LANDED. Also instant, and for the mirror reason: the flight
        // layer has just put this mark down on this pad, so the real one stands
        // up in the same frame the ghost is taken away in.
        if was.arriving && !now.arriving {
            _ = claim()
            shown = now.departing ? nil : now.target
            flip = 1
            return
        }

        // SOMETHING IS ON ITS WAY HERE. Turn what we are wearing away, timed so
        // the collapse finishes as the ghost touches down - the owner's "they
        // should rotate out AND the sword will land on them". Nothing to turn
        // away (a bare seat receiving a mark) simply waits.
        if now.arriving && !was.arriving {
            let mine = claim()
            guard shown != nil else { return }
            waiting = true
            rotateOut(mine, after: roleMakeWayDelay)
            return
        }

        // Nothing is flying: the ordinary gesture between two marks.
        let gest = RoleGesture.resolve(shown: shown, next: now.target, settled: settled)
        guard gest != .none else { return }
        let mine = claim()
        switch gest {
        case .restore:
            // Never animated: the mark was already there and nothing about it
            // changed - the only thing that moved was a gesture that turned out
            // to be wrong. Easing it back would be a motion nobody asked for.
            shown = now.target
            flip = 1
        case .flip:
            // Both faces of one coin: collapse, swap at the edge, open out.
            // The collapse finishes a hair before the swap (roleFlipSettle), so
            // the face changes on a coin that is genuinely edge-on.
            withAnimation(.easeIn(duration: roleFlipCollapse)) { flip = 0.001 }
            Task { @MainActor in
                try? await Task.sleep(nanoseconds: UInt64(roleFlipHalf * 1_000_000_000))
                guard mine == gesture else { return }
                shown = now.target
                withAnimation(.easeOut(duration: roleFlipHalf)) { flip = 1 }
            }
        case .rotateOut:
            rotateOut(mine, after: 0)
        case .rotateIn:
            if let t = now.target { rotateIn(t, mine) }
        case .none:
            break
        }
    }

    /// Half a coin, turning edge-on and gone. `delay` is the make-way pause a
    /// seat expecting an arrival takes first; zero for a role that simply ended.
    private func rotateOut(_ mine: Int, after delay: Double) {
        Task { @MainActor in
            if delay > 0 {
                try? await Task.sleep(nanoseconds: UInt64(delay * 1_000_000_000))
                guard mine == gesture else { return }
                waiting = false
            }
            withAnimation(.easeIn(duration: roleFlipCollapse)) { flip = 0.001 }
            try? await Task.sleep(nanoseconds: UInt64(roleFlipHalf * 1_000_000_000))
            guard mine == gesture else { return }
            // Reset the scale as the mark goes, not with it: `flip` is how this
            // view says "mid-gesture", and a seat left at width zero with
            // nothing in it would read as unsettled forever.
            shown = nil
            flip = 1
        }
    }

    /// The other half, coming round from edge-on.
    private func rotateIn(_ mark: RoleMarkKind, _ mine: Int) {
        shown = mark
        flip = 0.001
        withAnimation(.easeOut(duration: roleFlipHalf)) { flip = 1 }
        Task { @MainActor in
            try? await Task.sleep(nanoseconds: UInt64(roleFlipHalf * 1_000_000_000))
            guard mine == gesture else { return }
            flip = 1
        }
    }
}
