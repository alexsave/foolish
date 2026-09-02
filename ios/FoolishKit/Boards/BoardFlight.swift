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
// catchable in screenshots / easy to eyeball. The harness reads it from its
// environment; the iMessage extension cannot (it is spawned by the system, so
// SIMCTL_CHILD_ never reaches it) and reads the `dev.slowmo` file instead.
public var flightTime: Double {
    #if DEBUG
    if let s = ProcessInfo.processInfo.environment["HARNESS_SLOWMO"], let n = Double(s), n > 0 {
        return 0.5 * n
    }
    if MessageDevBoard.slowmo > 0 { return 0.5 * MessageDevBoard.slowmo }
    #endif
    return 0.5
}
public let flightGap: Double = 0.025       // web inter-event queue gap = 25ms


/// ROUND 16: the HOLD between a cover that ENDED THE BOUT and the sweep that
/// clears the table (owner: "when you cover and cause the deck to discard (last
/// defense), it should give some time to let people see what you covered with").
///
/// Every other beat in a sequence is a card moving, so `flightGap`'s 25ms is
/// enough - the eye is following the motion, not reading the board. This one is
/// different: the cover lands and the very next beat takes it away again, so the
/// card that decided the bout is on the table for about a frame. The hold is a
/// beat of NOTHING moving, which is the only thing that makes a board readable.
///
/// Expressed against `flightTime` rather than as a bare 1.5 so it scales with
/// HARNESS_SLOWMO like every other duration here - a filmed sequence keeps its
/// proportions instead of the hold shrinking to nothing as the flights stretch.
///
/// ROUND 20 took it from 0.9x to 3x a flight - 1.5 seconds at the shipping
/// `flightTime` (owner: "for last defense, still not enough of a pause in
/// animation when they cover. Both for finish and for the other one. Make it
/// like 1.5 second"). "Both" is the two ends `holdsAfter` scans for: the bout
/// that closes into the DISCARD, and the last one of a game, which closes into
/// the TRASH. Three flights' worth is deliberately longer than anything else on
/// this board: the point is that the eye STOPS, and half a flight was still
/// being read as part of the motion around it.
public var boutEndHold: Double { flightTime * 3 }

