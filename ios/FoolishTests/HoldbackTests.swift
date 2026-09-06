// HoldbackTests - ROUND 43, everything `handHoldback` touches.
//
// Round 42 gave the message board a HOLDBACK: while an open replay flies my own
// played cards out of my hand, those cards stay DRAWN in the fan so they leave
// the hand rather than appearing from behind it. It works, and it introduced
// three defects, all of them the same mistake in different clothes - the rest of
// the board was never told the fan is now laying out cards the kernel hand does
// not contain.
//
//  1. THEY WERE TAPPABLE. `hand(_:)` deliberately un-veils held cards so the fan
//     draws them as ordinary cards - which also gave them an ordinary drag
//     gesture and ordinary selection. `toggle` wrote the identity into
//     `selection` regardless of whether the card was in my hand; `selectedCards`
//     then filtered it out, so the move was never playable, but the identity
//     STAYED. Pick that table up later and the card comes home already selected,
//     and `actionBar` gates both `canPickup` and `canDone` on `cards.isEmpty` -
//     so Take and Good silently vanish, with nothing on screen to explain it and
//     no way to deselect a card that was never selectable. Dragging one raised a
//     "move not allowed" toast about a card the player had already played.
//     Owner: "do not allow these replay cards to be selected or clicked."
//
//  2. THE ROW-JUMP TRACE WAS BLIND TO IT. `laidHandCount` - the trigger for the
//     `fan-rows` breadcrumb that exists to answer "we had ten cards and they
//     said good, why did the hand change rows?" - counted the KERNEL hand, while
//     `handHeight` beside it measured `fanCards`. The one row change the
//     holdback causes (the drop when the held cards let go) could not fire it.
//
//  3. IT HAD NO RESCUE. Every other veil in this file has an unconditional
//     safety net. The holdback was cleared in two places, both inside
//     `runEventStream`, so a replay superseded by anything else - `flyUndoReturn`,
//     `flyUndoRelease`, the genesis-deal fallback - left cards I had already
//     played sitting in my fan indefinitely.
//
// WHAT IS ASSERTED HOW. The three rules that are values (`selectionAfterTap`,
// `laidCount`, `holdbackIsMine`) are tested as values - they were written static
// and pure for exactly that. The two that are about WHERE a line is written (the
// fan is handed the lock; every teardown calls the rescue) are source tests,
// the same choice CountOwnershipTests and WoodHitRegionTests make and for the
// same reason: `@State` on a SwiftUI view has no seam.
//
// MUTATION-CHECKED, each test against the exact defect it names - see the
// per-test notes.

import XCTest
@testable import FoolishKit

final class HoldbackTests: XCTestCase {

    private func hand() -> [Card] {
        [Card(s: 0, v: 6), Card(s: 1, v: 9), Card(s: 2, v: 12)]
    }

    private func source(_ path: String) throws -> String {
        let here = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
        return try String(contentsOf: here.deletingLastPathComponent()
            .appendingPathComponent(path), encoding: .utf8)
    }

    // MARK: 1 - the selection may only ever name cards that are in my hand

    /// The ordinary behaviour first, so nothing below can pass by refusing every
    /// tap: a card in my hand toggles on, then off.
    func testTappingAHandCardStillToggles() {
        let h = hand()
        let on = Veil.selectionAfterTap([], card: h[1], hand: h)
        XCTAssertEqual(on, [h[1].identity])
        let off = Veil.selectionAfterTap(on, card: h[1], hand: h)
        XCTAssertTrue(off.isEmpty)
    }

    /// THE BUG. A held-back replay card is NOT in `view.me.hand` (it has already
    /// left it - that is why it needs holding back), so a tap on one must leave
    /// the selection exactly as it was.
    ///
    /// MUTANT: restore the old body (`if selection.contains … else insert`) and
    /// this fails - the played card lands in the selection.
    func testTappingACardThatIsNotInMyHandSelectsNothing() {
        let h = hand()
        let played = Card(s: 3, v: 14)              // flew out on the open replay
        XCTAssertFalse(h.map(\.identity).contains(played.identity))
        XCTAssertTrue(Veil.selectionAfterTap([], card: played, hand: h).isEmpty,
                      "a card the kernel hand does not contain can never be selected")
        // …and it cannot be smuggled in alongside a real selection either.
        let live: Set<String> = [h[0].identity]
        XCTAssertEqual(Veil.selectionAfterTap(live, card: played, hand: h), live)
    }

