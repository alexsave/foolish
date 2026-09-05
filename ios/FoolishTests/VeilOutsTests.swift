// WHAT THE VEIL ANSWERS - the board's outs, pinned.
//
// The board's "how is this card drawn right now" state is nine pieces of
// `@State` (`animator.hidden`/`preHidden`, `sweepBattles`, `sweepTableIds`,
// `sweptFlownIds`, `sweepUnplaced`, `sweepArriving`, `handHoldback` +
// `handHoldbackAt`, `orphanedOpens`), and until round 45 every one of them was
// read straight out of `body`. Nothing downstream could be exercised at all:
// the four sets the board actually HANDS OUT - the hand fan's `hidden`, the fan
// slot it withholds, and the battle grid's `hidden` and `flyingNow` - existed
// only as expressions inside a SwiftUI view.
//
// That is not a theoretical gap. Item 12 of this same cleanup mutated a
// comparable condition (`unstartedReplay` ignoring `replayPending`, which makes
// the veil NEVER LIFT) and all 539 tests passed. So these are CHARACTERIZATION
// tests in the strict sense: they were written by reading what the code answers
// TODAY, over a matrix that includes the combinations nobody would design for,
// and they are the oracle the collapse in this same round was measured against.
// Where a case looks wrong, it is recorded rather than corrected, and said so.
//
// MUTATION-CHECKED - see the per-test notes for the exact mutant each catches.

import XCTest
@testable import FoolishKit

final class VeilOutsTests: XCTestCase {

    private func c(_ s: Int, _ v: Int) -> Card { Card(s: s, v: v) }
    private var six: Card { c(0, 6) }
    private var nine: Card { c(1, 9) }
    private var queen: Card { c(2, 12) }

    private func source(_ path: String) throws -> String {
        let here = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
        return try String(contentsOf: here.deletingLastPathComponent()
            .appendingPathComponent(path), encoding: .utf8)
    }

    // MARK: 1 - `Veil.veiled`: three sources, unioned

    /// The ordinary board. Nothing animating, no replay outstanding, no move of
    /// mine in flight - and therefore nothing veiled. This is the state the
    /// board spends almost all of its life in, so it is the one that has to be
    /// provably free of every source.
    func testARestingBoardVeilsNothing() {
        XCTAssertTrue(Veil.veiled(hidden: [], pendingOpen: nil,
                                  handBeforeMyMove: nil, myHand: nil).isEmpty)
        XCTAssertTrue(Veil.veiled(hidden: [], pendingOpen: nil,
                                  handBeforeMyMove: nil,
                                  myHand: [six, nine, queen]).isEmpty,
                      "a hand alone veils nothing - it takes a move to do that")
    }

    /// The animator's own set passes through untouched. Everything else this
    /// function does is additive on top of it.
    ///
    /// MUTANT: start `ids` from the empty set instead of `hidden` and this
    /// fails - and on a device that is every in-flight card painted twice, once
    /// as a ghost and once in place.
    func testTheAnimatorsHiddenSetPassesThrough() {
        let out = Veil.veiled(hidden: [six.identity], pendingOpen: nil,
                              handBeforeMyMove: nil, myHand: nil)
        XCTAssertEqual(out, [six.identity])
    }

    /// AN UNSTARTED REPLAY VEILS ITS OWN CARDS, before the animator has been
    /// told anything. This is the first paint of an arriving bubble: the view is
    /// published, `replayPending` is up, and `flyBoutEndToDiscard` has not run
    /// yet. Without it the cards show already-landed for a paint and then
    /// animate - the "starts already landed" family.
    ///
    /// MUTANT: drop the `pendingOpen` union and this fails.
    func testAnUnstartedReplayVeilsItsTouchedCards() {
        let out = Veil.veiled(hidden: [], pendingOpen: [nine.identity],
                              handBeforeMyMove: nil, myHand: nil)
        XCTAssertEqual(out, [nine.identity])
    }

    /// NIL AND EMPTY ARE THE SAME ANSWER HERE, deliberately recorded: a replay
    /// that touches no cards veils nothing, exactly like no replay at all. The
    /// nil-ness is the CONTROLLER's business (`unstartedReplay`), not this
    /// function's - which is why the counts half of `pendingOpen` is not an
    /// input to the veil at all.
    func testAReplayThatTouchesNoCardsVeilsNothing() {
        let empty = Veil.veiled(hidden: [six.identity], pendingOpen: [],
                                handBeforeMyMove: nil, myHand: nil)
        let none = Veil.veiled(hidden: [six.identity], pendingOpen: nil,
                               handBeforeMyMove: nil, myHand: nil)
        XCTAssertEqual(empty, none)
    }