/// ROUND 28: the HOLD between the last move of a game and the RANKS replacing
/// the board (owner, on the 1.0(28) walk of the animation catalogue: "hold for 1
/// second").
///
/// Cousin to `boutEndHold` and there for the same reason - the final board is
/// the one nobody gets a second look at, because the next thing on screen is a
/// results screen and there is no bubble to reopen that shows the table again.
/// It was 500ms, hard-coded in `settleResults`, which reads as the board being
/// taken away from you rather than given to you.
///
/// Against `flightTime` like every other duration here, so a filmed game-over
/// keeps its proportions under HARNESS_SLOWMO: 2x a flight is 1.0s shipping.
public var gameOverHold: Double { flightTime * 2 }

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
    /// THE CONFLICT MODEL's red (docs/ANIMATION_CATALOGUE.md, 1.0(28)): true for
    /// a flight that is a RETRACTION - a card travelling BACK the way it came
    /// because a newer chain disowned the motion that placed it. A reversal and
    /// a play are the same motion with the sign flipped, so without a colour the
    /// player cannot tell "my card went down" from "my card came back"; the web
    /// settled on red after months of glitch-fixing (AnimationOverlay.tsx draws
    /// a reverting card with a red border, red shadow and a pink face) and the
    /// phone does not invent a second vocabulary. The tint lives on the flight
    /// GHOST only - see FlyingCardsLayer - so a card that lands back in a hand
    /// is a normal card again the moment it lands.
    public let revert: Bool
    public init(id: String, card: Card?, from: CGRect, to: CGRect,
                angle: Double = 0, fromAngle: Double = 0, revert: Bool = false) {
        self.id = id; self.card = card; self.from = from; self.to = to
        self.angle = angle; self.fromAngle = fromAngle; self.revert = revert
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
    /// discard/pickup step + up to 7 draw steps, each ≈0.55s, plus round 16's
    /// one `boutEndHold`, is under 5s).
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

    /// Round-8 (atomic takeoff): show `f` as ghosts resting AT THEIR SOURCE right
    /// now - synchronously, the same instant the caller veils the real cards -
    /// then hold them there (progress 0) until the real flight `play`s and carries
    /// them off. This closes the gap the owner saw as "the card disappears for a
    /// few frames": a played card's hand copy is veiled the instant it is played,
    /// but its overlay flight could not START until the kernel `apply` had
    /// published a table slot to fly TO (an `await` later) - so for those frames
    /// the card was veiled in the hand and not yet anywhere in the overlay. The
    /// ghost now appears in the SAME frame the hand copy vanishes, sitting exactly
    /// where the card was, so the swap reads as one object, never a blink. `play`
    /// reuses the SAME flight ids (`place-<id>`), so the ghost view persists and
    /// simply starts moving - no re-spawn. Cleared by `play` on success, or by
    /// `cancelHeld` if the move is rejected and never flies.
    public func showHeld(_ f: [Flight]) {
        guard !f.isEmpty else { return }
        AnimLog.say("held ghost at source [\(f.map { $0.card?.identity ?? "?" }.sorted().joined(separator: ","))]")
        flights = f
        hidden.formUnion(Set(f.compactMap { $0.card?.identity }))
        progress = 0
        isAnimating = true
    }

    /// Round-8: drop a held ghost that will never fly (its move was rejected), so
    /// the resting ghost does not linger at the source after its real card has
    /// been revealed back into the hand. Only clears the overlay if these ARE the
    /// held ghosts (nothing else is mid-flight), never a real in-flight step.
    public func cancelHeld(_ ids: Set<String>) {
        guard !isAnimating || progress == 0 else { return }
        let mine = flights.filter { $0.card.map { ids.contains($0.identity) } ?? false }
        guard !mine.isEmpty else { return }
        flights = []
        isAnimating = false
        progress = 0
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
    /// The web's revert red - rgb(220, 38, 38) - and the pink face wash that
    /// stands in for its `backgroundColor: rgb(255,150,150)` + warm filter.
    /// One place, so the retraction colour cannot fork between call sites.
    public static let revertRed = Color(red: 220 / 255, green: 38 / 255, blue: 38 / 255)
    public static let revertWash = Color(red: 1.0, green: 150 / 255, blue: 150 / 255)

    @ObservedObject var animator: BoardAnimator
    public init(animator: BoardAnimator) { self.animator = animator }

    public var body: some View {
        ZStack {
            ForEach(animator.flights) { f in
                let p = animator.progress
                let cx = f.from.midX + (f.to.midX - f.from.midX) * p
                let cy = f.from.midY + (f.to.midY - f.from.midY) * p
                FCard(card: f.card, size: CGSize(width: 50, height: 70))
                    // THE CONFLICT MODEL's red, on the ghost only - the web's
                    // exact vocabulary for a superseded optimistic card
                    // (AnimationOverlay.tsx: border rgb(220,38,38), red drop
                    // shadow, warmed filter, pink face). A pink wash over the
                    // face plus the red border reads as "this card is being
                    // taken back", and because it is drawn HERE rather than in
                    // FCard, the card that lands back in a hand is untouched.
                    .overlay(
                        RoundedRectangle(cornerRadius: 5)
                            .fill(FlyingCardsLayer.revertWash.opacity(f.revert ? 0.35 : 0))
                    )
                    .overlay(
                        RoundedRectangle(cornerRadius: 5)
                            .strokeBorder(FlyingCardsLayer.revertRed, lineWidth: f.revert ? 2 : 0)
                    )
                    // Bug 1: rotate INTO the final table angle over the flight,
                    // about the bottom edge (the same pivot FBattleGrid tilts a
                    // laid-across card about), so a cover flies in already
                    // rotating and the hand-off to the real tilted card is
                    // seamless. `angle` is 0 for everything that lands flat.
                    .rotationEffect(.degrees(f.fromAngle + (f.angle - f.fromAngle) * p), anchor: .bottom)
                    .scaleEffect(1.0 + 0.15 * (1 - abs(p - 0.5) * 2))   // bulge mid-flight
                    .shadow(color: f.revert ? FlyingCardsLayer.revertRed.opacity(0.6 * p)
                                            : .black.opacity(0.4 * p),
                            radius: 10 * p, y: 8 * p)
                    .position(x: cx, y: cy)
                    // A CARD NEVER FADES, AND A GHOST IS A CARD.
                    //
                    // FBattleGrid's rule, applied to the overlay it hands off
                    // to. This ForEach's membership changes at every step
                    // boundary and again at teardown (`flights = []`), and
                    // SwiftUI's default for a view leaving a container is
                    // `.opacity` - so a ghost cleared inside any ambient
                    // transaction dissolves in mid-air instead of handing over
                    // to the real card at its destination. Owner, on a
                    // retracted pickup: "Those cards then FADED. We should
                    // NEVER fade cards in this game. Real life cards don't ever
                    // fade like that! EVER!"
                    .transition(.identity)
            }
        }
        .allowsHitTesting(false)
    }
}
