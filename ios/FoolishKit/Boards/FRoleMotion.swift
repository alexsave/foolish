// FRoleMotion.swift - the role marks MOVE.
//
// Round 16, the owner: "animate the status transitions with more moving. Have
// the shield fly across the table to the next defender. When a player says
// good, the sword should spin like a coin to the checkbox. Most swords can
// fade, but maybe the first attacker sword could fly to the next first
// attacker."
//
// Until now a role mark was a fact printed under a seat: the shield was simply
// somewhere else on the next paint, and a sword became a check between two
// frames. Nothing carried the eye from the seat that had the role to the seat
// that has it now, which at eight seats is the difference between reading the
// board and hunting for it.
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
//   THE FLIGHT - a role LEAVES one seat for another. The shield sails across
//   the table to the next defender; the sword hands the opening move to the
//   next first attacker. Both endpoints hide their mark for the duration, so
//   the ghost in the overlay is the only one of it on screen and the landing is
//   a hand-off, not a second copy appearing.
//
//   THE FADE - a role simply ENDS (an attacker's sword when the bout closes,
//   a check when the goods are cleared). Nothing travels, so nothing should
//   pretend to: it fades. The owner's "most swords can fade" is a rule about
//   meaning, not about cost - a mark that flies is a mark that went somewhere.
//
// Reduce Motion turns all three into an instant swap.

import SwiftUI

/// The three role marks, as a value - so one view can hold "whichever mark this
/// seat is wearing" and animate between them. The board decides which is
/// primary (a seat is never two of these at once: the kernel rejects a
/// defender's `good`, and `showsSword` already stands down for both).
public enum RoleMarkKind: Equatable, Sendable {
    case shield   // defending
    case sword    // may attack / opens the bout
    case check    // said good

