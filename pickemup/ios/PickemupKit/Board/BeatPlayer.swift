// BeatPlayer.swift - plays the kernel's timeline (pickemup/c/src/pk_beats.h).
//
// NO DURATION, CURVE OR ORDER LIVES HERE. A plan is what the kernel laid out
// (Pk.beats*); this holds which plan is current and when it began, and every
// frame asks C two things: the board as of now (pk_api_beats_frame: the count,
// the pile, whose turn, my hand with the cards still in the air unseen) and
// each beat's transform (pk_api_beat_sample). What is left for Swift is the
// rendering half of foolish's anim_plan boundary: WHICH ANCHOR a beat's sample
// moves, and a ghost card tweened between two anchors' frames.
//
// ONE PLAN AT A TIME, AND A NEW ONE WINS. `play` replaces whatever was
// playing, mid-flight: the superseded plan is dropped, never unwound (the
// clear-not-revert rule), and the new plan starts from the board the kernel
// says it starts from, not from what was on screen.
//
// THE CLOCK STARTS ON THE FIRST FRAME ANYBODY SEES (MOTION_REPORT Take B,
// IOS_DECISIONS I46). An opened bubble is adopted, and its plan handed here,
// while the extension's view is still hidden behind a white drawer; a clock
// started then played the first draws to nobody. So `play` only starts the
// clock when the board is on screen (`onScreen`, the controller's); otherwise
// the plan waits at its first frame, and the clock starts in the first BODY
// that samples it on screen (`ms()`, foolish's first-paint rule: derived where
// the frame is drawn, never in an onChange a paint later).
//
// A STAGED DRAFT HOLDS. A plan whose settle half is held until Send (`held`)
// stays current at its last frame after it ends, so the board keeps showing
// the draft as played rather than the kernel's settled state; Send's plan
// (channel B) replaces it.

import CPickemup
import QuartzCore
import SwiftUI

@MainActor
public final class BeatPlayer: ObservableObject {

    /// The current plan, or nil: the board shows the settled view.
    @Published public private(set) var plan: PkBeatsSnap?
    /// True while a frame can still change (the timeline is running).
    @Published public private(set) var animating = false

    /// When the current plan's clock started; nil while it waits for the
    /// board to be on screen.
    public private(set) var began: CFTimeInterval?
    private var endWork: DispatchWorkItem?
    /// The clock, replaceable by a test.
    public var now: () -> CFTimeInterval = { CACurrentMediaTime() }
    /// Is the board on screen? The controller says (the hosting view hidden
    /// until the drawer is up, the extension off screen after it disappears);
    /// a host with no controller (a test, a preview) is always on screen.
    @Published public var onScreen = true

    public init() {}

    /// Play `plan` from now. nil (the kernel could not lay it out) or an empty
    /// plan that holds nothing clears: the settled view, no motion.
    public func play(_ plan: PkBeatsSnap?) {
        endWork?.cancel()
        endWork = nil
        guard let plan, !plan.beat.isEmpty || plan.held > 0 else {
            clear()
            return
        }
        self.plan = plan
        began = nil
        animating = !plan.beat.isEmpty
        if onScreen { start() }
    }

    /// The plan's clock starts now, and its end is due totalMs from now.
    private func start() {
        guard let plan, began == nil else { return }
        began = now()
        let serial = plan.serial
        let work = DispatchWorkItem { [weak self] in self?.ended(serial) }
        endWork = work
        DispatchQueue.main.asyncAfter(deadline: .now() + Double(plan.totalMs) / 1000, execute: work)
    }

    /// Drop the plan: the settled view.
    public func clear() {
        endWork?.cancel()
        endWork = nil
        began = nil
        plan = nil
        animating = false
    }

    /// The plan ran out: a plan that holds a staged draft stays, frozen at its
    /// end; any other is done and the settled view takes over.
    private func ended(_ serial: Int) {
        guard let plan, plan.serial == serial else { return }
        animating = false
        if plan.held == 0 { self.plan = nil }
    }

    /// Milliseconds into the current plan. Called by the board's body on every
    /// frame it draws: the first one drawn on screen starts a waiting clock,
    /// and until then the plan stands at 0.
    public func ms(at t: CFTimeInterval? = nil) -> Int {
        guard plan != nil else { return 0 }
        if began == nil, onScreen { start() }
        guard let began else { return 0 }
        return max(0, Int(((t ?? now()) - began) * 1000))
    }

    /// Is the kernel's current plan the one this player holds? A build this
    /// player never played (a test's, a probe's) makes the samples unreadable.
    private var current: Bool {
        guard let plan else { return false }
        return plan.serial == Pk.beatsSerial
    }

    /// The board as of `ms`, or nil when the settled view is what shows.
    public func frame(_ ms: Int) -> PkBeatFrameSnap? {
        guard current else { return nil }
        return Pk.beatFrame(ms)
    }

    /// Is a beat of `kind` running or yet to run?
    public func pending(_ kind: Int, at ms: Int) -> Bool {
        guard let plan else { return false }
        return plan.beat.contains { $0.kind == kind && ms < $0.startMs + $0.durMs }
    }

