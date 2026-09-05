// BoardDrag.swift — the shared drag-to-play plumbing (web DragContext parity).
//
// A hand card can be DRAGGED onto the table (attack, or pass/auto-cover for a
// defender) or onto a specific uncovered attack (cover). To decide which, the
// board needs the on-screen frames of the battle slots and the hand, all in one
// coordinate space ("board"). FBattleGrid and FHandFan publish their frames up
// through these preference keys; `BoardDrop.target` turns a release point into a
// PlayTarget, and CardPlay resolves that against the kernel menu — exactly like
// the web's getTableCardUnderCursor + determineGameAction.

import SwiftUI

/// The shared coordinate space every board names on its root so drag locations
/// and reported frames agree.
public let boardSpace = "board"

/// battle index → its frame in `boardSpace`.
public struct BattleFramesKey: PreferenceKey {
    public static let defaultValue: [Int: CGRect] = [:]
    public static func reduce(value: inout [Int: CGRect], nextValue: () -> [Int: CGRect]) {
        value.merge(nextValue()) { _, new in new }
    }
}

/// The hand's frame in `boardSpace` — dropping back inside it cancels the play.
public struct HandFrameKey: PreferenceKey {
    public static let defaultValue: CGRect = .zero
    public static func reduce(value: inout CGRect, nextValue: () -> CGRect) {
        let n = nextValue()
        if n != .zero { value = n }
    }
}

/// Applies a card-flight matchedGeometryEffect only when a shared namespace is
/// given — so a card animates as it moves between the hand and the table (same
/// card.identity in both places). No namespace ⇒ no effect (previews/tests).
///
/// THE INVARIANT THAT MAKES THIS SAFE, and the reason two guards could be
/// deleted in round 43:
///
///     A BOARD EITHER VEILS CARDS OR SHARES A NAMESPACE. NEVER BOTH.
///
/// The offline `TableView` shares a namespace and passes no `hidden` set - its
/// flights ARE matchedGeometry. The message board veils cards and passes no
/// namespace - `BoardAnimator`'s overlay owns every flight there, and a shared
/// namespace would fly each card a second time, cross-fading between the two
/// copies (an opacity animation on a card, the one thing this game never does).
///
/// Round-7 #2 wrote that rule per-card instead, in two places: FHandFan and
/// FBattleGrid each computed `hidden.contains(id) ? nil : namespace`, to drop
/// the namespace for a card the overlay was flying. Both were structurally
/// unreachable by the time they were read - on the veiling board `namespace`
/// was already nil, and on the namespace board `hidden` is always empty - so
/// each was a guard against a combination neither caller could produce.
/// Stating the rule once, here, is the whole of what those two lines did.
///
/// Pinned by `FlightNamespaceInvariantTests`, which fails if a board ever
/// starts passing both.
public struct FlightID: ViewModifier {
    public let id: String
    public let namespace: Namespace.ID?
    public init(id: String, namespace: Namespace.ID?) { self.id = id; self.namespace = namespace }

    @ViewBuilder public func body(content: Content) -> some View {
        if let namespace {
            content.matchedGeometryEffect(id: id, in: namespace, isSource: true)
        } else {
            content
        }
    }
}

public enum BoardDrop {
    /// Resolve a release point to a play target: a specific uncovered attack if the
    /// point lands on its slot, the hand (cancel) if it fell back into the fan, else
    /// the open table (attack / pass).
    /// The slack each slot's rect grows by, so a release just outside a card still
    /// covers it. It is larger than FBattleGrid's own gap (10 across, 12 down), so
    /// neighbouring inflated rects OVERLAP by a few points — which is why the hit
    /// below must break ties deterministically rather than take whichever it meets
    /// first.
    private static let slack: CGFloat = 8

    public static func target(at point: CGPoint, battles: [Int: CGRect],
                              handFrame: CGRect) -> PlayTarget {
        // NEAREST CENTRE WINS among the slots the point falls in. Two things make
        // the obvious `first(where:)` wrong: the inflated rects overlap (see
        // `slack`), and `battles` is a DICTIONARY — Swift seeds hash order randomly
        // per process, so `first` over an overlap band picked an arbitrary battle,
        // and not even the same one across launches. That made a release in the gap
        // cover the wrong attack (when both were coverable), and made the drag verb
        // hint flicker between "Cover" and "Pass" while hovering there.
        let hit = battles
            .filter { $0.value.insetBy(dx: -slack, dy: -slack).contains(point) }
            .min { a, b in
                let da = hypot(point.x - a.value.midX, point.y - a.value.midY)
                let db = hypot(point.x - b.value.midX, point.y - b.value.midY)
                // Distance ties (a point exactly equidistant from two centres) fall
                // back to the lower battle index, so the result is total.
                return da == db ? a.key < b.key : da < db
            }
        if let hit { return .battle(hit.key) }
        if handFrame.contains(point) { return .hand }
        return .table
    }
}
