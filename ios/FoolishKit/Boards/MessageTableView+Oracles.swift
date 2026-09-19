// WHAT THE BOARD SAYS ABOUT ITSELF - the traces, and the levels a rig scenario
// reads off them once everything has settled.
//
// Every one of these exists because a defect here is a DISAGREEMENT between
// what the kernel holds and what the screen shows, and only the board knows
// which of its three sources it used. The lag is the feature; what these count
// is a board that still disagrees with NOTHING pending to put it right, which
// is the shape of every "it seems to be a bit behind" report.
//
// LEVELS, NOT COUNTS, wherever the defect is a state that outlives a moment -
// see `sweepVisibleNow` and `veilStandingNow`, which both had to stop being
// tallies before they could mean anything.

import SwiftUI
import Foundation

extension MessageTableView {

    #if DEBUG
    private static var lastGridTrace = ""
    static func traceGrid(sweeping: Bool, shown: [BattleView], hidden: Set<String>,
                          atRest: Bool = false, preHidden: Set<String> = [],
                          veil: Set<String> = [], flyingNow: Set<String> = [],
                          arriving: Set<String> = []) {
        // THE VEIL STILL STANDING, read off the same `preHidden` this line is
        // already handed. It used to be assigned at the call site, one line
        // above the call - which only held while the caller and the counter
        // were in the same file, and it says nothing the trace does not already
        // have in its hand. Same value, same paint, one owner.
        veilStandingNow = preHidden.count
        let pairs = shown.filter { $0.defense != nil }.count
        let visible = shown.reduce(0) { n, b in
            n + (hidden.contains(b.attack.identity) ? 0 : 1)
              + ((b.defense.map { hidden.contains($0.identity) ? 0 : 1 }) ?? 0)
        }
        // A CARD ON THE TABLE THAT IS NOT DRAWN, with nothing animating: the
        // owner's "it animates that card moving, but it just doesn't land on
        // the table - the card just vanishes". Veiling is legitimate mid-flight
        // and never at rest, exactly like the count lag.
        let veiled = shown.reduce(0) { n, b in
            n + (hidden.contains(b.attack.identity) ? 1 : 0)
              + ((b.defense.map { hidden.contains($0.identity) ? 1 : 0 }) ?? 0)
        }
        // AT REST means more than "no sequence running": a board that has a
        // replay pending but has not started it yet is legitimately holding the
        // whole move back (`pendingOpen`), and every FIRST paint looks like that.
        if atRest && veiled > 0 {
            vanishedAtRest += 1
            let stuck = shown.flatMap { b in
                [b.attack.identity, b.defense?.identity].compactMap { $0 }
            }.filter { hidden.contains($0) }
            // ROUND 22, the distinction that makes this number readable. A
            // PRE-HIDDEN card is one held back so it can fly in rather than pop
            // in - the flight is already scheduled, and the grid trace right
            // after shows `visible` climbing as each lands. Every arrival in the
            // rig produces one or two of those, on every build back to 1.0(22),
            // so a raw count of 1-2 is the floor rather than a finding, and a
            // real defect would have been lost in it.
            //
            // STRANDED is the defect: hidden with nothing scheduled to reveal
            // it - the "it animates that card moving, but it just doesn't land
            // on the table" this counter was added for, and bug #11's
            // `visible=0 hidden=1` with no flight after it. This one must be 0.
            let stranded = stuck.filter { !preHidden.contains($0) }
            if !stranded.isEmpty { strandedAtRest += 1 }
            AnimLog.say("VANISHED [\(stuck.sorted().joined(separator: ","))] "
                + "preHidden=\(stuck.filter { preHidden.contains($0) }.count) "
                + "veilOnly=\(stuck.filter { veil.contains($0) && !preHidden.contains($0) }.count) "
                + "STRANDED=\(stranded.count)")
        }
        // THE PRE-BOUT GRID, as a LEVEL rather than a count.
        //
        // A counter cannot answer this one. Laying the sweep out is synchronous
        // (`playBoutEnd` sets it before it starts a Task) while the sequence
        // counter that says "something is animating" is raised inside that
        // Task, so there is always a paint where the grid is up, fully visible,
        // and nothing yet claims to be running - filmed at log 43 of a goodend
        // run, four cards visible, with the stream beginning at 47. Counting
        // paints reports that as a defect on a board that is working, which is
        // the same trap `staleAtRest` and `vanishedAtRest` both fell into.
        //
        // What IS a defect is a grid still standing once everything has
        // settled, seconds later: `view.battles` is empty by then, so every
        // oracle comparing the controller's view against the kernel reads CLEAN
        // while the screen shows a table that is not there. Owner, 1.0(24): a
        // round-ending good after Send "caused all the right animations to
        // play, BUT the attack card that was covered and the card that covered
        // it remained on the table". So this is the LAST KNOWN state, for a
        // scenario to read at rest, not a tally of moments.
        sweepVisibleNow = sweeping ? visible : 0
        // THE TILT, as the grid itself will compute it this paint. A covered
        // attack lies across only once its cover is flying or landed, so on the
        // FIRST paint of an open that is about to replay a cover this must be 0:
        // a pair that opens tilted has to straighten before the cover flies, and
        // that straighten-then-tilt is the flash the veil exists to prevent.
        // Read off `FBattleGrid.coverTilted` rather than restated, so the line
        // cannot drift from what is drawn.
        let tiltedNow = Set(shown.compactMap { b -> String? in
            FBattleGrid.coverTilted(defense: b.defense, hidden: hidden, flyingNow: flyingNow)
                ? b.defense?.identity : nil
        })
        let tilted = tiltedNow.count
        // THE FLASH, AS A NUMBER. A pair that lay across on one paint and is
        // upright on the next, with its cover still to ARRIVE, is the defect
        // itself: the board drew the cover already landed, then had to take the
        // tilt back before flying it in. Owner, 1.0(47): "I briefly saw the
        // uncovered cards animate from rotated back to straight, then rotate
        // back as my cover cards flew to cover."
        //
        // The `arriving` term is what keeps a SWEEP honest - a pair untilts
        // legitimately as it is carried off to the discard, and that cover is
        // leaving, not arriving. Must be 0 for a whole run.
        for id in Self.lastTilted.subtracting(tiltedNow) where arriving.contains(id) {
            tiltFlashes += 1
            AnimLog.say("TILT-FLASH \(id) straightened with its cover still to arrive")
        }
        Self.lastTilted = tiltedNow
        let line = "grid sweeping=\(sweeping) cells=\(shown.count) pairs=\(pairs) visible=\(visible) hidden=\(hidden.count) tilt=\(tilted)"
            + (atRest && veiled > 0 ? "  <-- \(veiled) VANISHED AT REST" : "")
        if line != lastGridTrace { lastGridTrace = line; AnimLog.say(line) }
    }