    /// LIVE PLAY: a card this move just put in MY hand is veiled until the
    /// animator takes over. `handBeforeMyMove` is the hand captured
    /// SYNCHRONOUSLY in `play`, so anything in the hand now and not in there is
    /// a card this move gave me - a pickup's table cards, a bout-end refill.
    ///
    /// MUTANT: union the whole hand rather than the difference and this fails -
    /// on a device, my entire hand vanishes the instant I take.
    func testACardThisMoveGaveMeIsVeiled() {
        let out = Veil.veiled(hidden: [], pendingOpen: nil,
                              handBeforeMyMove: [six.identity],
                              myHand: [six, nine])
        XCTAssertEqual(out, [nine.identity], "only the card that was not there before")
    }

    /// THE THREE SOURCES ARE A UNION, NEVER A CHOICE. All three up at once (an
    /// arrival landing on a board that is already animating a move of mine) and
    /// every id survives - the handoff between them is invisible precisely
    /// because nothing arbitrates.
    func testAllThreeSourcesUnionRatherThanOverride() {
        let out = Veil.veiled(hidden: [six.identity],
                              pendingOpen: [nine.identity],
                              handBeforeMyMove: [],
                              myHand: [queen])
        XCTAssertEqual(out, [six.identity, nine.identity, queen.identity])
    }

    /// …AND NOTHING EVER SUBTRACTS. A card that is in `hidden` AND was in my
    /// hand before the move stays veiled: the pre-move hand is only ever
    /// consulted to ADD. Recorded because the obvious misreading of
    /// `subtracting(before)` is that the pre-move hand un-veils, and it does not.
    ///
    /// MUTANT: `ids.subtract(before)` anywhere in this function and it fails.
    func testThePreMoveHandOnlyEverAdds() {
        let out = Veil.veiled(hidden: [six.identity], pendingOpen: nil,
                              handBeforeMyMove: [six.identity, nine.identity],
                              myHand: [six])
        XCTAssertEqual(out, [six.identity],
                       "a card already hidden for its flight stays hidden")
    }

    /// A pre-move hand that has since LOST cards (I attacked) adds nothing at
    /// all - the difference is taken in one direction only.
    func testCardsThatLeftMyHandAreNotVeiled() {
        let out = Veil.veiled(hidden: [], pendingOpen: nil,
                              handBeforeMyMove: [six.identity, nine.identity,
                                                 queen.identity],
                              myHand: [six])
        XCTAssertTrue(out.isEmpty)
    }

    /// AN EMPTY PRE-MOVE HAND VEILS THE WHOLE HAND. Weird-looking and correct:
    /// `handBeforeMyMove` is `nil` when no move of mine is in flight, so an
    /// EMPTY set is the genuine "I held nothing and this move gave me cards"
    /// - the last pickup of a game played out of an empty hand. Pinned because
    /// `nil` and `[]` are the two states of one optional and they mean opposite
    /// things here.
    func testAnEmptyPreMoveHandIsNotTheSameAsNoMove() {
        let all = Veil.veiled(hidden: [], pendingOpen: nil,
                              handBeforeMyMove: [], myHand: [six, nine])
        XCTAssertEqual(all, [six.identity, nine.identity])
        let none = Veil.veiled(hidden: [], pendingOpen: nil,
                               handBeforeMyMove: nil, myHand: [six, nine])
        XCTAssertTrue(none.isEmpty)
    }

    /// NO HAND, NO LIVE-PLAY VEIL - even with a move of mine outstanding. That
    /// is a spectator (`me` is nil, there is no fan) or a board whose view has
    /// not published yet. Recorded as a characterization, not endorsed: it is
    /// the guard `let hand = controller.view?.me?.hand` in the accessor, and
    /// a spectator has no move of their own to be mid-flight anyway.
    func testWithNoHandTheLivePlaySourceIsSilent() {
        let out = Veil.veiled(hidden: [six.identity], pendingOpen: nil,
                              handBeforeMyMove: [], myHand: nil)
        XCTAssertEqual(out, [six.identity])
    }

    // MARK: 2 - `Veil.flying`: hidden \ preHidden

    /// The one derivation four places rest on. `preHide` puts a card in both
    /// sets; `openSlots` takes it out of `preHidden` alone as its own step
    /// begins, so the difference is exactly what is in the air.
    func testFlyingIsTheVeilMinusWhatHasNotStarted() {
        let out = Veil.flying(hidden: [six.identity, nine.identity],
                              preHidden: [nine.identity])
        XCTAssertEqual(out, [six.identity])
    }

