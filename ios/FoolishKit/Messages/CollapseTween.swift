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
import Foundation

public enum CollapseTween {
    // MARK: The host's curve, and the box height that follows it
    //
    // THE DRAWER IS A SPRING. Seventeen collapses filmed at the recorder's own
    // rate and averaged (the per-take noise is 13-16pt; the average fits to
    // 3.5pt of a 481pt travel) land on a CRITICALLY DAMPED spring with a
    // response of 0.338s. No bezier follows that through its opening 70ms,
    // which is where the drawer sheds a fifth of its travel - and that opening
    // is exactly where every earlier bezier put the hand 30-40pt off the edge.
    //
    // WHY THE BOX IS DRIVEN EXPLICITLY, NOT BY `withAnimation`. Three reasons,
    // each filmed:
    //
    //   1. A SwiftUI animation renders its START value on its first frame. On
    //      the flip that frame is the still-expanded box under a drawer whose
    //      top has already moved 20-45pt, i.e. the hand pushed off the bottom
    //      of the screen for a frame. An evaluator renders the RIGHT height on
    //      its first frame instead.
    //   2. `.spring(response:)` cannot express where the host's spring already
    //      IS when our first report arrives; `lead` can.
    //   3. The height SwiftUI renders is whatever the state held when its
    //      update cycle ran. A free-running timer at twice the frame rate
    //      keeps that value at most half a frame stale, whichever way the two
    //      clocks happen to be phased. (It does NOT make the app render
    //      faster: measured, the timer ticked 142 times in 1.2s while the film
    //      still showed a new height on every other 8ms frame - SwiftUI
    //      renders on its own display link, 60Hz on this device, and the
    //      render server composites the host's transaction at 120Hz between
    //      our frames. That residual is a sawtooth of the drawer's own 8ms
    //      travel, ~26pt at peak, and no curve can remove it.)
    //
    // The formula is pure so it can be checked against the filmed average
    // (CollapseCurveTests) rather than re-filmed. `scratchpad/acx/phase.py`
    // reads the lag between the two clocks off a filmed take, which is how
    // `hostLead` was set and how it should be reset on a device that differs.

    /// The host drawer's spring response, in seconds. Fitted; see above.
    public static let hostResponse: Double = 0.338

    /// How far into the host's spring it already is when the collapse's first
    /// geometry report reaches `follow`, in seconds. Added to the tween's own
    /// clock so its renders land in phase with the drawer.
    ///
    /// MEASURED, not tuned: 5ms centres the box on the drawer (fresh frames
    /// +2ms behind, the re-composited frame between them 6ms ahead). Ten is
    /// chosen on purpose, because a dropped frame at peak velocity leaves the
    /// box one extra frame too tall and its bottom edge off the screen - the
    /// one thing the owner rules out entirely. Five extra milliseconds is a
    /// 15pt margin against that, paid as 15pt of hand-above-the-edge on the
    /// frames we do draw. Filmed: at 3ms a dropped frame put the bar off
    /// screen twice; at 5ms the margin was 14pt; at 20ms the hand rode 68pt
    /// high.
    public static let hostLead: Double = 0.010

    /// How often the driver evaluates the curve, in Hz. Twice the frame rate:
    /// see point 3 above. A tick that lands between two renders costs one
    /// evaluation and a state write, nothing more.
    public static let driveHz: Double = 120

    /// How long the driver runs before handing the box back to the model. The
    /// spring is at 99.99% by 0.5s; the rest is the host's own settle.
    public static let driveDuration: Double = 1.2

    /// Progress 0...1 of a critically damped spring `response` seconds long,
    /// `t` seconds in. Zero before the start; approaches 1 asymptotically.
    public static func hostProgress(at t: Double, response: Double = hostResponse) -> Double {
        guard t > 0 else { return 0 }
        let w = 2 * Double.pi / response
        return 1 - (1 + w * t) * exp(-w * t)
    }

    /// The box height `t` seconds into a collapse from `from` to `to`.
    public static func height(from: CGFloat, to: CGFloat, at t: Double,
                              response: Double = hostResponse) -> CGFloat {
        to + (from - to) * CGFloat(1 - hostProgress(at: t, response: response))
    }

    /// How far the wool hangs below the box while a collapse runs, in points:
    /// the lead's margin (~15pt) plus one 8ms re-composite of the drawer at
    /// peak (~26pt), so a box kept deliberately short shows wool under the
    /// hand and not the host's fallback colour. See `MessagesRootView`'s
    /// background.
    public static let woolOverhang: CGFloat = 48

    /// A retarget's easing: a later, taller report moves the target over this
    /// long rather than in one step (the step was filmed as a 34pt hop).
    public static let retargetDuration: Double = 0.18

    /// Where an eased retarget from `a` to `b` is, `t` seconds after it began.
    public static func retargetBlend(from a: CGFloat, to b: CGFloat, at t: Double) -> CGFloat {
        let x = min(1, max(0, t / retargetDuration))
        let eased = 1 - (1 - x) * (1 - x)          // ease-out, matches the old .easeOut
        return a + (b - a) * CGFloat(eased)
    }

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

    /// THE HEIGHT THE BOX PASSES THROUGH ON ITS WAY BACK TO THE MODEL, and why
    /// it is not simply the model's own.
    ///
    /// 1.0(43), owner, two reports that are one bug: "table to discard animation
    /// still seems to have a geometry mismatch like it's using expanded coords
    /// on a collapsed screen", and "geometry seems to be broken by the auto
    /// collapse? then fixed by swiping to expand/collapse". Measured on the rig,
    /// on a board that had been compact and still for three seconds after an
    /// ARMED collapse: the board's own GeometryReader reads 243, and every
    /// PUBLISHED frame still describes the expanded box - `handFrame` at midY
    /// 623 rather than 217, the battle cards at y 345 rather than 142. The
    /// settlement sweep released by Send then flew from 200pt below the bottom
    /// of a 261pt drawer. A MANUAL collapse takes `.follow`, never touches the
    /// box height, and measures correctly throughout - which is exactly the
    /// difference between the two paths the owner noticed.
    ///
    /// The cause is that the tween ends on the height the box already rests at.
    /// `boxHeight` runs 0 -> expanded -> (animated) target, and the target IS
    /// the model height, so releasing the override is numerically no change at
    /// all. SwiftUI delivered the subtree's preferences once, from the expanded
    /// pass that opened the animation, and never had a later height change to
    /// deliver from - the compact passes that follow all measure a size it
    /// already believes it published.
    ///
    /// So the release goes through here first: half a point off the rest height,
    /// which is a real change, then back, which is another. Sub-pixel, so
    /// nothing on screen moves, and the second one republishes every landmark a
    /// flight aims at against the size actually on screen.
    public static let remeasureNudge: CGFloat = 0.5

    /// The one height that ends a tween. MUST differ from `target`, or the
    /// release publishes nothing - see `remeasureNudge`.
    public static func handBack(target: CGFloat) -> CGFloat { target + remeasureNudge }

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
