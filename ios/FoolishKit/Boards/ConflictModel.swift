// ConflictModel.swift - THE CONFLICT MODEL (docs/ANIMATION_CATALOGUE.md,
// decided 1.0(28)): when something has to leave the board that a move did not
// take off it - a staged move an arrival overrides, a sequence a newer arrival
// supersedes - the board REVERSES it before it plays anything else. The cards
// travel back the way they came, tinted red, and only when the board is
// standing at a state the newest chain vouches for does that chain animate
// forward. Never a cut, never a snap.
//
// THE DECISION IS THE WEB'S, RE-GROUNDED ON PHONE INPUTS. The web hardened the
// same problem over months of glitch-fixing and its verdict lives in the shared
// animation core (c/src/anim_plan.h, anim_resolve_unconfirmed_attack_covers):
// per pending card, REVERT (fly it back, it was never accepted), MERGE (keep
// it, its confirmation is still coming), CLEAR (it WAS accepted and this same
// broadcast sweeps it off the table, so it must NOT fly home first - reverting
// those is the "I put a card down, someone picked it up, and it flew back to my
// hand" flicker).
//
// WHY THIS IS SWIFT AND NOT A CALL INTO THAT C FUNCTION - considered, and
// decided against, with the owner's standing "when in doubt, move logic to C"
// on the table:
//
//   * The C entry point decides against a SERVER VERDICT: an authoritative
//     table, a defender-capacity inference, a broadcast that may yet confirm
//     the pending card. None of those inputs exist here. An iMessage conflict
//     has exactly two sources - an arrival that overrides my staged move, and
//     an arrival that supersedes a sequence still in flight - and in both the
//     whole "verdict" is that a newer chain exists. Calling the C function
//     would mean fabricating a fake server state for it to judge, which pins
//     nothing: the web could change its capacity rule tomorrow and this board
//     would silently change how it retracts a staged card.
//   * What genuinely carries over is the TRICHOTOMY (revert / keep / clear),
//     the ORDERING (reverse fully, then play), and the RED - vocabulary and
//     choreography, not computation. The computation here is set membership
//     against the arriving chain's own opening board and stream, inputs only
//     this platform has.
//   * A new C entry with phone-shaped inputs would have exactly one caller on
//     exactly one platform today - there is no second chain-based client yet to
//     keep from drifting.
//
// LIFT THIS TO C - a standing intent, not a vague TODO (owner, on accepting
// this file: "definitely add a comment to lift to C later. Because once I get
// this in a good state, a lot of code will be moved out of swift and to C").
// This file is the SPEC to lift, and the trigger is "once this is in a good
// state": the verdicts and the reversal shapes confirmed on a real table, the
// rig oracles quiet, the owner done adjusting the choreography. Until then it
// stays here, where it can still be changed in one place per walk.
//
// AND THE DIRECTION OF TRAVEL IS INVERTED from what anim_plan.h's header says.
// That header's premise - "the WEB is the spec; iOS re-derived the same
// choreography a second time" - is no longer the owner's position: "the
// imessage behavior and layout and animation is slightly different from the
// webs, and I prefer the imessage version." So when this moves to C it does
// NOT get reconciled with anim_resolve_unconfirmed_attack_covers - it REPLACES
// that rule as the shared one, and the WEB becomes the client that re-derives.
// Do not "fix" this file to conform to the C function; that would be exactly
// backwards.
//
// OPEN QUESTION, deferred by the owner and explicitly not for this round: what
// inputs the future C entry takes. Their words on the fake-server-state
// objection above: "so what we fabricate a fake server state? Maybe we just
// fabricate exactly what we need for this. But later." So the likely shape is
// a C entry that takes exactly the inputs THIS decision needs (the arriving
// stream's card set and its opening board's standing sets), not a fabricated
// copy of the web's server state - but that is recorded here as the open
// question it is, not built.
//
// HOW EACH VERDICT READS ON THE PHONE. "Accepted" here means "the arriving
// chain - the thread's truth - stands behind where the superseded motion put
// the card":
//
//   REVERT - nothing in the newest truth accounts for the card being where the
//     doomed motion put it. My staged, unsent cards are the canonical case (no
//     other device ever saw them), and so is anything a fork winner un-happens.
//     These fly back the way they came, tinted red.
//   KEEP - the card stands at its post spot on the arriving chain's own opening
//     board (the state its replay animates FROM), and the replay does not move
//     it. This is MERGE's twin: the card's "confirmation" is the newest chain
//     itself. Flying it home only for the incoming seed to snap it straight
//     back would be the clear-flicker one board later, so it does not move.
//     A burst of arrivals that each EXTEND the animating chain lands here for
//     almost every card, which is why "undo whatever is animating" (the owner's
//     burst rule) mostly resolves to "stop it and let the newest chain play" -
//     the reversal theatre is reserved for motion the truth disowns.
//   CLEAR - the arriving chain's own stream moves the card (the arriving player
//     covered it or picked it up, or the arriving chain is my own sent move
//     coming back). The forward replay animates it; a red flight first would be
//     the web's pickup flicker under a new name. This is the verdict the
//     catalogue says to get right first.
//
// Pure and static in the house style (`goodsOpening`, `outsWith`, `holdsAfter`,
// `parallelGroups`): every rule here can be asserted without a board.

import SwiftUI

/// The three-way verdict, named after the web's (`AttackCoverResolution` in
/// src/state/optimisticConflicts.ts / AnimResolve in c/src/anim_plan.h).
public enum ConflictVerdict: Equatable {
    case revert   // fly it back the way it came, tinted red
    case keep     // the newest chain vouches for it where it is - do not move it
    case clear    // the newest chain's own replay animates it - hands off
}