    /// A SEQUENCE THAT HAS VEILED BUT NOT STARTED FLIES NOTHING. The whole veil
    /// is still pre-hidden, so no attack tilts and every veiled card defers its
    /// fan slot - the "in anticipation" shift round-6 bug 10 was about.
    func testAVeilWithNoStepRunningFliesNothing() {
        let ids: Set<String> = [six.identity, nine.identity]
        XCTAssertTrue(Veil.flying(hidden: ids, preHidden: ids).isEmpty)
    }

    /// AND AT REST, EVERY HIDDEN CARD READS AS FLYING. `preHidden` empties at a
    /// teardown while `hidden` may not (an opened-but-unflown orphan stays in
    /// `hidden` alone), so a stranded card reads as permanently in flight. That
    /// is the shape of the round-17 orphan bug seen from this side, and it is
    /// what `strandedAtRest` counts - recorded here as the function's honest
    /// answer, not as desirable.
    func testAStrandedHiddenCardReadsAsFlyingForever() {
        XCTAssertEqual(Veil.flying(hidden: [six.identity], preHidden: []),
                       [six.identity])
    }

    /// A pre-hidden id that is not hidden is simply ignored - the result can
    /// never exceed `hidden`. (`preHide` writes both sets, so this is unreachable
    /// today; pinned so a future writer of one set alone cannot widen the answer.)
    func testFlyingIsAlwaysWithinTheVeil() {
        let out = Veil.flying(hidden: [six.identity],
                              preHidden: [nine.identity, queen.identity])
        XCTAssertEqual(out, [six.identity])
    }

    // MARK: 3 - `handSlotDeferred`: which veiled cards reserve no fan width

    /// The default for a veiled card: no slot yet. A deal heading for my hand is
    /// veiled from the moment the sequence starts, and if it reserved its slot
    /// from then my present cards would slide left "in anticipation" while other
    /// seats' steps played.
    func testAVeiledCardReservesNoSlot() {
        let out = Veil.handSlotDeferred(veiled: [six.identity], flying: [],
                                        holdback: [])
        XCTAssertEqual(out, [six.identity])
    }

    /// …EXCEPT THE ONE FLYING THIS INSTANT, which keeps its slot: it needs a
    /// real landing frame, and the fan opens for it as it lands.
    ///
    /// MUTANT: drop `.subtracting(flying)` and the incoming card has no slot to
    /// land in - every deal flies to the fan's edge and snaps.
    func testTheCardInTheAirKeepsItsSlot() {
        let out = Veil.handSlotDeferred(veiled: [six.identity, nine.identity],
                                        flying: [nine.identity], holdback: [])
        XCTAssertEqual(out, [six.identity])
    }

    /// A HELD-BACK CARD IS NEVER DEFERRED. It is pre-hidden (its table copy must
    /// stay invisible until its ghost lands) and pre-hidden is exactly what this
    /// set is built from - so without the subtraction the fan drops the very
    /// cards the holdback exists to keep on screen and the hand renders closed.
    ///
    /// MUTANT: drop `.subtracting(holdback…)` and this fails; on a device it is
    /// the round-42 report, my own replayed attack flying from a hand that has
    /// already closed over it.
    func testAHeldCardKeepsItsSlotEvenWhollyVeiled() {
        let out = Veil.handSlotDeferred(veiled: [six.identity, nine.identity],
                                        flying: [], holdback: [nine])
        XCTAssertEqual(out, [six.identity])
    }

    /// Held AND flying at once - the instant a holdback lets go, where the same
    /// id is in both subtrahends. Two subtractions of one id is still one id
    /// removed; recorded because that overlap is a real frame, not a hypothetical.
    func testHeldAndFlyingAtOnceIsStillJustNotDeferred() {
        let out = Veil.handSlotDeferred(veiled: [six.identity, nine.identity],
                                        flying: [nine.identity], holdback: [nine])
        XCTAssertEqual(out, [six.identity])
    }

    /// A holdback card that is not veiled at all changes nothing. (The normal
    /// state a beat after a teardown: the veil is down, the fan still holds.)
    func testAnUnveiledHoldbackIsInert() {
        let out = Veil.handSlotDeferred(veiled: [six.identity], flying: [],
                                        holdback: [nine, queen])
        XCTAssertEqual(out, [six.identity])
    }