    /// The invariant, not just the tap: an identity that has since LEFT my hand
    /// is swept on the next tap whatever that tap was for. This is the half that
    /// actually cures the vanished Take/Good - a selection carrying a card that
    /// is not in the hand disables both buttons and cannot be cleared by tapping
    /// the card, because the card is not on screen to tap.
    ///
    /// MUTANT: drop the `.intersection(mine)` and this fails while the test
    /// above still passes - which is the point of having both.
    func testAStaleIdentityIsSweptOutOfTheSelection() {
        let h = hand()
        // A REAL card that is simply not in this hand. It used to be `v: 14`,
        // which no deck holds - and the sweep would then have "passed" for the
        // wrong reason once the rule became the kernel's, because a card with no
        // dense id has no bit to sweep.
        let gone = Card(s: 3, v: 13).identity
        let next = Veil.selectionAfterTap([gone, h[0].identity],
                                          card: h[2], hand: h)
        XCTAssertFalse(next.contains(gone), "a card that left my hand may not stay selected")
        XCTAssertEqual(next, [h[0].identity, h[2].identity])
    }

    // MARK: 1b - the fan is handed a lock, and the lock is invisible

    /// The board must pass the held ids to `FHandFan`, or the gesture is still
    /// live however careful `toggle` is: a drag never reaches `toggle` at all,
    /// it reaches `onDragEnded` and raises a "move not allowed" toast.
    ///
    /// MUTANT: delete the `locked:` argument from `hand(_:crop:reserveNoSlot:)`
    /// and this fails.
    func testTheBoardLocksTheHeldCardsInTheFan() throws {
        let src = try source("FoolishKit/Boards/MessageTableView.swift")
        let head = try XCTUnwrap(src.range(of: "private func hand(_ view: GameView"))
        let fn = String(src[head.lowerBound...].prefix(2500))
        XCTAssertTrue(fn.contains("locked: Set(fanHoldback.map(\\.identity))"),
                      "the fan must be told which cards are held, or they stay draggable")
    }

    /// …and the lock must be the SILENT one. `disabled` would have gated the
    /// same gesture, but `FCard` renders it at opacity 0.5 - and a held card
    /// looking like an ordinary card right up to the moment it flies is the
    /// entire point of the holdback. So `locked` may reach the gesture and must
    /// never reach `FCard`.
    ///
    /// MUTANT: pass the held ids as `disabled:` instead (or add
    /// `disabled: locked.contains(...)` to the FCard init) and this fails.
    func testTheLockGatesTheGestureAndPaintsNothing() throws {
        let src = try source("FoolishKit/DesignSystem/FHandFan.swift")
        XCTAssertTrue(src.contains("guard !locked.contains(card.identity) else { return }"),
                      "the drag must stand down for a locked card")
        XCTAssertTrue(src.contains("guard !locked.contains(c.identity) else { return }"),
                      "…and so must the tap, which is synthesized inside the same gesture")
        // The FCard init in `cardView` - everything up to the closing paren of
        // the constructor call. `locked` must not appear anywhere in it.
        let cardInit = try XCTUnwrap(src.range(of: "FCard(card: card,"))
        let ctor = String(src[cardInit.lowerBound...].prefix(220))
        XCTAssertFalse(ctor.contains("locked"),
                       "a locked card must look EXACTLY like an unlocked one - "
                       + "`disabled` dims to 0.5, which is the affordance this must not have")
        // Belt: `FCard` has no such flag to be given by any other route. (Its
        // `disabled` is documented there as "dimmed + locked", which is the
        // word - not the parameter.)
        let card = try source("FoolishKit/DesignSystem/FCard.swift")
        XCTAssertFalse(card.contains("var locked"), "the lock is a gesture rule, not a paint rule")
        XCTAssertFalse(card.contains("locked:"), "…and FCard takes no such argument")
    }

