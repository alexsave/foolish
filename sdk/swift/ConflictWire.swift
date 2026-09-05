// ConflictWire.swift - the conflict model, asked of the kernel.
//
// When something has to leave a board that no move took off it - a staged move
// an arrival overrides, a sequence a newer arrival supersedes - the board
// REVERSES it before it plays anything else: the cards travel back the way they
// came, tinted red, and only then does the newest chain animate forward.
//
// The rule used to be Swift (ConflictModel.swift's conflictVerdict /
// conflictDest / ConflictFacts). It gives the same answers on any screen, so it
// is c/src/anim_plan.c's anim_conflict_* now and this is the crossing. Building
// the actual flights - rects, angles, the red tint - stayed behind, because
// that is rendering.
//
// THE FACTS ARE OPAQUE HERE ON PURPOSE. A `ConflictFacts` carries the kernel's
// INPUTS, not three sets Swift derived: "both sides of a battle stand" and "a
// masked back names nothing" then have exactly one implementation, and a second
// client cannot drift from it. What the facts SAY is only ever observed through
// a verdict.
//
// ONE CALL ANSWERS BOTH the per-motion verdicts and the reversal's shape, for
// the reason BeatWire's does: a board that asked for a motion's verdict and for
// the reversal's order separately could be told two different things about the
// same card.

import Foundation
import CFoolish

/// HOW THIS APP LEARNS THAT ITS OWN OPTIMISTIC CARD SURVIVED - said once, at
/// startup, because it is a property of the app and not of the question.
///
/// It is the ONLY thing the two clients' conflict rule disagrees about. The
/// CLEAR test, the standing test, the pool and masked-back rules and the
/// reversal's order are one implementation for both; the transport answers a
/// single question at the end of the verdict - once a card has failed both
/// tests, is "not accounted for" conclusive? (anim_plan.h.)
///
/// There is deliberately no default. A verdict asked before anyone declares
/// gets FIO_ETRANSPORT and this reader turns it into no plan at all, which is
/// loud, rather than quietly handing a new client iMessage's answer.
public enum AnimTransport: Int32, Sendable {
    /// iMessage: every message carries the whole game, totally ordered, so a
    /// newer chain is the complete truth and doom is knowable locally.
    case chain  = 1
    /// Everything through a server (the app's online play, a watch, Steam): a
    /// card's confirmation is its own later broadcast, so "the newest news does
    /// not mention my card" means the receipt is still in the post.
    case server = 2

    /// Say which one this process is. Call it from the app or extension entry
    /// point before any board is built; calling it twice with the same value is
    /// harmless, and the harness relies on that.
    public static func declare(_ t: AnimTransport) { _ = fio_set_transport(t.rawValue) }

    /// What is set, or nil if nothing has said. For diagnostics: a wrong-mode
    /// bug looks exactly like an animation bug and this is the cheap way to
    /// tell them apart.
    public static var current: AnimTransport? { AnimTransport(rawValue: fio_transport()) }
}

/// The three-way verdict. See anim_plan.h for the rule; the short of it is
/// whether the arriving chain accounts for the card being where the doomed
/// motion put it.
public enum ConflictVerdict: Equatable, Sendable {
    case revert   // nothing in the newest truth accounts for it - fly it back, red
    case keep     // the newest chain vouches for it where it is - do not move it
    case clear    // the newest chain's own replay animates it - hands off
}

/// Where a doomed motion PUT its card - the side of the arriving board the
/// standing check reads. `pool` is a destination with no persistent per-card
/// view (the discard pile, the deck, an opponent's badge): a card that went
/// into a pool is bookkeeping, and conjuring a ghost back OUT of a pile is how
/// the "deal from the pile onto the table" class of bug happens.
public enum ConflictDest: Equatable, Sendable {
    case table
    case myHand
    case pool

    var wire: UInt8 {
        switch self {
        case .table:  return UInt8(FIO_CONFLICT_DEST_TABLE)
        case .myHand: return UInt8(FIO_CONFLICT_DEST_MY_HAND)
        case .pool:   return UInt8(FIO_CONFLICT_DEST_POOL)
        }
    }

    /// Which kind of place an event's cards went (`fio_conflict_dest`). `kind`
    /// nil is a step this board cannot name, which moves nothing anywhere.
    public init(of kind: EventType?, seat: Int, mySeat: Int) {
        switch fio_conflict_dest(Int32(kind?.rawValue ?? -1), Int32(seat), Int32(mySeat)) {
        case Int32(FIO_CONFLICT_DEST_TABLE):   self = .table
        case Int32(FIO_CONFLICT_DEST_MY_HAND): self = .myHand
        default:                               self = .pool
        }
    }
}

/// One motion a doomed sequence made, as the rule sees it: the card and the
/// kind of place it landed. The flight it flew is the board's.
public struct ConflictMotion: Equatable, Sendable {
    public let card: Card?
    public let dest: ConflictDest
    public init(card: Card?, dest: ConflictDest) {
        self.card = card
        self.dest = dest
    }
}

/// What the arriving chain says, as the kernel takes it: the cards its stream
/// moves, the table of the board that stream opens on, and my hand there.
public struct ConflictFacts: Equatable, Sendable {
    /// The facts portion of `fio_conflict_packed`'s input. Opaque - see the
    /// file header for why the SETS are not mirrored here.
    let wire: [UInt8]