    // MARK: the rendering half

    /// A card in the air: its face or back, where it is, how big, how turned.
    public struct Ghost: Equatable, Identifiable {
        public let id: Int
        public let card: Int?            // nil: a back
        public let center: CGPoint
        public let size: CGSize
        public let scale: CGFloat
        public let scaleX: CGFloat
        public let rot: Double
        public let dimmed: Bool          // a gathered under-card
        public let retract: Bool         // foolish's red retraction ghost
        public let from: String
        public let to: String
    }

    /// The anchor names a beat's (anchor, index) is drawn at.
    public static func anchorName(_ anc: Int, _ i: Int) -> String? {
        switch anc {
        case PK_ANC_DECK:    return "deck"
        case PK_ANC_STACK:   return "stack"
        case PK_ANC_HAND:    return "hand.\(i)"
        case PK_ANC_FAN:     return "fan.\(i)"
        case PK_ANC_SLOT:    return "slot.\(i)"
        case PK_ANC_SEAT:    return "seat.\(i)"
        case PK_ANC_DIR:     return "dir"
        case PK_ANC_STRIP:   return "strip"
        case PK_ANC_PICKER:  return "picker.\(i)"
        case PK_ANC_SCRIM:   return "scrim"
        case PK_ANC_BURY:    return "bury.\(i)"
        case PK_ANC_RESULTS: return "results"
        case PK_ANC_BOARD:   return "board"
        case PK_ANC_ROW:     return "roster.\(i)"
        default:             return nil
        }
    }

    /// Where a flight starts or lands: the anchor's frame, except a fan, where
    /// a back lands at (or leaves from) the fan's right end, one fan card big.
    static func rect(_ anc: Int, _ i: Int, _ anchors: [String: CGRect]) -> CGRect? {
        guard let name = anchorName(anc, i) else { return nil }
        if anc == PK_ANC_HAND, anchors[name] == nil, let hand = anchors["hand"] {
            return CGRect(x: hand.maxX - PkLayout.deckSize.width, y: hand.minY,
                          width: PkLayout.deckSize.width, height: PkLayout.cardH)
        }
        guard let r = anchors[name] else { return nil }
        if anc == PK_ANC_FAN {
            let c = PkLayout.fanCard
            return CGRect(x: r.maxX - c.width, y: r.midY - c.height / 2, width: c.width, height: c.height)
        }
        return r
    }

    /// Every card in the air at `ms`: flights, gathered under-cards and flips
    /// over a hand slot. An anchor that is not on screen (my own seat has no
    /// fan) drops the ghost, never the timing.
    public func ghosts(_ ms: Int, anchors: [String: CGRect]) -> [Ghost] {
        guard current, let plan else { return [] }
        var out: [Ghost] = []
        for (k, b) in plan.beat.enumerated() {
            guard b.kind == PK_BK_FLIGHT || b.kind == PK_BK_GATHER || (b.kind == PK_BK_FLIP && b.to != PK_ANC_FAN)
            else { continue }
            if ms < b.startMs || ms >= b.startMs + b.durMs { continue }
            for part in 0..<max(b.parts, 1) {
                guard let s = Pk.beatSample(k, part: part, ms: ms), s.state == PK_BS_ACTIVE else { continue }
                let card = b.card == PK_CARD_HIDDEN || b.card == PK_CARD_NONE || s.face == 0 ? nil : b.card
                if b.kind == PK_BK_FLIP {
                    guard let r = Self.rect(b.to, b.toI, anchors) else { continue }
                    out.append(Ghost(id: k * 16 + part, card: card, center: CGPoint(x: r.midX, y: r.midY), size: r.size,
                                     scale: 1, scaleX: CGFloat(s.scaleX), rot: 0, dimmed: false, retract: false,
                                     from: Self.anchorName(b.to, b.toI) ?? "", to: Self.anchorName(b.to, b.toI) ?? ""))
                    continue
                }
                guard let a = Self.rect(b.from, b.fromI, anchors), let z = Self.rect(b.to, b.toI, anchors) else { continue }
                let p = CGFloat(s.p)
                let center = CGPoint(x: a.midX + (z.midX - a.midX) * p, y: a.midY + (z.midY - a.midY) * p)
                let size = CGSize(width: a.width + (z.width - a.width) * p, height: a.height + (z.height - a.height) * p)
                out.append(Ghost(id: k * 16 + part, card: card, center: center, size: size, scale: CGFloat(s.scale),
                                 scaleX: 1, rot: Double(s.rot), dimmed: b.kind == PK_BK_GATHER,
                                 retract: b.flags & PK_BF_RETRACT != 0,
                                 from: Self.anchorName(b.from, b.fromI) ?? "", to: Self.anchorName(b.to, b.toI) ?? ""))
            }
        }
        return out
    }

