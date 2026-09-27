// Anchors.swift - where every element of the table is, by the name UI.html's
// anchor table and motion grid use, so the flight layer that comes next can
// fly a card from one to another without asking any view where it is.
//
// THE SEAM FOR THE FLIGHTS. foolish's BoardFlight reads a handful of frame
// preference keys (DeckFrameKey, SeatFramesKey, HandCardFramesKey ...); this
// product has one key and a name per anchor instead, so the next worker
// reads `anchors["deck"]`, `anchors["fan.2"]`, `anchors["hand.4"]` and adds no
// plumbing. Every frame is in the board's coordinate space (`boardSpace`),
// the same space the drag gestures report in.
//
// The names (UI.html "What holds which edge"):
//   status, strip, dir, stack, halo, deck, deckn, picker, scrim,
//   seat.k, fan.k, slot.k (k a seat), hand (the whole row), hand.i (card i),
//   pills, pill.draw, pill.play, pill.pass, pill.undo, squares, pill.say,
//   bubble, caption (Messages' own: not on this board)

import SwiftUI

/// The board's named coordinate space (foolish's `boardSpace`).
public let boardSpace = "board"

public struct PkAnchorKey: PreferenceKey {
    public static let defaultValue: [String: CGRect] = [:]
    public static func reduce(value: inout [String: CGRect], nextValue: () -> [String: CGRect]) {
        value.merge(nextValue()) { _, new in new }
    }
}

public extension View {
    /// Report this view's frame in the board space under `name`.
    func pkAnchor(_ name: String) -> some View {
        background(GeometryReader { g in
            Color.clear.preference(key: PkAnchorKey.self, value: [name: g.frame(in: .named(boardSpace))])
        })
    }
}