    /// How many times a paint has shown a count that disagreed with the kernel
    /// WHILE NOTHING WAS ANIMATING.
    ///
    /// Mid-sequence disagreement is the whole point of the overrides - the board
    /// deliberately lags so a badge does not jump to its final value before the
    /// cards that earn it have flown. AT REST there is no such licence: the
    /// board is simply wrong, and the human has to close the bubble and reopen
    /// it. That is the invariant, and it is the one the arrival rig checks.
    public private(set) static var staleAtRest = 0
    public static func resetStaleAtRest() { staleAtRest = 0 }

    /// How many times a displayed count moved AWAY from a truth that had not
    /// changed - the board going backwards.
    ///
    /// The lag is allowed to trail the kernel; that is what it is for. Within
    /// one unchanged truth it may only ever CONVERGE on it, because the states
    /// it walks are the steps of the move that produced that truth. A shown
    /// value that retreats is a stream writing a board older than the one on
    /// screen, which is the defect itself - filmed as the deck badge going
    /// 9 -> 12 -> 9 with nothing about the deck happening.
    public private(set) static var backwardsPaints = 0
    /// Pairs that lay across and then straightened while their cover had still
    /// not arrived - the tilt flash, counted rather than eyeballed. Must be 0.
    public private(set) static var tiltFlashes = 0
    /// The defenses drawn tilted on the previous paint, which is the only thing
    /// a flash can be measured against.
    static var lastTilted: Set<String> = []
    /// Cards sitting on the table that the board is not drawing, at rest -
    /// INCLUDING the ones pre-hidden for a flight that is about to land, which
    /// is most of them. See the note at the counter for why that matters.
    public private(set) static var vanishedAtRest = 0
    /// The half of `vanishedAtRest` that is a real defect: a card hidden with
    /// nothing scheduled to reveal it. This one must be zero.
    public private(set) static var strandedAtRest = 0
    /// How many cards are VEILED RIGHT NOW and reserving no fan slot - i.e.
    /// `animator.preHidden`, the set `handSlotDeferred` keeps out of the hand.
    ///
    /// ROUND 40's invariant, as a level the rig can read once everything has
    /// settled (owner: "when no sequence is running, nothing is pre-hidden and
    /// nothing is deferred - the hand lays out every card it holds"). Non-zero
    /// seconds after the last move means a veil nothing will ever take down:
    /// the fan is CENTRED, so a hand permanently laying out 4 of its 7 cards is
    /// drawn at the wrong width and the wrong centre, and every later veil
    /// change re-lays the whole row from that wrong baseline - the owner's
    /// "still seeing the hand card rows twitch during the discard animation".
    ///
    /// A LEVEL, not a count, for the reason `sweepVisibleNow` spells out: the
    /// defect is a veil still standing at rest, not a moment that had one.
    public private(set) static var veilStandingNow = 0
    /// How many cards the pre-bout sweep grid is drawing RIGHT NOW (0 when it
    /// is not sweeping). Read at rest by the harness oracle: nonzero once
    /// everything has settled means phantom cards on a table the game says is
    /// empty. A level, not a count - see `traceGrid` for why.
    public private(set) static var sweepVisibleNow = 0
    private static var lastShown: Int?
    private static var lastTruth: Int?

