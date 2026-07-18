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
    public static func target(at point: CGPoint, battles: [Int: CGRect],
                              handFrame: CGRect) -> PlayTarget {
        if let hit = battles.first(where: { $0.value.insetBy(dx: -8, dy: -8).contains(point) }) {
            return .battle(hit.key)
        }
        if handFrame.contains(point) { return .hand }
        return .table
    }
}