    /// A CARD VEILED ONLY BY THE CONTROLLER DEFERS ITS SLOT. On an arriving
    /// bubble's first paint the animator knows nothing yet, so `flying` is empty
    /// and everything `pendingOpen` names defers - which is what stops the fan
    /// reserving width for cards the replay has not delivered.
    func testAnArrivalsFirstPaintDefersEverythingItTouches() {
        let veiled = Veil.veiled(hidden: [], pendingOpen: [six.identity],
                                 handBeforeMyMove: nil, myHand: nil)
        let out = Veil.handSlotDeferred(veiled: veiled, flying: [], holdback: [])
        XCTAssertEqual(out, [six.identity])
    }

    /// Deferring is only ever a narrowing of the veil - a card the board is
    /// DRAWING can never be denied its width. Asserted as a property over the
    /// whole small matrix rather than one case, because it is the thing that
    /// would break silently.
    func testDeferredIsAlwaysWithinTheVeil() {
        let ids = [six.identity, nine.identity, queen.identity]
        for veiled in Self.subsets(of: ids) {
            for flying in Self.subsets(of: ids) {
                for held in Self.subsets(of: ids) {
                    let cards = [six, nine, queen].filter { held.contains($0.identity) }
                    let out = Veil.handSlotDeferred(veiled: veiled, flying: flying,
                                                    holdback: cards)
                    XCTAssertTrue(out.isSubset(of: veiled),
                                  "deferred \(out.sorted()) escaped veil \(veiled.sorted())")
                }
            }
        }
    }

    // MARK: 4 - `Veil.fan`, and its relationship to the deferral

    /// What the fan itself draws nothing for: the veil minus the held cards.
    func testTheFanUnveilsWhatTheHoldbackHolds() {
        let out = Veil.fan(veiled: [six.identity, nine.identity], holdback: [nine])
        XCTAssertEqual(out, [six.identity])
    }

    /// THE PAIR'S INVARIANT: the fan never withholds a slot from a card it is
    /// drawing. `handSlotDeferred` is `Veil.fan` minus the cards in the air, so
    /// it is always a subset - a deferred-but-drawn card would be a card laid
    /// out at somebody else's slot, which is the "hand rows twitch" family.
    func testEveryDeferredCardIsAlsoOneTheFanIsNotDrawing() {
        let ids = [six.identity, nine.identity, queen.identity]
        for veiled in Self.subsets(of: ids) {
            for flying in Self.subsets(of: ids) {
                for held in Self.subsets(of: ids) {
                    let cards = [six, nine, queen].filter { held.contains($0.identity) }
                    let deferred = Veil.handSlotDeferred(veiled: veiled,
                                                         flying: flying,
                                                         holdback: cards)
                    let hidden = Veil.fan(veiled: veiled, holdback: cards)
                    XCTAssertTrue(deferred.isSubset(of: hidden),
                                  "deferred \(deferred.sorted()) is not within drawn-nothing "
                                  + "\(hidden.sorted())")
                }
            }
        }
    }

    // MARK: 5 - `Veil.grid`: the battle grid's two sets

    /// NOT SWEEPING, the live table: the grid honours the hand veil, and the
    /// sweep sets are not consulted at all. Junk in all three of them to prove
    /// it - this is the branch where they mean nothing.
    func testTheLiveGridReadsTheHandVeilAndNothingElse() {
        let g = Veil.grid(sweeping: false, veiled: [six.identity],
                          sweptFlown: [nine.identity],
                          sweepUnplaced: [queen.identity],
                          sweepArriving: [queen.identity],
                          flying: [six.identity])
        XCTAssertEqual(g.hidden, [six.identity])
        XCTAssertEqual(g.flyingNow, [six.identity])
    }

    /// SWEEPING, the pre-bout table: BOTH ends of the sequence are hidden -
    /// what has already flown OFF the grid (`sweptFlown`) and what has not yet
    /// flown ONTO it (`sweepUnplaced`, a bout-ending cover being replayed).
    ///
    /// MUTANT: pass `sweptFlown` alone and the replayed cover is on the grid
    /// from the first paint - the owner's "the cards just showed as covered,
    /// then went to discard". Pass `sweepUnplaced` alone and every swept card
    /// stays on the table beside its own ghost.
    func testTheSweepGridHidesBothEndsOfTheSequence() {
        let g = Veil.grid(sweeping: true, veiled: [],
                          sweptFlown: [six.identity],
                          sweepUnplaced: [nine.identity],
                          sweepArriving: [], flying: [])
        XCTAssertEqual(g.hidden, [six.identity, nine.identity])
    }