/// Where a doomed motion PUT its card - the side of the flight the verdict
/// checks against the arriving chain's opening board. `pool` is a destination
/// with no persistent per-card view (the discard pile, the deck, an opponent's
/// badge): a card that went into a pool is bookkeeping, and conjuring a ghost
/// back OUT of a pile to un-book it is how the "deal from the pile onto the
/// table" class of bugs happens, so pools are never reversed.
public enum ConflictDest: Equatable {
    case table
    case myHand
    case pool
}

/// What the arriving chain says, reduced to the three sets the verdict reads.
/// Built from the same opening every arrival already carries: the stream it
/// will animate (`openReplayEvents`) and the board that stream opens on
/// (`openReplayPriorState`).
public struct ConflictFacts: Equatable {
    /// Card identities the arriving replay itself moves (its events' non-nil
    /// cards). Masked cards cannot be named here - they are opponents' hand
    /// draws, which can never collide with a card this board has shown.
    public let incomingMoved: Set<String>
    /// Identities standing on the arriving replay's OPENING table.
    public let tableAtOpen: Set<String>
    /// Identities in MY hand on that opening board.
    public let myHandAtOpen: Set<String>

    public init(incomingMoved: Set<String>, tableAtOpen: Set<String>,
                myHandAtOpen: Set<String>) {
        self.incomingMoved = incomingMoved
        self.tableAtOpen = tableAtOpen
        self.myHandAtOpen = myHandAtOpen
    }

    public init(events: [GameEvent], prior: GameView?) {
        var moved = Set<String>()
        for ev in events {
            for case let c? in ev.cards { moved.insert(c.identity) }
        }
        var table = Set<String>()
        for b in prior?.battles ?? [] {
            table.insert(b.attack.identity)
            if let d = b.defense { table.insert(d.identity) }
        }
        self.init(incomingMoved: moved,
                  tableAtOpen: table,
                  myHandAtOpen: Set((prior?.me?.hand ?? []).map(\.identity)))
    }

    /// The arriving chain could not be read (a peek that failed to decode).
    /// Empty sets make everything REVERT, which is the honest default: a chain
    /// we cannot read vouches for nothing.
    public static let unknown = ConflictFacts(incomingMoved: [], tableAtOpen: [],
                                              myHandAtOpen: [])
}

/// One motion a doomed sequence actually made: the flight it flew and the kind
/// of place it put the card. Recorded by `runEventStream` as each group lands,
/// deposited by a superseded sequence's teardown, consumed by the conflict
/// reversal.
public struct FlownMotion: Equatable {
    public let flight: Flight
    public let dest: ConflictDest
    public init(flight: Flight, dest: ConflictDest) {
        self.flight = flight
        self.dest = dest
    }
}

extension MessageTableView {

    /// THE VERDICT, per card. `id` nil is a masked back - an opponent's draw
    /// has no identity to conflict on and no persistent view to fly back from
    /// (it landed INTO a badge), so it is kept: the newest sequence's count
    /// freeze owns that badge from here.
    ///
    /// Precedence matters and is the whole web lesson: CLEAR is checked before
    /// the standing sets, because a card the incoming replay moves may well
    /// also stand on its opening table (a pickup's cards do, by definition) -
    /// and the replay taking it off the table is the animation; a red flight
    /// first is the flicker.
    public static func conflictVerdict(id: String?, dest: ConflictDest,
                                       facts: ConflictFacts) -> ConflictVerdict {
        guard let id else { return .keep }
        if facts.incomingMoved.contains(id) { return .clear }
        switch dest {
        case .pool:   return .keep
        case .table:  return facts.tableAtOpen.contains(id) ? .keep : .revert
        case .myHand: return facts.myHandAtOpen.contains(id) ? .keep : .revert
        }
    }

    /// Which kind of place an event's cards went, for the verdict above. The
    /// mapping is the board's own flight-building one (`openReplayFlights`):
    /// placements land on the table; my own draws and pickups land in my hand;
    /// everything else - an opponent's draw or pickup, a discard sweep, the
    /// no-flight notices - lands in a pool.
    public static func conflictDest(of kind: EventType?, seat: Int,
                                    mySeat: Int) -> ConflictDest {
        switch kind {
        case .attackPass, .cover, .defenderMove:
            return .table
        case .deal, .refill, .pickup:
            return seat == mySeat ? .myHand : .pool
        default:
            return .pool
        }
    }

    /// THE REVERSAL, as flights: every REVERT-verdict motion of the doomed
    /// sequence flipped end for end, in REVERSE group order - the cards travel
    /// back the way they came, last motion first, each group as one parallel
    /// step (the same composition rule the forward direction uses). KEEP and
    /// CLEAR motions build nothing; groups left empty by the verdicts are
    /// dropped rather than played as beats of silence.
    ///
    /// The flipped flight starts from where the motion ENDED (its `to`, where
    /// the card is now resting) and lands at its source; the tilt runs
    /// backwards the same way. `revert: true` is what draws it red - the tint
    /// is on the flight ghost only, so a card that lands back in a hand is a
    /// normal card again the moment it lands.
    public static func reversalSteps(debt: [[FlownMotion]],
                                     facts: ConflictFacts) -> [[Flight]] {
        var steps: [[Flight]] = []
        for group in debt.reversed() {
            let flights = group.compactMap { m -> Flight? in
                let f = m.flight
                guard conflictVerdict(id: f.card?.identity, dest: m.dest,
                                      facts: facts) == .revert else { return nil }
                return Flight(id: "revert-\(f.id)", card: f.card,
                              from: f.to, to: f.from,
                              angle: f.fromAngle, fromAngle: f.angle,
                              revert: true)
            }
            if !flights.isEmpty { steps.append(flights) }
        }
        return steps
    }
}