    /// The board's own hand still passes `hidden` MINUS the held cards - the
    /// un-veiling round 42 added, which is what makes them visible in the fan at
    /// all. Pinned because the round-43 lock only makes sense on a card that is
    /// drawn: if this un-veil were ever reverted the lock would be guarding an
    /// empty slot and the two changes would silently disagree.
    ///
    /// ROUND 45: the subtraction moved BEHIND a name (`fanVeil`) with the rest
    /// of the veil's outs, so the source half of this now pins the call and the
    /// value half - which is the part that was always the point - is asserted
    /// directly rather than read off a string. Same rule, same expression, one
    /// of the two halves no longer a spelling test.
    func testTheHeldCardsAreStillDrawnInTheFan() throws {
        let src = try source("FoolishKit/Boards/MessageTableView.swift")
        XCTAssertTrue(src.contains("hidden: Veil.fan(veiled: veiledCardIds, holdback: fanHoldback)"),
                      "the fan's hidden set must still be the veil MINUS the held cards")
        let held = Card(s: 0, v: 6)
        XCTAssertFalse(Veil.fan(veiled: [held.identity, "x"], holdback: [held])
                                .contains(held.identity),
                                "a held card is drawn in the fan, veiled or not")
    }

    // MARK: 2 - the row-jump trace counts the hand the fan lays out

    /// `laidCount` is the fan's own arithmetic: the hand it is really given
    /// (`fanCards`) minus the deals still deferring a slot.
    ///
    /// MUTANT: drop `holding:` from `laidCount` (count the kernel hand, as
    /// `laidHandCount` used to) and the first assertion fails - which is exactly
    /// the row change the `fan-rows` trace could not see.
    func testTheLaidCountIncludesWhatTheHoldbackIsHolding() {
        let h = hand()
        // REAL cards, and that now matters: the count is the kernel's
        // (anim_laid_count) over dense ids, so a value outside 1...13 is not a
        // card at all and is not laid out. This used to read `v: 14`, which the
        // old Swift set algebra counted happily because it only ever compared
        // identity STRINGS.
        let held = [Card(s: 3, v: 13), Card(s: 3, v: 11)]
        XCTAssertEqual(HandLayout.laidCount(hand: h, holding: held, deferred: []), 5,
                       "the fan lays out the hand PLUS the holdback, so the row split is taken on 5")
        XCTAssertEqual(HandLayout.laidCount(hand: h, holding: [], deferred: []), 3,
                       "…and the count drops when the holdback lets go - the row change "
                       + "the trace exists to explain")
    }

    /// A deal still deferring its slot is not laid out, holdback or no holdback -
    /// the older half of the same rule, kept honest here so the round-43 change
    /// cannot quietly drop it.
    func testADeferredSlotIsNotLaidOut() {
        let h = hand()
        XCTAssertEqual(HandLayout.laidCount(hand: h, holding: [],
                                            deferred: [h[0].identity]), 2)
    }

    /// A card the kernel hand has already got back is not counted twice. Same
    /// rule `fanCards` keeps (the fan places by index, so a doubled identity is
    /// two cards in one slot) - asserted here as well because `laidCount` is
    /// what the row split is taken on.
    func testAHeldCardTheHandAlreadyHasIsNotDoubled() {
        let h = hand()
        XCTAssertEqual(HandLayout.laidCount(hand: h, holding: [h[1]], deferred: []), 3)
    }

    /// Both ends of the trace read the SAME function. The `.onChange` body
    /// deliberately re-reads live rather than using the captured body locals
    /// (its own comment says why), so there are two computations of one number
    /// and they must not be two different computations - a line that prints "1
    /// row" next to a count needing two invites a wrong conclusion, which is
    /// worse than no trace.
    ///
    /// MUTANT: restore either site to `hand.filter { !deferred.contains(…) }` and
    /// this fails.
    func testTheTraceAndItsTriggerAreTheSameArithmetic() throws {
        let src = try source("FoolishKit/Boards/MessageTableView.swift")
        XCTAssertTrue(src.contains("let laidHandCount = HandLayout.laidCount("),
                      "the trigger must count the hand the fan lays out")
        let onChange = try XCTUnwrap(src.range(of: ".onChange(of: laidHandCount)"))
        let block = String(src[onChange.upperBound...].prefix(1400))
        XCTAssertTrue(block.contains("HandLayout.laidCount("),
                      "the live re-read must be the same function as the trigger")
        XCTAssertFalse(block.contains("hand.filter { !deferred.contains"),
                       "…not its own copy of a slightly different sum")
    }

    // MARK: 3 - the holdback's rescue

