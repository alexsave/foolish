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
    /// Report this view's frame in the board space under `name`, and take
    /// whatever motion the playing plan gives that anchor (PkFX). The frame is
    /// the laid-out one, not the moved one, so a flight aims at where a thing
    /// rests.
    func pkAnchor(_ name: String) -> some View {
        // the motion INSIDE, the measuring outside it: a riffle or a stamp
        // never moves the frame a flight aims at
        pkFX(name)
            .background(GeometryReader { g in
                Color.clear.preference(key: PkAnchorKey.self, value: [name: g.frame(in: .named(boardSpace))])
            })
    }

    /// The motion alone, for a part with no anchor of its own (a deck layer,
    /// one card of a fan).
    func pkFX(_ name: String) -> some View { modifier(PkFXModifier(name: name)) }
}

/// What a beat does to an anchored element at this moment: every value is a
/// sample of the kernel's timeline (pk_beat_sample), mapped to the element by
/// BeatPlayer.effects. Identity when nothing plays.
public struct PkFX: Equatable {
    public var opacity: CGFloat = 1
    public var scale: CGFloat = 1
    public var scaleX: CGFloat = 1
    public var dx: CGFloat = 0
    public var dy: CGFloat = 0
    public var rot: Double = 0          // degrees about Z
    public var rotY: Double = 0         // degrees about Y (the direction box's TURN)
    /// A skipped fan's red bar, how far across (0...1).
    public var slash: CGFloat?
    /// A turn bar still fading in (or out) under a fan.
    public var bar: CGFloat?
    /// A called fan's ring, fading in or out.
    public var ring: CGFloat?
    public init() {}
}

public struct PkFXKey: EnvironmentKey {
    public static let defaultValue: [String: PkFX] = [:]
}

public extension EnvironmentValues {
    var pkFX: [String: PkFX] {
        get { self[PkFXKey.self] }
        set { self[PkFXKey.self] = newValue }
    }
}

struct PkFXModifier: ViewModifier {
    let name: String
    @Environment(\.pkFX) private var all

    func body(content: Content) -> some View {
        let f = all[name] ?? PkFX()
        content
            .scaleEffect(x: f.scale * f.scaleX, y: f.scale)
            .rotationEffect(.degrees(f.rot))
            .rotation3DEffect(.degrees(f.rotY), axis: (x: 0, y: 1, z: 0))
            .offset(x: f.dx, y: f.dy)
            .opacity(f.opacity)
    }
}