    /// A SWEEP TILTS ONLY FOR WHAT IS COMING DOWN. Everything else a sweep flies
    /// is LEAVING, with nothing left to tilt onto - so the animator's own
    /// "flying now" is deliberately not passed here, and `sweepArriving` is.
    ///
    /// MUTANT: pass `flying` on this branch too and the attack under a card that
    /// is being carried to the discard rotates as it goes.
    func testASweepTiltsOnlyForACardArrivingOnIt() {
        let g = Veil.grid(sweeping: true, veiled: [],
                          sweptFlown: [six.identity], sweepUnplaced: [],
                          sweepArriving: [nine.identity],
                          flying: [six.identity, queen.identity])
        XCTAssertEqual(g.flyingNow, [nine.identity])
    }

    /// THE CASE THAT KEEPS THE SWEEP SETS SEPARATE FROM THE HAND VEIL, and the
    /// reason round 45 did not collapse them: A PICKUP.
    ///
    /// The picked-up card lives in TWO places at once - on the table it is being
    /// swept from, and in the hand it is arriving into. `Veil.veiled` hides it
    /// for the hand's sake from the instant the sequence starts. If the sweep
    /// grid honoured that, the table copy would vanish before its own flight
    /// ever lifted it: the card would be gone from the table, absent from the
    /// hand, and the ghost would spawn out of nowhere. So in the SAME PAINT one
    /// identity is hidden on one grid and drawn on the other, and no single set
    /// can say both.
    func testAPickedUpCardIsVeiledInTheHandAndDrawnOnTheSweep() {
        let veiled = Veil.veiled(hidden: [six.identity], pendingOpen: nil,
                                 handBeforeMyMove: nil, myHand: nil)
        let sweeping = Veil.grid(sweeping: true, veiled: veiled,
                                 sweptFlown: [], sweepUnplaced: [],
                                 sweepArriving: [], flying: [])
        XCTAssertFalse(sweeping.hidden.contains(six.identity),
                       "the table copy must stay up until its own flight lifts it")
        let live = Veil.grid(sweeping: false, veiled: veiled,
                             sweptFlown: [], sweepUnplaced: [],
                             sweepArriving: [], flying: [])
        XCTAssertTrue(live.hidden.contains(six.identity),
                      "…and the same id IS hidden on the live grid, in the same paint")
    }

    /// A sweep that has neither flown nor is waiting on anything hides nothing -
    /// the settled middle of a bout-end hold, where the whole pre-bout table
    /// simply sits there.
    func testASweepAtRestDrawsItsWholeTable() {
        let g = Veil.grid(sweeping: true, veiled: [six.identity, nine.identity],
                          sweptFlown: [], sweepUnplaced: [],
                          sweepArriving: [], flying: [six.identity])
        XCTAssertTrue(g.hidden.isEmpty)
        XCTAssertTrue(g.flyingNow.isEmpty)
    }

    // MARK: 6 - the board hands these out, and hands out nothing else

    /// The outs are only pinned if the board actually USES them. Each of the
    /// four is asserted at its call site, in the house idiom of
    /// `HoldbackTests.testTheTraceAndItsTriggerAreTheSameArithmetic`: the value
    /// tests above are worthless if `body` quietly grows a second copy of the
    /// subtraction beside the named one.
    func testTheBoardAsksTheseFunctionsRatherThanRepeatingThem() throws {
        let src = try source("FoolishKit/Boards/MessageTableView.swift")
        for call in ["Veil.veiled(hidden: animator.hidden",
                     "Veil.handSlotDeferred(veiled: veiledCardIds",
                     "Veil.fan(veiled: veiledCardIds, holdback: fanHoldback)",
                     "Veil.grid(sweeping: sweeping, veiled: veiledCardIds"] {
            XCTAssertTrue(src.contains(call), "the board no longer calls \(call)")
        }
        // …and the inline forms they replaced are gone, so there is one answer.
        XCTAssertFalse(src.contains("animator.hidden.subtracting(animator.preHidden)"),
                                  "\"flying right now\" has a name (`Veil.flying`) - do not re-derive it")
        XCTAssertFalse(src.contains("sweeping ? sweepHidden"),
                                  "the grid's two sets come from `Veil.grid`, not a ternary per argument")
    }

