// BoardFlight.swift — flights to/from the deck and discard piles.
//
// matchedGeometryEffect (FlightID) is the primary card animation — a card that
// persists as a view in both places (hand↔table) flies there for free, GPU-driven.
// But cards leaving TO the discard pile (which shows only backs + a count) or
// coming FROM the deck have no persistent destination/source view to match, so
// those get a small overlay of cards whose centre is interpolated by a single
// `withAnimation` over a `progress` value — SwiftUI's own animation curve, not a
// hand-rolled timer loop. It matches the web's flight FEEL (the web uses one
// overlay for everything; iOS uses matchedGeometry where it can and this overlay
// only where it must). Anchor preference keys publish the deck/discard/hand-card
// rects in `boardSpace`. Card values are the kernel {s,v}; a hidden card ({-1,-1})
// renders as a back. flightTime matches the web's 500ms.

import SwiftUI

public let flightTime: Double = 0.5        // web ANIMATION_TIME = 500ms
public let flightGap: Double = 0.025       // web inter-event queue gap = 25ms

/// The deck pile's rect in `boardSpace` (draw source / flip source).
public struct DeckFrameKey: PreferenceKey {
    public static let defaultValue: CGRect = .zero
    public static func reduce(value: inout CGRect, nextValue: () -> CGRect) {
        let n = nextValue(); if n != .zero { value = n }
    }
}

/// The discard pile's rect in `boardSpace` (cards_to_trash target).
public struct DiscardFrameKey: PreferenceKey {
    public static let defaultValue: CGRect = .zero
    public static func reduce(value: inout CGRect, nextValue: () -> CGRect) {
        let n = nextValue(); if n != .zero { value = n }
    }
}

/// Per-hand-card slot rects in `boardSpace`, keyed by card.identity (draw target /
/// pickup target for the local hand).
public struct HandCardFramesKey: PreferenceKey {
    public static let defaultValue: [String: CGRect] = [:]
    public static func reduce(value: inout [String: CGRect], nextValue: () -> [String: CGRect]) {
        value.merge(nextValue()) { _, new in new }
    }
}

/// Opponent seat rects in `boardSpace`, keyed by seat — the target for opponent
/// draws/pickup (masked card backs fly to their badge).
public struct SeatFramesKey: PreferenceKey {
    public static let defaultValue: [Int: CGRect] = [:]
    public static func reduce(value: inout [Int: CGRect], nextValue: () -> [Int: CGRect]) {
        value.merge(nextValue()) { _, new in new }
    }
}

/// One card in flight: a face (or masked back when `card` is nil/hidden) moving
/// from `from` to `to` in `boardSpace`.
public struct Flight: Identifiable, Equatable {
    public let id: String
    public let card: Card?
    public let from: CGRect
    public let to: CGRect
    public init(id: String, card: Card?, from: CGRect, to: CGRect) {
        self.id = id; self.card = card; self.from = from; self.to = to
    }
}

/// A batch of cards that fly SIMULTANEOUSLY (the web animates all cards of one
/// event together, no intra-event stagger).
public typealias FlightStep = [Flight]

/// Plays overlay flights. `play(steps)` runs each step's cards as ONE
/// `withAnimation` over `flightTime`, awaiting between steps with structured
/// concurrency (a short cascade like discard→draws is a couple of steps, not a
/// long-lived event loop).
@MainActor
public final class BoardAnimator: ObservableObject {
    /// Cards currently in flight (rendered by FlyingCardsLayer). `progress` 0→1
    /// drives the position/scale tween.
    @Published public private(set) var flights: [Flight] = []
    @Published public private(set) var progress: Double = 0
    /// Card identities currently in flight — the board hides these so the ghost
    /// reads as the card lifting out and landing (web CardFace opacity:0).
    @Published public private(set) var hidden: Set<String> = []
    @Published public private(set) var isAnimating = false

    public init() {}

    public func play(_ steps: [FlightStep]) async {
        for step in steps where !step.isEmpty {
            isAnimating = true
            flights = step
            hidden = Set(step.compactMap { $0.card?.identity })
            progress = 0
            // One paint at from-position, then animate to-position.
            try? await Task.sleep(nanoseconds: 25_000_000)
            withAnimation(.timingCurve(0.25, 0.46, 0.45, 0.94, duration: flightTime)) {
                progress = 1
            }
            try? await Task.sleep(nanoseconds: UInt64(flightTime * 1_000_000_000))
            try? await Task.sleep(nanoseconds: UInt64(flightGap * 1_000_000_000))
        }
        flights = []; hidden = []; isAnimating = false; progress = 0
    }

    public func isHidden(_ identity: String) -> Bool { hidden.contains(identity) }
}

/// The overlay of in-flight cards, layered above the board in `boardSpace`. Each
/// card interpolates its centre from `from` to `to` and scales 1.0→1.15 (a lighter
/// version of the web's 1.5→1.8, which is tuned to the web's smaller cards).
public struct FlyingCardsLayer: View {
    @ObservedObject var animator: BoardAnimator
    public init(animator: BoardAnimator) { self.animator = animator }

    public var body: some View {
        ZStack {
            ForEach(animator.flights) { f in
                let p = animator.progress
                let cx = f.from.midX + (f.to.midX - f.from.midX) * p
                let cy = f.from.midY + (f.to.midY - f.from.midY) * p
                FCard(card: f.card, size: CGSize(width: 50, height: 70))
                    .scaleEffect(1.0 + 0.15 * (1 - abs(p - 0.5) * 2))   // bulge mid-flight
                    .shadow(color: .black.opacity(0.4 * p), radius: 10 * p, y: 8 * p)
                    .position(x: cx, y: cy)
            }
        }
        .allowsHitTesting(false)
    }
}
