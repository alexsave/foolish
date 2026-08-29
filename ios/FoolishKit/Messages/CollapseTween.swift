// CollapseTween — what the surface's box height does while the drawer collapses.
//
// Round 10d tweened the box from its expanded height down to the compact one on
// the host's own curve, because the host snaps our MODEL box to the compact
// height and renders it glued to the drawer's DESCENDING top edge: a box as tall
// as the drawer is visible at that instant exactly fills it. All of that stands.
//
// WHAT DID NOT: the tween's target was whichever height happened to arrive on
// the first down-snap, and it was then held for 1.2 seconds with every later
// report ignored. But a style transition does not report one height, it bounces
// through several in both directions - one collapse was filmed reporting
// 748, 315, 307, 778, 758, 253, 315. Land on 315 and the box is right; land on
// 253 and the box eases to 60pt SHORTER than the drawer it is supposed to fill
// and stays there until the release. Top-anchored, that is a strip of bare wool
// along the bottom and a screen that reads as collapsed past its own compact
// height (owner, 1.0(22): "the lobby screen collapsed... to a height even
// shorter than the normal collapsed height. was super weird"). It self-heals
// when the box is released, which is why it looks like a glitch rather than a
// layout bug, and why it needs a particular arrival order to show up at all.
//
// So the rule gains one clause: while collapsing, a LATER compact-sized report
// that is TALLER than the current target re-points the tween at it. Upward only,
// and that asymmetry is the whole design - a box a few points taller than the
// drawer is clipped by the drawer and invisible, while a box shorter than it
// exposes what is behind. Chasing the noise DOWN as well would just trade a
// stuck-short box for a visible wobble along the bottom edge.
//
// A pure function over (report, state) so the sequence above can be replayed as
// a test instead of re-filmed - see CollapseTweenTests, which drives that exact
// sequence in both orders.

import CoreGraphics

public enum CollapseTween {
    /// Heights under this are the compact strip; an expanded board is far
    /// taller. The transition reports both, and only the compact ones say
    /// anything about where the collapse is going.
    public static let compactThreshold: CGFloat = 500

    /// How much taller a report must be than the current target before it is
    /// treated as a correction rather than as jitter.
    public static let retargetSlack: CGFloat = 2

    /// How much shorter than the armed height a report must be to count as the
    /// collapse flip rather than an ordinary small step (a manual drag).
    public static let flipDrop: CGFloat = 60

    public enum Step: Equatable {
        /// Begin the collapse: hold `from`, ease to `to` on the host's curve.
        case start(from: CGFloat, to: CGFloat)
        /// The host has settled taller than the snap we started with - ease up
        /// to it rather than resting short of the drawer.
        case retarget(to: CGFloat)
        /// Transition noise. Leave the box where it is.
        case hold
        /// Not collapsing: the box follows the model box exactly, as it does
        /// through every manual drag.
        case follow
    }

    /// `armed` is set by the host in the same runloop turn it requests .compact;
    /// `armedFrom` is the drawer height at that moment (the true expanded one,
    /// before the transition's noise). `target` is where the tween is currently
    /// headed, and is meaningless unless `collapsing`.
    public static func step(height: CGFloat, armed: Bool, armedFrom: CGFloat,
                            collapsing: Bool, target: CGFloat) -> Step {
        // The flip, consumed once: later reports cannot retrigger it, and a
        // manual grabber drag - never armed - never triggers it at all.
        if armed, armedFrom > height + flipDrop { return .start(from: armedFrom, to: height) }
        guard collapsing else { return .follow }
        // Expanded-sized reports mid-collapse are noise about where the drawer
        // WAS, not where it is going.
        guard height < compactThreshold else { return .hold }
        return height > target + retargetSlack ? .retarget(to: height) : .hold
    }
}