    private static var lastCountTrace = ""
    static func traceCount(shown: Int, override: Int?, veil: Int?, truth: Int) {
        // AT REST means nothing is pending, and a sequence counter alone does
        // not say that. An OVERRIDE or a VEIL is the board declaring that it is
        // deliberately holding this badge until the cards that earn it have
        // flown - and both go up a beat BEFORE the Task that raises
        // `sequenceDepth` runs. Counting that window made every bout-ending
        // arrival report two stale paints on every build back to 1.0(22)
        // (filmed: `shown=23 veil=23 truth=19`, then `override=23`, then the
        // same numbers again reading "lagging" once the depth caught up), which
        // is a floor high enough to hide a real one. The lag is the feature;
        // what this counter is for is a board that disagrees with the kernel
        // with NOTHING pending to put it right.
        let atRest = !BoardAnimator.isSequencing && override == nil && veil == nil
        if atRest && shown != truth { staleAtRest += 1 }
        if let ls = lastShown, let lt = lastTruth, lt == truth, shown != ls,
           abs(shown - truth) > abs(ls - truth) {
            backwardsPaints += 1
            AnimLog.say("deck WENT BACKWARDS \(ls) -> \(shown) with truth \(truth)")
        }
        lastShown = shown; lastTruth = truth
        let line = "deck shown=\(shown) override=\(override.map(String.init) ?? "-")"
            + " veil=\(veil.map(String.init) ?? "-") truth=\(truth)"
            + (shown == truth ? "" : atRest ? "  <-- STALE AT REST" : "  <-- lagging (animating)")
        if line != lastCountTrace { lastCountTrace = line; AnimLog.say(line) }
    }

    private static var lastMarkTrace: [Int: String] = [:]
    static func traceMark(seat: Int, defender: Bool, attacker: Bool, good: Bool,
                          out: Bool, flying: Bool, roles: RoleState,
                          battles: Int, sweep: Int) {
        let mark = good ? "check" : defender ? "shield" : attacker ? "sword" : "-"
        let line = "mark s\(seat)=\(mark)\(flying ? " (flying)" : "")"
            + " [def=\(defender) atk=\(attacker) good=\(good) out=\(out)]"
            + " roles=d\(roles.defender) fa\(roles.firstAttacker) g\(roles.goodMask)"
            + " battles=\(battles) sweep=\(sweep)"
        if line != lastMarkTrace[seat] { lastMarkTrace[seat] = line; AnimLog.say(line) }
    }
    #endif
}
