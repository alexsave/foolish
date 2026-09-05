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
    /// IS THE BOARD'S BOX MID-TWEEN RIGHT NOW? Set by the one place that runs
    /// the tween (MessagesRootView.follow) and read by the one place that aims
    /// flights (MessageTableView.playStep).
    ///
    /// ROUND 30, the geometry half of the 1.0(29) report: "in the collapsed
    /// view, when I sent it, it then animated the card flying to the other
    /// players hand ... however the animation seemed to go to the table center
    /// rather than their hand."
    ///
    /// A flight is aimed ONCE, at build time, and then plays for half a second
    /// against whatever the board has become. On a settled board that is fine.
    /// During the collapse it is not: the tween deliberately HOLDS the box at
    /// its expanded height on the style flip and eases it down to the compact
    /// one (667pt -> 261pt on this device), so every landmark a flight aims at
    /// is moving. An opponent's badge sits at 15% of the board height, i.e.
    /// y=100 expanded and y=31 compact, while the table's centre lands at
    /// y=130 - so a card aimed mid-tween flies to y=100 in a board where that
    /// is 30pt ABOVE the middle of the table and 70pt BELOW the badge. Which
    /// is the report, to the pixel.
    ///
    /// A static rather than plumbed state for the same reason FHandFan's
    /// `instantExit` is one: the reader is a view that is rebuilt constantly
    /// and the writer is its ancestor, and a captured binding between them is
    /// a race. Exactly one writer, and it is the tween itself.
    @MainActor public static var isTweening = false

    /// IS THE HOST STILL MOVING THE SHEET? Set from the extension's
    /// `willTransition`/`didTransition` (the only place that can know), read by
    /// the board before it starts an OPEN REPLAY.
    ///
    /// ROUND 30, the owner on 1.0(33): "when I replayed a bout ending good
    /// bubble, the rotation of the sword to check animation started WHILE the
    /// view was coming up into view. So we barely saw the sword."
    ///
    /// Tapping a bubble expands the extension, and the board is mounted and
    /// running while Messages is still sliding the sheet up. The replay is the
    /// whole reason the player tapped, and it was spending its first half behind
    /// the edge of the screen - worst for the shortest gestures, which is why a
    /// good's sword-to-check was the one that vanished. The card flights were
    /// losing the same beat and nobody had noticed.
    ///
    /// A flag rather than the existing `awaitTransitionSettled` continuation
    /// because the waiter lives on the view controller and the thing that needs
    /// to wait is a SwiftUI view three frameworks down; this is the same shape
    /// as `isTweening` beside it, and for the same reason.
    @MainActor public static var isPresenting = false

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