    /// Each mark's own drawn size (FRoleMark), which differ because a sword on
    /// the shared 24x24 grid is rotated 45 degrees and needs a bigger box to
    /// read the same size as a shield.
    var size: CGFloat {
        switch self {
        case .shield: return FRoleMark.shield
        case .sword:  return FRoleMark.sword
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

/// Half a coin flip: collapse, swap, open. Two of these back to back is the
/// whole gesture, and the pair deliberately comes in under a card's motion -
/// the mark is a caption on the move, not the move.
public var roleFlipHalf: Double { flightTime * 0.22 }

/// A role that simply ended, going. Slower than half a flip and faster than a
/// whole one: it should read as "that is over" without competing with whatever
/// is flying at the same moment.
public var roleFadeTime: Double { flightTime * 0.3 }

/// A role mark in flight between two seats, in `boardSpace`.
public struct RoleFlight: Identifiable, Equatable {
    public let id: String
    public let kind: RoleMarkKind
    public let from: CGPoint
    public let to: CGPoint
    /// Seats whose own mark must stay hidden while this is in the air (both
    /// ends: the one it left and the one it is going to).
    public let fromSeat: Int
    public let toSeat: Int
    /// Total degrees turned over the flight. The sword takes a full turn - it is
    /// being handed to the next player to swing; the shield keeps its face to
    /// the room and only leans into the throw.
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
    /// One mark becomes another at the same seat: the coin flip.
    case flip
    /// The role ended and nothing took it over: fade where it stood.
    case fadeOut
    /// A role arrived at a seat that was bare: fade in.
    case fadeIn
    /// The mark came back to the one it was already wearing while a gesture was
    /// still playing: take that gesture back and stand the mark up where it is.
    case restore
    /// Nothing to say.
    case none

    /// A mark that TRAVELS is not a gesture this view makes at all - the board
    /// hides both endpoints and the ghost carries it (see `FRoleCoin.flying`),
    /// so a flight never reaches here.
    public static func between(_ shown: RoleMarkKind?, _ next: RoleMarkKind?) -> RoleGesture {
        switch (shown, next) {
        case let (a?, b?): return a == b ? .none : .flip
        case (.some, .none): return .fadeOut
        case (.none, .some): return .fadeIn
        case (.none, .none): return .none
        }
    }

    /// WHICH gesture to make, including the case `between` cannot see.
    ///
    /// A mark can go away and come straight BACK to what it was, within one
    /// paint: a bout end empties the live table before the sweep grid stands up
    /// in its place, and for that single frame every attacker's sword has no
    /// reason to exist. Asking `between` alone answers `.none` both times - so
    /// the fade-out started by the first change was never taken back, and its
    /// own task then blanked a seat that had never stopped wearing its mark.
    /// (That is the sword that vanished instead of landing on the next opener:
    /// the shield survived the same blink only because its kind changed again
    /// afterwards, which gave it a `.fadeIn`.)
    ///
    /// `settled` is "this view is at rest showing `shown`" - no half-played
    /// fade or flip. Same mark and settled is genuinely nothing to do; same
    /// mark and UNsettled is the blink, and the answer is to put it back.
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
    /// This seat's mark is currently in the air as a flight ghost - draw
    /// nothing, and do not fade, or the hand-off would show two of it.
    public let flying: Bool

    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @State private var shown: RoleMarkKind?
    @State private var flip: CGFloat = 1
    @State private var fade: Double = 1
    /// Claims the half-finished gesture. A mark can change again mid-flip (a
    /// bout that ends on the beat after a good), and the older Task must not
    /// wake up and finish INTO the mark the newer one already swapped past -
    /// which is the one way this could leave a seat wearing the wrong role.
    /// A token rather than re-reading `kind`, because the Task captured this
    /// view's value and its `kind` is frozen at the moment it was made.
    @State private var gesture = 0

    public init(kind: RoleMarkKind?, flying: Bool = false) {
        self.kind = kind
        self.flying = flying
        _shown = State(initialValue: kind)
        _fade = State(initialValue: kind == nil ? 0 : 1)
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
        .opacity(flying ? 0 : fade)
        // The hand-off is a swap of one drawn thing for another at the same
        // instant; interpolating it would show the mark ghosting back in under
        // the one that just landed.
        .animation(nil, value: flying)
        .onChange(of: kind) { next in advance(to: next) }
    }

    /// At rest showing `shown`: no half-played fade or flip to take back.
    private var settled: Bool { fade == (shown == nil ? 0 : 1) && flip == 1 }

    private func advance(to next: RoleMarkKind?) {
        let gest = RoleGesture.resolve(shown: shown, next: next, settled: settled)
        guard gest != .none else { return }
        // Claims whatever was in flight, so a fade-out's task cannot wake up and
        // blank the mark this call just decided to keep.
        gesture += 1
        let mine = gesture
        guard !reduceMotion else { shown = next; fade = next == nil ? 0 : 1; flip = 1; return }
        switch gest {
        case .restore:
            // Never animated: the mark was already there and nothing about it
            // changed - the only thing that moved was a gesture that turned out
            // to be wrong. Easing it back would be a fade nobody asked for.
            shown = next
            fade = next == nil ? 0 : 1
            flip = 1
        case .flip:
            // Both faces of one coin: collapse, swap at the edge, open out.
            withAnimation(.easeIn(duration: roleFlipHalf)) { flip = 0.001 }
            Task { @MainActor in
                try? await Task.sleep(nanoseconds: UInt64(roleFlipHalf * 1_000_000_000))
                guard mine == gesture else { return }
                shown = next
                fade = 1
                withAnimation(.easeOut(duration: roleFlipHalf)) { flip = 1 }
            }
        case .fadeOut:
            // The role ended. Nothing travelled, so it fades where it stood.
            withAnimation(.easeOut(duration: roleFadeTime)) { fade = 0 }
            Task { @MainActor in
                try? await Task.sleep(nanoseconds: UInt64(roleFadeTime * 1_000_000_000))
                guard mine == gesture else { return }
                shown = nil
            }
        case .fadeIn:
            shown = next
            flip = 1
            fade = 0
            withAnimation(.easeOut(duration: roleFadeTime)) { fade = 1 }
        case .none:
            break
        }
    }
}