    /// The DEBUG grid trace must report the SAME hidden set the grid is given.
    /// It is the only thing that says which cards the table is withholding, and
    /// a trace computing its own answer is worse than no trace - it was two
    /// copies of one ternary before this round.
    func testTheGridTraceReportsTheSetTheGridWasGiven() throws {
        let src = try source("FoolishKit/Boards/MessageTableView.swift")
        XCTAssertTrue(src.contains("Self.traceGrid(sweeping: sweeping, shown: shown,\n"
                                  + "                       hidden: grid.hidden,"),
                      "the trace must read the same value the grid does")
        XCTAssertTrue(src.contains("hidden: grid.hidden,\n"),
                      "…which is the grid's own argument")
        XCTAssertTrue(src.contains("flyingNow: grid.flyingNow)"))
    }

    // MARK: 7 - the one IN that collapsed: `sweepTableIds`

    /// `sweepTableIds` was a `@State` cache of the identities in `sweepBattles`,
    /// written beside it at the only two sites that touch either. Round 45 made
    /// it computed off `PreBoutTable.cardIds`, so this is what the cache used to hold.
    func testTheSweptTableIsEveryCardOnIt() {
        let uncovered = BattleView(attack: six, defense: nil)
        let covered = BattleView(attack: nine, defense: queen)
        XCTAssertEqual(PreBoutTable.cardIds([uncovered, covered]),
                                  [six.identity, nine.identity, queen.identity])
        XCTAssertTrue(PreBoutTable.cardIds([]).isEmpty,
                      "a dropped sweep holds no ids - what `dropSweep` used to assign by hand")
    }

    /// An uncovered attack contributes ONE id, not a nil-padded two. (The old
    /// inline flatMap said `+ (b.defense.map { [$0.identity] } ?? [])` at three
    /// sites; a fourth writer reaching for `compactMap` over a two-element array
    /// would be a silently different set.)
    func testAnUncoveredAttackContributesOnlyItself() {
        XCTAssertEqual(PreBoutTable.cardIds([BattleView(attack: six, defense: nil)]),
                                  [six.identity])
    }

    /// THE INVARIANT THE COLLAPSE RESTS ON: `sweepBattles` is written in exactly
    /// two places, and both of them are the sweep's own setters. While that
    /// holds, a derived `sweepTableIds` and the old hand-kept one are the same
    /// value at every instant; the moment a third writer appears they are not,
    /// and the derived one is the one that stays right.
    ///
    /// MUTANT: add `sweepBattles = view.battles` anywhere else in the file and
    /// this fails.
    func testOnlyTheSweepSettersEverWriteTheSweptTable() throws {
        let src = try source("FoolishKit/Boards/MessageTableView.swift")
        let writes = src.components(separatedBy: "sweepBattles = ").count - 1
        XCTAssertEqual(writes, 2,
                                  "`sweepBattles` must be written only by `setSweep` and `dropSweep` - "
                                  + "`sweepTableIds` is derived from it")
        XCTAssertTrue(src.contains(
            "private var sweepTableIds: Set<String> { PreBoutTable.cardIds(sweepBattles) }"),
                      "…and derived, not cached")
    }

    /// The FIVE sites that used to spell the derivation out now ask the kernel:
    /// `setSweep`, `sweepTableIds`, `coveredSweep`'s subset test and
    /// `sweepTableForReplay`'s two. The subset tests are the ones that matter -
    /// each decides whether one reconstruction of the pre-bout table "accounts
    /// for" another, and a set built one card differently drops a covered pair
    /// off the table mid-sequence. There is now no way to build one differently:
    /// it is anim_table_card_ids, once, and the board never names a battle's
    /// cards itself.
    ///
    /// `traceGrid`'s own walk is deliberately NOT folded in: it counts per SLOT
    /// (a card drawn twice is two), which is a different question from the set
    /// of identities a table holds.
    func testTheSweptIdsHaveOneDerivation() throws {
        let src = try source("FoolishKit/Boards/MessageTableView.swift")
        XCTAssertFalse(src.contains("func ids(_ bs: [BattleView])"),
                                  "`coveredSweep` must ask the kernel, not carry its own copy")
        XCTAssertFalse(src.contains("b.attack.identity] + (b.defense.map"),
                                  "naming a battle's cards belongs to `PreBoutTable.cardIds` alone")
    }

    // MARK: - helpers

    /// Every subset of a small id list, for the property assertions above.
    private static func subsets(of ids: [String]) -> [Set<String>] {
        (0..<(1 << ids.count)).map { mask in
            Set(ids.enumerated().compactMap { mask & (1 << $0.offset) != 0 ? $0.element : nil })
        }
    }
}
