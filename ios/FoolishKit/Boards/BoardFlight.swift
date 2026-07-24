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

// web ANIMATION_TIME = 500ms; HARNESS_SLOWMO=N (dev) scales it up so flights are
// catchable in screenshots / easy to eyeball.
public var flightTime: Double {
    #if DEBUG
    if let s = ProcessInfo.processInfo.environment["HARNESS_SLOWMO"], let n = Double(s), n > 0 {
        return 0.5 * n
    }
    #endif
    return 0.5
}
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

/// Per-battle-CARD rects in `boardSpace`, keyed by card.identity — where each
/// attack/defense card ACTUALLY sits on the table. Round-7 #2: a bout-end discard
/// sweep flies each trashed card from ITS OWN rect, so the overlay ghost spawns
/// exactly where the real card was (reading as that card flying to the pile),
/// instead of every card sharing one reconstructed table centroid and bunching
/// up in the middle ("identical cards appear very close together, then fly").
public struct BattleCardFramesKey: PreferenceKey {
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
    /// Round-6 bug 1: the card's FINAL on-table rotation in degrees. A laid-across
    /// cover lands tilted, so its overlay ghost ROTATES 0 -> `angle` as it flies
    /// (matching the card it becomes) instead of arriving flat and the real card
    /// snapping tilted the instant the ghost is removed — the "blend transforms
    /// mid flight from not rotated to rotated" the round-6 device pass flagged.
    /// Defaulted 0 (upright), so every deck / discard / pickup / attack flight
    /// stays flat exactly as before; only the cover flight sets it.
    public let angle: Double
    /// The rotation the card STARTS at (round-6 bug 6). A card being swept to the
    /// discard was lying across its battle a moment ago, so its ghost lifts off
    /// still tilted and flattens into the pile (`fromAngle` = its table tilt,
    /// `angle` = 0) rather than snapping upright the instant it leaves. Defaulted
    /// 0, so a hand/deck flight starts flat exactly as before.
    public let fromAngle: Double
    public init(id: String, card: Card?, from: CGRect, to: CGRect,
                angle: Double = 0, fromAngle: Double = 0) {
        self.id = id; self.card = card; self.from = from; self.to = to
        self.angle = angle; self.fromAngle = fromAngle
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
    /// Non-zero while a board is mid multi-step ANIMATED sequence: a bout-end
    /// cascade (discard/pickup, then each drawing player's refill) or an
    /// open-delta replay (`MessageTableView.flyBoutEndToDiscard` /
    /// `replayLastMoveOnOpen`, both wrap their Task in `sequenceDepth += 1` /
    /// `-= 1`). Originally HARNESS_AUTOGAME-only (it waited on this before
    /// switching players); note 8 promoted it into the real extension's own
    /// completion signal too — see `waitForSettle` below, which
    /// `MessagesViewController.stage` awaits instead of guessing a fixed
    /// sleep long enough for the longest possible sequence.
    public static var sequenceDepth = 0
    public static var isSequencing: Bool { sequenceDepth > 0 }

    /// note 8: block until no animated sequence is running, instead of a
    /// caller guessing how long one might take. A plain attack/cover (no
    /// bout end) never touches `sequenceDepth` at all — matchedGeometry's own
    /// spring animates it — so this returns almost immediately then; a bout
    /// end or open-delta replay can run several steps at ~`flightTime` +
    /// `flightGap` (≈0.55s) each, and this waits out the real total instead
    /// of a constant tuned for the common short case (the bug an all-players-
    /// draw sequence used to get cut off mid-flight by, note 8). `timeout`
    /// bounds the wait: an unbalanced `sequenceDepth` increment (a bug, not a
    /// real long sequence) must never wedge the caller forever. 8s is
    /// comfortably above the longest sequence today (an 8-seat bout end: one
    /// discard/pickup step + up to 7 draw steps, each ≈0.55s, is under 4.5s).
    public static func waitForSettle(pollInterval: UInt64 = 100_000_000,
                                     timeout: TimeInterval = 8.0) async {
        let deadline = Date().addingTimeInterval(timeout)
        while isSequencing, Date() < deadline {
            try? await Task.sleep(nanoseconds: pollInterval)
        }
    }

    /// Cards currently in flight (rendered by FlyingCardsLayer). `progress` 0→1
    /// drives the position/scale tween.
    @Published public private(set) var flights: [Flight] = []
    @Published public private(set) var progress: Double = 0
    /// Card identities currently in flight — the board hides these so the ghost
    /// reads as the card lifting out and landing (web CardFace opacity:0).
    @Published public private(set) var hidden: Set<String> = []
    @Published public private(set) var isAnimating = false
    /// note 36: cards that already exist in the model (a refilled hand) but
    /// have not YET had their deck→hand (or table→hand) flight play — hidden
    /// the instant a caller predicts them, synchronously, well before `play`
    /// gets around to that step. Without this a newly-drawn card renders at
    /// its landing spot for a beat, THEN vanishes and re-flies in (the flash).
    /// Kept separate from `hidden` (which also covers the CURRENT in-flight
    /// step) so a later step starting doesn't accidentally un-hide a
    /// still-pending prediction for a LATER step.
    ///
    /// Round-6 bug 10: also the "reserve no hand slot yet" set. A card in here
    /// is a deal/pickup PREDICTED for a step that has not started, so the hand
    /// fan must not widen for it yet (my present cards must not slide left in
    /// anticipation while OTHER seats' deals animate first). `openSlots` pulls a
    /// card out of here the instant its own step begins — still opacity-hidden
    /// (`hidden` keeps it) but now laid out, so its landing frame publishes and
    /// the fan opens for it AS it arrives. Published so the board's layout reacts.
    @Published public private(set) var preHidden: Set<String> = []

    public init() {}

    /// Synchronously hide `ids` before their flight is even scheduled (note
    /// 36). Callers predict these — e.g. "my hand minus what I had before" —
    /// and must call this BEFORE the state change that would render them
    /// renders, so there is no gap for the flash. `play` removes an id from
    /// this set the moment its OWN step actually plays it; `clearPreHidden`
    /// is the safety net for anything predicted but never consumed.
    public func preHide(_ ids: Set<String>) {
        AnimLog.say("veil preHide [\(ids.sorted().joined(separator: ","))]")
        preHidden.formUnion(ids)
        hidden.formUnion(ids)
    }

    /// Round-6 bug 10: this step is about to fly, so its incoming hand cards
    /// stop being "future predictions" and start reserving their real hand
    /// slots NOW — a beat before their flight builds and plays. They leave
    /// `preHidden` (so `MessageTableView.handSlotDeferred` no longer excludes
    /// them and the fan opens for them, publishing their landing frame) but
    /// stay in `hidden`, so they are still invisible until their flight lands.
    /// The caller wraps this in a `withAnimation` matching the flight so the
    /// fan makes room over the flight rather than jumping. A no-op for ids not
    /// currently predicted.
    public func openSlots(_ ids: Set<String>) {
        AnimLog.say("veil openSlots [\(ids.sorted().joined(separator: ","))] (make room, still hidden)")
        preHidden.subtract(ids)
    }

    /// Round-6 bug 13: un-hide exactly `ids`, whether they were pre-hidden for a
    /// flight that then played, one that never got built, or one that was
    /// abandoned. The targeted twin of `clearPreHidden` below: that one hands
    /// back EVERYTHING still pending, which a caller cleaning up after its own
    /// cards must not do — it would also reveal what a newer, unrelated sequence
    /// has pre-hidden but not yet flown (the bug-9 double animation). A no-op
    /// for ids that are not hidden, so it is safe to call as a blanket net.
    public func reveal(_ ids: Set<String>) {
        let shown = ids.filter { hidden.contains($0) }
        if !shown.isEmpty { AnimLog.say("veil reveal (pop IN) [\(shown.sorted().joined(separator: ","))]") }
        preHidden.subtract(ids)
        hidden.subtract(ids)
    }

    /// Force-reveal any still-pending pre-hidden ids — called once a whole
    /// sequence (bout-end, or an open-delta replay) finishes, so a prediction
    /// that never got consumed (e.g. a frame that never became ready) cannot
    /// leave a card invisible forever.
    public func clearPreHidden() {
        if !preHidden.isEmpty { AnimLog.say("veil clearPreHidden (pop IN) [\(preHidden.sorted().joined(separator: ","))]") }
        hidden.subtract(preHidden)
        preHidden.removeAll()
    }

    public func play(_ steps: [FlightStep]) async {
        for step in steps where !step.isEmpty {
            isAnimating = true
            flights = step
            let ids = Set(step.compactMap { $0.card?.identity })
            AnimLog.say("flight START [\(step.map { f in "\(f.card?.identity ?? "back"):\(Int(f.from.midX)),\(Int(f.from.midY))->\(Int(f.to.midX)),\(Int(f.to.midY))" }.joined(separator: " "))]")
            preHidden.subtract(ids)          // these are now this step's OWN hidden cards
            hidden = preHidden.union(ids)
            progress = 0
            // One paint at from-position, then animate to-position.
            try? await Task.sleep(nanoseconds: 25_000_000)
            withAnimation(.timingCurve(0.25, 0.46, 0.45, 0.94, duration: flightTime)) {
                progress = 1
            }
            try? await Task.sleep(nanoseconds: UInt64(flightTime * 1_000_000_000))
            try? await Task.sleep(nanoseconds: UInt64(flightGap * 1_000_000_000))
            AnimLog.say("flight LAND (pop IN at dest) [\(ids.sorted().joined(separator: ","))]")
        }
        // Un-hide this call's own step ids, but leave any OTHER pending
        // pre-hidden cards (for a step not yet reached) hidden.
        flights = []; hidden = preHidden; isAnimating = false; progress = 0
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
                    // Bug 1: rotate INTO the final table angle over the flight,
                    // about the bottom edge (the same pivot FBattleGrid tilts a
                    // laid-across card about), so a cover flies in already
                    // rotating and the hand-off to the real tilted card is
                    // seamless. `angle` is 0 for everything that lands flat.
                    .rotationEffect(.degrees(f.fromAngle + (f.angle - f.fromAngle) * p), anchor: .bottom)
                    .scaleEffect(1.0 + 0.15 * (1 - abs(p - 0.5) * 2))   // bulge mid-flight
                    .shadow(color: .black.opacity(0.4 * p), radius: 10 * p, y: 8 * p)
                    .position(x: cx, y: cy)
            }
        }
        .allowsHitTesting(false)
    }
}