    /// The ownership rule as a value. A teardown owns the holdback only if the
    /// holdback was armed no later than the veil that teardown itself raised -
    /// so a superseded sequence can never wipe the holdback its replacement just
    /// armed (the hazard written out at `runEventStream`'s teardown), while a
    /// sequence that supersedes a replay without being one still rescues it.
    ///
    /// EQUAL EPOCHS ARE THE SAME SEQUENCE: `replayLastMoveOnOpen` stamps the
    /// holdback with the very `veiledAt` it hands to `runEventStream`, so `<`
    /// would mean the common case never releases at all.
    ///
    /// MUTANT: change `<=` to `<` and the first assertion fails; change it to
    /// `>=` and the last one fails.
    func testOnlyTheVeilThatCouldHaveRaisedItMayTakeItDown() {
        XCTAssertTrue(Veil.holdbackIsMine(armedAt: 7, teardownAt: 7),
                      "a stream is handed the same epoch its own open stamped")
        XCTAssertTrue(Veil.holdbackIsMine(armedAt: 4, teardownAt: 7),
                      "a later teardown rescues an older holdback - the whole point")
        XCTAssertFalse(Veil.holdbackIsMine(armedAt: 8, teardownAt: 7),
                       "a superseded teardown must not wipe the holdback its replacement armed")
    }

    /// Every teardown that can end a sequence over a live holdback calls the
    /// rescue - and there is exactly ONE place left that empties it, so the rule
    /// above cannot be bypassed by a fourth copy drifting out of step.
    ///
    /// MUTANT: delete the `releaseHoldback` call from any one of the four and
    /// this fails naming it.
    func testEveryTeardownRescuesTheHoldback() throws {
        let src = try source("FoolishKit/Boards/MessageTableView.swift")
        // One writer: `releaseHoldback` itself. (`handHoldback = isSpectating ? …`
        // is the arming, and the fly-time `removeAll` is cards letting go one
        // group at a time - neither is a bare clear.)
        let bareClears = src.components(separatedBy: "handHoldback = []").count - 1
        XCTAssertEqual(bareClears, 1,
                       "only `releaseHoldback` may empty the holdback, so only one place "
                       + "has to get the epoch rule right")
        // …called wherever the veil itself is handed back. Five sites: the
        // empty-stream guard, `runEventStream`'s teardown, the two undo
        // reverses and the genesis-deal fallback - the last three being the
        // ones round 42 left with no rescue at all. Paired positionally rather
        // than by name so a SIXTH teardown, added later, fails this until it
        // gets the rescue too.
        let veilClears = src.components(separatedBy: "animator.clearPreHidden(raisedBy: veiledAt)").count - 1
        XCTAssertEqual(veilClears, 5, "a teardown was added or removed - see below")
        XCTAssertEqual(src.components(separatedBy: "releaseHoldback(raisedBy: veiledAt)").count - 1,
                       veilClears,
                       "every place that hands the veil back must hand the holdback back")
        var searched = src[...]
        var rescued = 0
        while let hit = searched.range(of: "animator.clearPreHidden(raisedBy: veiledAt)") {
            // The rescue sits within a few lines of the veil's own clear in
            // every teardown - before it in the empty-stream guard, after it in
            // the four real teardowns - so look both ways.
            let lo = src.index(hit.lowerBound, offsetBy: -400, limitedBy: src.startIndex) ?? src.startIndex
            let hi = src.index(hit.upperBound, offsetBy: 400, limitedBy: src.endIndex) ?? src.endIndex
            if src[lo..<hi].contains("releaseHoldback(raisedBy: veiledAt)") { rescued += 1 }
            searched = searched[hit.upperBound...]
        }
        XCTAssertEqual(rescued, veilClears,
                       "every teardown that clears the veil must clear the holdback with it")
        // And `runEventStream`'s empty-stream guard, which returns ahead of the
        // teardown entirely. Anchored on the line only it carries, because
        // `guard !events.isEmpty` appears three times in this file.
        let empty = try XCTUnwrap(src.range(of: "ROUND 16: HAND THE COUNTS BACK"))
        XCTAssertTrue(String(src[empty.upperBound...].prefix(1600))
                        .contains("releaseHoldback(raisedBy: veiledAt)"),
                      "a stream that came back empty flies nothing and must let go too")
    }

