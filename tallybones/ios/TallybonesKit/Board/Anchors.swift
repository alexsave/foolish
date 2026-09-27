// COPIED from pickemup/ios/PickemupKit/Board/Anchors.swift at 8e216923 - replaced by the shared-board lift
//
// Where every element of the table is, by name, so the kernel's beats (T9)
// can aim at an element without asking any view where it is. pickemup's one
// preference key and a name per anchor, without its PkFX half: the only beat
// drawn today is the dice settle, which the tray applies itself (BeatPlayer),
// so there is no effect to route by anchor yet (DECISIONS T62).
//
// The names: die.i (i 0...4), tray, pill.roll, seat.k, card, row.c (c a
// Category raw value), status.

import SwiftUI

/// The board's named coordinate space (foolish's `boardSpace`).
public let boardSpace = "board"

public struct TbAnchorKey: PreferenceKey {
    public static let defaultValue: [String: CGRect] = [:]
    public static func reduce(value: inout [String: CGRect], nextValue: () -> [String: CGRect]) {
        value.merge(nextValue()) { _, new in new }
    }
}

public extension View {
    /// Report this view's frame in the board space under `name`.
    func tbAnchor(_ name: String) -> some View {
        background(GeometryReader { g in
            Color.clear.preference(key: TbAnchorKey.self, value: [name: g.frame(in: .named(boardSpace))])
        })
    }
}

/// The newest frames of every named anchor, for the beats and the rig.
/// Written by the root on every layout.
@MainActor
public enum TbAnchors {
    public static var latest: [String: CGRect] = [:]
}
