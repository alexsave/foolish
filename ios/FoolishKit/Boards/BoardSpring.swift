// BoardSpring - the two modifiers the message board's chrome wears, and the
// rule each of them is.
//
// Both were at the bottom of MessageTableView.swift, under the 2,200-line view
// they serve. Neither reads the controller, the animator or a single piece of
// board state: one is `animation(nil, value:)` spelled so the call sites cannot
// get it wrong, the other is a scroll-bounce policy. They are rules about
// chrome, and they are the same two rules wherever chrome sits.

import SwiftUI

/// THE CHROME DOES NOT RIDE THE BOARD'S CARD SPRING - round 7, written once.
///
/// `boardContent` animates on `.animation(FMotion.cardMotion, value:
/// controller.view)`. That is a SCOPED value-animation: every descendant whose
/// geometry changes BECAUSE `controller.view` changed springs with it. Five
/// pieces of chrome sit inside that scope and must not move on it - the staged
/// send hint, the promoted self role mark, the action column, the undo slot and
/// the settings/help squares. All five hang off `buttonLift`/`statusMarkLift`,
/// so a pickup that grows the hand past the two-row threshold changes their
/// bottom padding, and on the card spring the buttons visibly FLOATED up with
/// the cards arriving under them. Owner, round 7: "buttons should NEVER move /
/// float".
///
/// THE MECHANISM IS THE NESTED SAME-TRIGGER OVERRIDE, and only that: an
/// `.animation(nil, value:)` on the descendant carrying THE SAME trigger value
/// as the ancestor's, innermost wins. Which is why the trigger stays spelled
/// out at every call site rather than being reached for inside here - the
/// identity of the two values IS the mechanism, and a site passing something
/// else is a site that does not work. Two other spellings were tried and
/// rejected for this vector: `.transaction { $0.animation = nil }` on the
/// chrome, which does not reliably beat a scoped value-animation (that is the
/// earlier fix the role mark went on drifting through), and keying on
/// `handHeight`, which is simply the wrong value - the change arrives through
/// `controller.view`.
///
/// THIS IS ONE OF THREE DIFFERENT VECTORS, NOT THREE TRIES AT ONE. Read from
/// the comments alone the trio looks like a band-aid on a band-aid on a
/// band-aid - it has been misread that way, and acted on - so: they cover
/// three unrelated ways the chrome can end up moving, and deleting any one of
/// them puts a separately filmed bug back.
///
///   1. THIS. The board's card spring reaching chrome inside its scope,
///      because the change is driven by `controller.view`. Five sites.
///   2. `actionBar`'s `.transaction { $0.animation = nil }`, confined to its
///      fixed-size 128x88 container. That covers the INSERTION of the Undo
///      pill into the column and its position within it - a change NOT driven
///      by `controller.view` at all (it rides whatever transaction is in
///      flight, e.g. the collapse's own `withAnimation`), so nothing keyed on
///      `controller.view` can reach it. Round 10g filmed Undo arriving ~295pt
///      above its slot and flying down over ~7 frames.
///   3. The `buttonLift` mirror. That gives the chrome its OWN honest
///      animation (`FMotion.chrome`) for the one change that genuinely does
///      move it, a row-count change, and it is deliberately driven from an
///      `.onChange` rather than by `controller.view` so that (1) cannot null
///      it. Round 36 asked for that slide explicitly ("at least make it slide
///      smoothly instead of jumping"), and (2) had to be NARROWED from the
///      placement down to the container because it was killing it.
///
/// INTERNAL, NOT FILEPRIVATE, because the board it serves is no longer the file
/// it is written in. That is the whole of the widening: FoolishKit-visible, not
/// public, so the export surface this framework dead-strips against is
/// unchanged - and `BoardSpringOverrideTests` still counts the sites that spend
/// it, so a sixth one cannot appear quietly.
extension View {
    func doesNotRideTheBoardSpring<Trigger: Equatable>(_ trigger: Trigger) -> some View {
        animation(nil, value: trigger)
    }
}

/// `scrollBounceBehavior(.basedOnSize)` behind an availability check - a
/// results screen that fits should feel like a fixed screen, not a scroll view
/// that rubber-bands when you brush it. Shared with the LOBBY, which scrolls for
/// the same reason and must feel the same when it does not have to. The project still deploys to iOS 16.0,
/// where the modifier does not exist yet (16.4); there it simply bounces, which
/// is the pre-round-16 ScrollView-less screen's only visible difference.
struct BounceOnlyWhenTooTall: ViewModifier {
    func body(content: Content) -> some View {
        if #available(iOS 16.4, *) {
            content.scrollBounceBehavior(.basedOnSize)
        } else {
            content
        }
    }
}