    // MARK: 4 (1.0(43)) - the fan opens on six cards, it does not grow into them
    //
    // MUTATIONS RUN for this section, each against the test that names it:
    //   1. `fanHoldback` returns `armed` unconditionally (i.e. the shipped
    //      behaviour) -> testTheVeilAnswersTheHoldbackBeforeItIsArmed fails.
    //   2. the veil asks with `max(mySeat, 0)` -> testASpectatorHoldsNothingBack.
    //   3. the open window returns `pending` alone -> testAStandingHoldback… (1st).
    //   4. the open window returns `armed + pending` -> testAStandingHoldback… (2nd).
    //   5. `laidHandCount` reads `handHoldback` again ->
    //      testNothingThatDrawsReadsTheArmedHoldbackDirectly.
    //   6. the veil's window never shuts (`controller.openReplayEvents` in place
    //      of `unstartedReplay`) -> same test, on the changed call.

    /// One placement of mine, the shape an open replay of my own attack carries.
    private func myAttack(_ card: Card, seat: Int = 0) -> [GameEvent] {
        [GameEvent(type: EventType.attackPass.rawValue, seat: seat, msg: 0, from: 0, to: 0,
                   cards: [card], target: nil, battle: nil, state: nil)]
    }

    /// THE BUG. Owner, 1.0(43): "we shouldn't start with 5 cards, fade the one I
    /// threw back in and rearrange animation, then throw it out. The visual
    /// should START with the 6 cards, and just fly the one."
    ///
    /// `handHoldback` is armed by `replayLastMoveOnOpen`, which runs from the
    /// view's `onChange` - a paint after the board's first. So on that first
    /// paint the fan was handed the kernel hand alone and the played card faded
    /// in and re-centred the row a moment later. While the veil's window is
    /// open the answer has to come from the controller, exactly as `pendingOpen`
    /// does for the counts.
    ///
    /// MUTANT: return `armed` unconditionally (i.e. read `handHoldback`
    /// directly, as every render site did) and this fails - the fan opens at
    /// five. Driven against the rig too: with the mutant the unified log carries
    /// `fan-rows ... laid=6 hand=5 held=1 ... seq=0` on a cold open of my own
    /// attack, and an `onChange` only fires on a CHANGE, so a line there at all
    /// is the count having been 5 for the paints before it. With the fix that
    /// line is gone and the only `fan-rows` left is the drop to 5 as the card
    /// leaves.
    func testTheVeilAnswersTheHoldbackBeforeItIsArmed() {
        let played = Card(s: 0, v: 6)
        let held = MessageTableView.fanHoldback(unstarted: myAttack(played),
                                                armed: [], mySeat: 0)
        XCTAssertEqual(held.map(\.identity), [played.identity],
                       "the card I played must be in the fan on the FIRST paint, not one later")
    }

    /// …and once the sequence has taken over, the armed holdback is the answer -
    /// it is the one that SHRINKS as each group's flight is built
    /// (`hand lets go`), and a veil that kept answering would put the cards back
    /// as fast as the stream let them go.
    func testAStartedReplayHandsTheAnswerToTheArmedHoldback() {
        let played = Card(s: 0, v: 6)
        XCTAssertTrue(MessageTableView.fanHoldback(unstarted: nil, armed: [], mySeat: 0).isEmpty,
                      "window shut and nothing armed: the fan lays the kernel hand")
        XCTAssertEqual(MessageTableView.fanHoldback(unstarted: nil, armed: [played], mySeat: 0)
                        .map(\.identity), [played.identity],
                       "window shut: the armed holdback answers on its own")
    }

    /// A SEAT-LESS viewer places nothing. Spectating IS seat -1, and the kernel
    /// spends -1 on "no particular player" as well as on "no seat", so the veil
    /// passes `mySeat` straight through and this rule is `myPlacedCards`' own -
    /// pinned here because the veil now asks it a paint earlier than the arming
    /// ever did, and because it is what makes the guardless call site correct.
    ///
    /// MUTANT: make the veil's seat `max(mySeat, 0)` (the obvious "clean up the
    /// negative" edit) and this fails - a spectator's fan grows seat 0's card.
    func testASpectatorHoldsNothingBack() throws {
        let played = Card(s: 0, v: 6)
        XCTAssertTrue(MessageTableView.fanHoldback(unstarted: myAttack(played, seat: 0),
                                                   armed: [], mySeat: -1).isEmpty,
                      "a viewer with no seat played nothing, so the fan holds nothing back")
        // …and the veil really does hand the seat over unguarded, which is the
        // half that makes the line above load-bearing rather than academic.
        // Anchored INSIDE the property: `mySeat: controller.mySeat` is spelt
        // four times in this file and a whole-file search would pass on any of
        // the other three.
        let src = try source("FoolishKit/Boards/MessageTableView.swift")
        let head = try XCTUnwrap(src.range(of: "private var fanHoldback: [Card] {"))
        let body = String(src[head.lowerBound...].prefix(200))
        XCTAssertTrue(body.contains("mySeat: controller.mySeat)"),
                      "the veil asks with my real seat; -1 is the spectator and the kernel knows it")
    }