    /// What every anchored element does at `ms` (Anchors.swift applies it).
    public func effects(_ ms: Int, anchors: [String: CGRect]) -> [String: PkFX] {
        guard current, let plan else { return [:] }
        var fx: [String: PkFX] = [:]
        func edit(_ name: String, _ f: (inout PkFX) -> Void) {
            var v = fx[name] ?? PkFX()
            f(&v)
            fx[name] = v
        }
        for (k, b) in plan.beat.enumerated() {
            // THE LOBBY'S LEAVE (grid "Leave", A13): the row that went fades
            // out where it stood, and the rows from its seat on stand one row
            // lower until the close-up (a HOLD on the card spring) lifts them.
            // Both hold from the plan's start, before either beat begins, so
            // the roster never shows the closed-up rows early.
            if b.to == PK_ANC_ROW, b.kind == PK_BK_FADE, b.sub == 0 {
                if let s = Pk.beatSample(k, ms: ms) {
                    edit("roster.gone") { $0.gone = b.toI; $0.opacity = s.apply != 0 ? CGFloat(s.opacity) : 1 }
                }
                continue
            }
            if b.to == PK_ANC_ROW, b.kind == PK_BK_HOLD {
                if let s = Pk.beatSample(k, ms: ms) {
                    let open: CGFloat = s.state == PK_BS_PENDING ? 1 : s.state == PK_BS_ACTIVE ? 1 - CGFloat(s.p) : 0
                    for r in b.toI..<PK_MAX_SEATS { edit("roster.\(r)") { $0.close = open } }
                }
                continue
            }
            switch b.kind {
            case PK_BK_FLIGHT, PK_BK_GATHER, PK_BK_HOLD:
                continue                      // ghosts, a rest
            case PK_BK_FLIP where b.to != PK_ANC_FAN:
                continue
            default:
                break
            }
            for part in 0..<max(b.parts, 1) {
                guard let s = Pk.beatSample(k, part: part, ms: ms), s.apply != 0 else { continue }
                let p = CGFloat(s.p)
                switch b.kind {
                case PK_BK_FLIP:              // the end reveal: one card of a fan turns
                    edit("fan.\(b.fromI).\(b.toI)") { $0.scaleX = CGFloat(s.scaleX) }
                case PK_BK_RIFFLE:
                    edit("deck.layer.\(part)") { $0.dx = CGFloat(s.dx); $0.rot = Double(s.rot) }
                case PK_BK_FATTEN:
                    edit("deck") { $0.scale *= CGFloat(s.scale) }
                case PK_BK_SHAKE:
                    edit(b.to == PK_ANC_DECK ? "deck" : "hand.\(b.toI)") { $0.dx += CGFloat(s.dx) }
                case PK_BK_HALO:
                    edit("halo") { $0.opacity *= CGFloat(s.opacity) }
                case PK_BK_BAND:              // the pile's wild draws it (PkCard.bandFX, A12)
                    edit("band") { $0.band = p }
                case PK_BK_STAMP:
                    edit("slot.\(b.toI)") { $0.scale *= CGFloat(s.scale); $0.opacity *= CGFloat(s.opacity) }
                case PK_BK_SLASH:
                    edit("fan.\(b.toI)") { $0.slash = p }
                case PK_BK_DIM:
                    if b.to == PK_ANC_HAND {
                        edit("hand") { $0.opacity *= CGFloat(s.opacity) }
                    } else {
                        for n in ["seat.\(b.toI)", "fan.\(b.toI)", "slot.\(b.toI)"] { edit(n) { $0.opacity *= CGFloat(s.opacity) } }
                    }
                case PK_BK_TURN_BAR:
                    if b.fromI != b.toI && b.fromI != PK_SEAT_NONE { edit("bar.\(b.fromI)") { $0.bar = 1 - p } }
                    edit("bar.\(b.toI)") { $0.bar = p }
                case PK_BK_TURN:
                    edit("dir") { $0.rotY = Double(s.rot) }
                case PK_BK_FADE:
                    if b.to == PK_ANC_FAN {
                        edit("ring.\(b.toI)") { $0.ring = CGFloat(s.opacity) }
                    } else if let n = Self.anchorName(b.to, b.toI) {
                        edit(n) { $0.opacity *= CGFloat(s.opacity) }
                    }
                case PK_BK_SHRUG, PK_BK_PULSE:
                    if let n = Self.anchorName(b.to, b.toI) { edit(n) { $0.scale *= CGFloat(s.scale) } }
                case PK_BK_RING:
                    if let n = Self.anchorName(b.to, b.toI) { edit(n) { $0.scale *= CGFloat(s.scale) } }
                    if b.to == PK_ANC_FAN { edit("ring.\(b.toI)") { $0.ring = CGFloat(s.opacity) } }
                case PK_BK_POP:
                    edit("picker.\(part)") { $0.scale *= CGFloat(s.scale); $0.opacity *= CGFloat(s.opacity) }
                case PK_BK_COLLAPSE:
                    let name = "picker.\(part)"
                    var d = CGSize.zero
                    if let t = anchors[name], let c = anchors["stack"] {
                        d = CGSize(width: (c.midX - t.midX) * p, height: (c.midY - t.midY) * p)
                    }
                    edit(name) { $0.scale *= CGFloat(s.scale); $0.opacity *= CGFloat(s.opacity); $0.dx += d.width; $0.dy += d.height }
                default:
                    break
                }
            }
        }
        return fx
    }
}