    /// Built from the same opening every arrival already carries.
    public init(events: [GameEvent], prior: GameView?) {
        self.init(moved: events.flatMap(\.cards),
                  openTable: prior?.battles ?? [],
                  myHand: prior?.me?.hand ?? [])
    }

    /// The same facts stated directly, for a caller that has the cards rather
    /// than the boards they came off.
    public init(moved: [Card?], openTable: [BattleView], myHand: [Card]) {
        var w: [UInt8] = []
        let ids = moved.prefix(255).map(Self.id)
        w.append(UInt8(ids.count))
        w.append(contentsOf: ids)
        let battles = openTable.prefix(Int(Self.none) - 1)
        w.append(UInt8(battles.count))
        for b in battles {
            w.append(Self.id(b.attack))
            w.append(Self.id(b.defense))
        }
        let hand = myHand.prefix(255).map(Self.id)
        w.append(UInt8(hand.count))
        w.append(contentsOf: hand)
        self.wire = w
    }

    /// The arriving chain could not be read (a peek that failed to decode).
    /// Nothing named, nothing standing - which reverts everything, the honest
    /// default: a chain nobody can read vouches for nothing.
    public static let unknown = ConflictFacts(moved: [], openTable: [], myHand: [])

    /// The verdict for one card at one destination.
    public func verdict(_ card: Card?, dest: ConflictDest) -> ConflictVerdict {
        ConflictPlan([[ConflictMotion(card: card, dest: dest)]], facts: self)
            .verdicts.first ?? .keep
    }

    static let none = UInt8(FIO_CONFLICT_NONE)
    /// A card as a dense id, or the "names nothing" byte. A masked back and a
    /// card outside the deck are the same case: neither can be conflicted on.
    static func id(_ c: Card?) -> UInt8 {
        guard let c, !c.isHidden, (0...3).contains(c.s), (1...13).contains(c.v)
        else { return none }
        return UInt8(c.s * 13 + (c.v - 1))
    }
}

/// The kernel's answer for a whole superseded sequence: what each motion's
/// verdict is, and which of them fly back in which order.
public struct ConflictPlan: Equatable, Sendable {
    /// One per motion, in the order they were handed over.
    public let verdicts: [ConflictVerdict]
    /// The reversal: motion indices, grouped into parallel steps, LAST flown
    /// group first. A group the verdicts emptied is dropped rather than played
    /// as a beat of silence.
    public let steps: [[Int]]

    public static let empty = ConflictPlan(verdicts: [], steps: [])

    /// Ask the kernel about a superseded sequence's motions, grouped as it flew
    /// them (one group per parallel step).
    public init(_ groups: [[ConflictMotion]], facts: ConflictFacts) {
        self = Self.ask(groups, facts) ?? .empty
    }

    private static func ask(_ groups: [[ConflictMotion]], _ facts: ConflictFacts) -> ConflictPlan? {
        let motions = groups.flatMap { $0 }
        // A debt past the kernel's cap is refused rather than half-reversed; no
        // real sequence comes near it (the same degrade-to-less-animation
        // discipline BeatWire and PreBoutTable keep over the same streams).
        guard groups.count <= 128, motions.count <= 255,
              groups.allSatisfy({ $0.count <= 255 }) else { return nil }

        var input: [UInt8] = [UInt8(FIO_CONFLICT_VERSION)]
        input.append(contentsOf: facts.wire)
        input.append(UInt8(groups.count))
        for g in groups { input.append(UInt8(g.count)) }
        for m in motions {
            input.append(ConflictFacts.id(m.card))
            input.append(m.dest.wire)
        }

        var out = [CChar](repeating: 0, count: 3 + 3 * motions.count + groups.count)
        let n: Int32 = input.withUnsafeBufferPointer { p in
            fio_conflict_packed(p.baseAddress, Int32(input.count), &out, Int32(out.count))
        }
        guard n >= 2 else { return nil }
        let b = out.prefix(Int(n)).map { UInt8(bitPattern: $0) }

        var at = 1
        let nVerdicts = Int(b[at]); at += 1
        guard b.count >= at + nVerdicts + 1 else { return nil }
        let verdicts: [ConflictVerdict] = b[at..<(at + nVerdicts)].map {
            switch Int32($0) {
            case Int32(FIO_CONFLICT_V_REVERT): return .revert
            case Int32(FIO_CONFLICT_V_CLEAR):  return .clear
            default:                           return .keep
            }
        }
        at += nVerdicts
        let nSteps = Int(b[at]); at += 1
        guard b.count >= at + nSteps else { return nil }
        let sizes = b[at..<(at + nSteps)].map { Int($0) }
        at += nSteps
        var steps: [[Int]] = []
        steps.reserveCapacity(nSteps)
        for size in sizes {
            guard b.count >= at + size else { return nil }
            steps.append(b[at..<(at + size)].map { Int($0) })
            at += size
        }
        return ConflictPlan(verdicts: verdicts, steps: steps)
    }

    private init(verdicts: [ConflictVerdict], steps: [[Int]]) {
        self.verdicts = verdicts
        self.steps = steps
    }
}