    /// The handoff must be a UNION, not a swap. A second bubble raising a fresh
    /// veil over a sequence that is still flying MY cards must not pull those
    /// cards out of the fan mid-flight; and a card both sides name appears once,
    /// because the fan places by index and a duplicate identity is two cards in
    /// one slot.
    ///
    /// MUTANT: return `pending` alone when the window is open and the first
    /// assertion fails; return `armed + pending` unfiltered and the second does.
    func testAStandingHoldbackSurvivesAFreshVeil() {
        let flying = Card(s: 1, v: 9), arriving = Card(s: 2, v: 12)
        let both = MessageTableView.fanHoldback(unstarted: myAttack(arriving),
                                                armed: [flying], mySeat: 0)
        XCTAssertEqual(both.map(\.identity), [flying.identity, arriving.identity],
                       "a card still in flight keeps its slot when a new veil goes up")
        let same = MessageTableView.fanHoldback(unstarted: myAttack(flying),
                                                armed: [flying], mySeat: 0)
        XCTAssertEqual(same.map(\.identity), [flying.identity],
                       "a card both sides name is held ONCE - the fan places by index")
    }

    /// AND NOTHING THAT DRAWS MAY READ THE ARMED STATE. The value tests above
    /// are worth nothing if a render site goes on asking `handHoldback`
    /// directly: that site is the one that opens at five. So the arming, the
    /// two teardown rescues and the fly-time `removeAll` are the ONLY mentions
    /// left, and every one of them is a WRITE or the rescue's own guard.
    ///
    /// MUTANT: put `holding: handHoldback` back into `laidHandCount` (the
    /// `fan-rows` trigger, which is the value the rig reads the bug off) and
    /// this fails.
    func testNothingThatDrawsReadsTheArmedHoldbackDirectly() throws {
        let src = try source("FoolishKit/Boards/MessageTableView.swift")
        // Every line naming it, minus the prose that describes it and the
        // traces that print it (a log is not a layout input).
        let mentions = src.split(separator: "\n", omittingEmptySubsequences: false)
            .map { $0.trimmingCharacters(in: .whitespaces) }
            .filter { $0.contains("handHoldback") && !$0.contains("handHoldbackAt") }
            .filter { !$0.hasPrefix("//") && !$0.contains("AnimLog.say(") && !$0.hasPrefix("+ \"") }
        // What is left: the declaration, the veil's own read of it, the arming,
        // the fly-time `removeAll` (a read and a write on two lines) and the
        // rescue's guard and clear. Nothing that lays anything out.
        let allowed = [
            "@State private var handHoldback: [Card] = []",
            "Self.fanHoldback(unstarted: unstartedReplay, armed: handHoldback,",
            "handHoldback = isSpectating ? []",
            "if !handHoldback.isEmpty {",
            "if !left.isEmpty, self.handHoldback.contains(where: { left.contains($0.identity) }) {",
            "self.handHoldback.removeAll { left.contains($0.identity) }",
            "guard !handHoldback.isEmpty else { return }",
            "handHoldback = []",
        ]
        XCTAssertEqual(mentions.count, allowed.count,
                       "a mention was added or removed: \(mentions)")
        for line in mentions {
            XCTAssertTrue(allowed.contains(line),
                          "`\(line)` reads the armed holdback - a render site must ask "
                          + "`fanHoldback`, which answers on the board's FIRST paint")
        }
        // Belt: the property really is what the render sites ask.
        XCTAssertTrue(src.contains("private var fanHoldback: [Card] {"),
                      "the veil's answer must exist to be asked")
    }
}
