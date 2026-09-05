// ConflictModel.swift - the RENDERING half of the conflict model
// (docs/ANIMATION_CATALOGUE.md, decided 1.0(28)): when something has to leave
// the board that a move did not take off it - a staged move an arrival
// overrides, a sequence a newer arrival supersedes - the board REVERSES it
// before it plays anything else. The cards travel back the way they came,
// tinted red, and only when the board is standing at a state the newest chain
// vouches for does that chain animate forward. Never a cut, never a snap.
//
// THE DECISION IS THE KERNEL'S NOW. The trichotomy (revert / keep / clear), the
// destination mapping, the facts an arriving chain vouches with, and the order
// the reversal plays in are all c/src/anim_plan.c's anim_conflict_*, reached
// through sdk/swift/ConflictWire.swift. This file is what is left when that
// leaves: turning the kernel's answer into actual `Flight`s, which is rects and
// angles and the red tint - rendering, and irreducibly per-platform.
//
// The lift was the standing intent this file was written with (owner, on
// accepting it: "definitely add a comment to lift to C later"), and the
// direction it names held: the iMessage rule went to C as the shared one rather
// than being reconciled with anim_resolve_unconfirmed_attack_covers, which
// answers a different question against a server verdict and stays for the web.
// See anim_plan.h for what separates them.

import SwiftUI

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

    /// THE REVERSAL, as flights. The kernel says WHICH motions fly back and in
    /// what order (`ConflictPlan` - reverse group order, empty groups dropped);
    /// this flips each one end for end.
    ///
    /// The flipped flight starts from where the motion ENDED (its `to`, where
    /// the card is now resting) and lands at its source; the tilt runs
    /// backwards the same way. `revert: true` is what draws it red - the tint
    /// is on the flight ghost only, so a card that lands back in a hand is a
    /// normal card again the moment it lands.
    public static func reversalSteps(debt: [[FlownMotion]],
                                     facts: ConflictFacts) -> [[Flight]] {
        let flat = debt.flatMap { $0 }
        let plan = ConflictPlan(debt.map { group in
            group.map { ConflictMotion(card: $0.flight.card, dest: $0.dest) }
        }, facts: facts)
        return plan.steps.map { step in
            step.map { i in
                let f = flat[i].flight
                return Flight(id: "revert-\(f.id)", card: f.card,
                              from: f.to, to: f.from,
                              angle: f.fromAngle, fromAngle: f.angle,
                              revert: true)
            }
        }
    }
}
