// ConflictModelTests - THE CONFLICT MODEL (docs/ANIMATION_CATALOGUE.md,
// decided 1.0(28)): when something has to leave the board that a move did not
// take off it, the board reverses it - red, against the OLD base - before it
// plays anything else.
//
// Two layers under test, matching where the model lives:
//
//   THE VERDICT, per card: revert / keep / clear against what the arriving
//   chain vouches for. The rule itself is the KERNEL's now
//   (c/src/anim_plan.c anim_conflict_*, crossed by sdk/swift/ConflictWire.swift)
//   and is asserted natively in c/tests/tests.c; what these cases pin is the
//   same behaviour AS THE BOARD REACHES IT, through the wire. Getting any leg
//   of it wrong reproduces a bug the web already paid for: a false REVERT is
//   the "card flew home and popped straight back" flicker, a false KEEP is the
//   silent swap the owner rejected, and a missing CLEAR is the "someone picked
//   my card up and it flew back to my hand" flicker by name.
//
//   MUTATION-CHECKED across the crossing, each applied alone and reverted:
//     the facts appended AFTER the group sizes (a shifted wire) -> 6 failures
//     CLEAR decoded as KEEP                                     -> 2 failures
//     the reversal flight not flipped end for end               -> 1 failure
//     the dense card id off by one (drop the -1)                -> 0 failures
//       BEFORE testEveryCardInTheDeckCrossesAsItself existed, which is why that
//       case is there: an id scheme that disagrees with the kernel's is
//       invisible in the middle of the deck (both sides of every comparison go
//       through it) and only bites at the last card, where it walks off the end
//       -> 1 failure with it.
//
//   THE RETRACTION HANDSHAKE (MessageTurnController.offerArrival /
//   finishConflictAdopt): an arrival over a staged move publishes the OLD base
//   with the staged moves dropped, lets the board fly them home in red, and
//   adopts the latched arrival only when that lands. The hard invariant - the
//   board must never render new-base-with-old-staged-moves, not even for one
//   frame - is what the deferral exists for, so most tests here are about
//   WHAT IS TRUE BETWEEN the offer and the finish.

import XCTest
@testable import FoolishKit

@MainActor
final class ConflictModelTests: XCTestCase {

    /// THE TRANSPORT IS PART OF THE QUESTION. The kernel refuses to answer a
    /// verdict until a host says which client it is (anim_plan.h), and every
    /// board in this target is the iMessage one, whose messages carry the whole
    /// game in a total order. The app declares this in its own entry point;
    /// a test process has no entry point, so it declares it here.
    override func setUp() {
        super.setUp()
        AnimTransport.declare(.chain)
    }

    private func c(_ suit: Int, _ v: Int) -> Card { Card(s: suit, v: v) }

    /// The facts stated directly. `table` is a row of uncovered attacks unless
    /// a test builds its own battles.
    private func facts(moved: [Card] = [], table: [Card] = [],
                       myHand: [Card] = [],
                       battles: [BattleView]? = nil) -> ConflictFacts {
        ConflictFacts(moved: moved,
                      openTable: battles ?? table.map { BattleView(attack: $0, defense: nil) },
                      myHand: myHand)
    }

    // MARK: - the verdict

    /// The canonical conflict: my staged, unsent card. No other device ever saw
    /// it, so the arriving chain neither moves it nor shows it - it must fly
    /// home, red. If this were KEEP, the model would be the silent swap the
    /// owner rejected ("the card I chose is on the table one frame and gone the
    /// next, with nothing to say it was mine or why it went").
    func testAStagedCardTheArrivalKnowsNothingOfFliesHome() {
        let mine = c(0, 9)
        XCTAssertEqual(facts(table: [c(1, 7)]).verdict(mine, dest: .table), .revert)
    }

    /// THE CLEAR LEG, the one the catalogue says to get right first, checked
    /// with the card ALSO standing on the opening table - which is exactly what
    /// a picked-up card does. CLEAR must win over KEEP's standing check,
    /// because the arriving replay animating the card off the table IS its
    /// animation; a red flight home first is the web's "I put a card down,
    /// someone picked it up, and it flew back to my hand" flicker.
    func testACardTheArrivingReplayItselfMovesNeverFliesHomeFirst() {
        let seven = c(2, 7)
        XCTAssertEqual(facts(moved: [seven], table: [seven]).verdict(seven, dest: .table),
                       .clear)
    }

    /// The MERGE twin: a card the arriving chain's opening board shows at its
    /// post spot is vouched for by the newest truth and must not move. Flying
    /// it home only for the incoming seed to snap it straight back would be the
    /// clear-flicker one board later - this is what keeps a burst of arrivals
    /// that each EXTEND the animating chain from theatrically un-playing moves
    /// that really happened.
    /// THE TRANSPORT IS A REAL INPUT ON THIS SIDE TOO. The kernel answers the
    /// shared tests either way, but the question at the end of the verdict -
    /// is "not accounted for" conclusive? - is one this app has to have
    /// declared. Unset, the kernel returns FIO_ETRANSPORT and this
    /// reader turns it into NO plan, which is loud, rather than quietly
    /// reverting or quietly keeping.
    ///
    /// MUTATION: give AnimTransport a default (declare `.chain` inside
    /// `ConflictPlan.ask`) and the middle assertion fails - a client that never
    /// says would inherit iMessage's answer without anyone noticing.
    func testAVerdictNeedsSomebodyToHaveSaidWhichClientThisIs() {
        let mine = c(0, 9)
        let f = facts(table: [c(1, 7)])
        XCTAssertEqual(AnimTransport.current, .chain, "setUp declared it, and it reads back")

        AnimTransport.declare(.chain)
        XCTAssertEqual(f.verdict(mine, dest: .table), .revert,
                       "a chain is complete, so a card it does not account for is doomed")

        AnimTransport.undeclare()         // FIO_TRANSPORT_UNSET - no Swift case for it
        XCTAssertNil(AnimTransport.current, "nothing has said")
        XCTAssertEqual(ConflictPlan([[ConflictMotion(card: mine, dest: .table)]], facts: f),
                       .empty,
                       "no transport, no plan - the reader degrades to no animation rather "
                       + "than to somebody else's answer")
        AnimTransport.declare(.chain)
    }

    func testACardTheNewestBoardVouchesForStaysPut() {
        let king = c(3, 13)
        XCTAssertEqual(facts(table: [king]).verdict(king, dest: .table), .keep)
        // BOTH SIDES of a battle stand. A cover read as "not on the table"
        // would be false-reverted off a table that is holding it.
        let cover = c(2, 11)
        XCTAssertEqual(facts(battles: [BattleView(attack: king, defense: cover)])
                        .verdict(cover, dest: .table),
                       .keep, "the cover on a standing battle is standing too")
    }

    /// A masked back has no identity to conflict on and no persistent view to
    /// fly back from (it landed INTO a badge). Reverting it would conjure a
    /// card out of a badge - the same class of wrongness as a deal flying from
    /// the pile onto the table.
    func testAMaskedBackIsKeptNotConjured() {
        XCTAssertEqual(ConflictFacts.unknown.verdict(nil, dest: .pool), .keep)
        XCTAssertEqual(ConflictFacts.unknown.verdict(nil, dest: .table), .keep)
        XCTAssertEqual(ConflictFacts.unknown.verdict(Card.hidden, dest: .table), .keep,
                       "a card back is the same case as no card at all")
    }

    /// A card that went into a POOL (the discard pile, an opponent's badge) is
    /// bookkeeping: even a chain that vouches for nothing does not justify
    /// flying ghosts back out of a pile.
    func testAPoolDestinationIsBookkeepingNotAFlight() {
        XCTAssertEqual(ConflictFacts.unknown.verdict(c(0, 6), dest: .pool), .keep)
        XCTAssertEqual(ConflictFacts.unknown.verdict(c(0, 6), dest: .table), .revert,
                       "…while an unvouched-for table card does fly, so this is not 'keep all'")
    }

    /// The standing check reads the side of the board the motion actually
    /// LANDED on. A staged pickup put the cards in MY HAND; the arriving chain
    /// showing those same cards on its TABLE is their PRE position, not their
    /// post one, so they must still fly back out (dest .myHand checks the hand
    /// set, not the table set) - and the mirror must hold too.
    func testMyHandDestChecksMyHandNotTheTable() {
        let picked = c(1, 11)
        XCTAssertEqual(facts(table: [picked]).verdict(picked, dest: .myHand), .revert,
                       "the arriving TABLE showing it does not vouch for my HAND holding it")
        XCTAssertEqual(facts(myHand: [picked]).verdict(picked, dest: .myHand), .keep)
        XCTAssertEqual(facts(myHand: [picked]).verdict(picked, dest: .table), .revert,
                       "…and the mirror: my hand does not vouch for a table spot")
    }

    /// EVERY CARD CROSSES AS ITSELF. The identity a card travels on is the
    /// kernel's dense id (`card_to_id`), and a Swift encoding that disagrees
    /// with it is invisible through most of the deck - both sides of every
    /// comparison go through the same encoder, so a consistent relabelling
    /// still answers correctly - and then walks off the end at the last card,
    /// where the kernel reads a corrupt wire and hands back an unreadable
    /// chain. An unreadable chain KEEPS everything, so the failure mode is the
    /// silent swap: nothing flies home, ever. Hence the whole deck, and hence
    /// the second assertion, which is the one that bites.
    func testEveryCardInTheDeckCrossesAsItself() {
        for s in 0...3 {
            for v in 1...13 {
                let card = c(s, v)
                XCTAssertEqual(facts(table: [card]).verdict(card, dest: .table), .keep,
                               "\(card.identity) is standing on the opening table")
                XCTAssertEqual(ConflictFacts.unknown.verdict(card, dest: .table), .revert,
                               "\(card.identity) is disowned by a chain that vouches for nothing")
            }
        }
    }

    // MARK: - where each motion put its card

    /// The dest mapping is the flight builder's own: placements land on the
    /// table, my draws and pickups land in my hand, and an opponent's draw or
    /// pickup - like a discard - lands in a pool with no per-card view. A wrong
    /// mapping sends the standing check to the wrong side of the board.
    func testWhereEachKindOfMotionPutItsCard() {
        XCTAssertEqual(ConflictDest(of: .attackPass, seat: 2, mySeat: 0), .table)
        XCTAssertEqual(ConflictDest(of: .cover, seat: 2, mySeat: 0), .table)
        XCTAssertEqual(ConflictDest(of: .pickup, seat: 0, mySeat: 0), .myHand)
        XCTAssertEqual(ConflictDest(of: .refill, seat: 0, mySeat: 0), .myHand)
        XCTAssertEqual(ConflictDest(of: .refill, seat: 2, mySeat: 0), .pool)
        XCTAssertEqual(ConflictDest(of: .pickup, seat: 2, mySeat: 0), .pool)
        XCTAssertEqual(ConflictDest(of: .cardsToTrash, seat: -1, mySeat: 0), .pool)
    }

    // MARK: - the facts

    /// The facts are read off the same opening every arrival already carries:
    /// the stream's cards, the opening table BOTH SIDES of each battle, and my
    /// hand. They are OPAQUE - the sets live in the kernel now
    /// (anim_conflict_facts) so there is only one of them - so what they say is
    /// asserted the only way it is ever read: through a verdict. Dropping a
    /// defense card from the table set would false-revert a standing cover;
    /// naming a masked card is impossible by construction and must not throw
    /// the read off the cards around it.
    func testTheFactsReadTheStreamAndTheOpeningBoard() {
        let atk = c(0, 6), def = c(0, 9), moved = c(2, 12), inHand = c(3, 8)
        let prior = GameView(status: 1, numPlayers: 2, powerSuit: 1, deckCount: 10,
                             discardCount: 0, hasFlipped: true, firstAttacker: 0,
                             defender: 1, viewer: 1, goodMask: 0, gameOver: -1,
                             flipped: nil,
                             battles: [BattleView(attack: atk, defense: def)],
                             eliminationOrder: [],
                             players: [
                                PlayerView(seat: 0, name: "a", status: 2, handCount: 6,
                                           awaitingAttack: false, strategyKey: 0, hand: nil),
                                PlayerView(seat: 1, name: "b", status: 2, handCount: 1,
                                           awaitingAttack: false, strategyKey: 0, hand: [inHand]),
                             ])
        let ev = GameEvent(type: EventType.pickup.rawValue, seat: 1, msg: 0, from: 2, to: 1,
                           cards: [moved, nil], target: nil, battle: nil, state: nil)
        let f = ConflictFacts(events: [ev], prior: prior)

        XCTAssertEqual(f.verdict(moved, dest: .table), .clear,
                       "the stream's own card is read off its events")
        XCTAssertEqual(f.verdict(atk, dest: .table), .keep,
                       "the opening table's attack stands")
        XCTAssertEqual(f.verdict(def, dest: .table), .keep,
                       "…and so does the cover on it")
        XCTAssertEqual(f.verdict(inHand, dest: .myHand), .keep,
                       "my hand on the opening board is read too")
        XCTAssertEqual(f.verdict(c(1, 5), dest: .table), .revert,
                       "a card the opening names nowhere is disowned - the masked "
                       + "entry beside `moved` named nothing and moved nothing else")
    }

    // MARK: - the reversal steps

    private func flight(_ card: Card, id: String? = nil,
                        from: CGRect = CGRect(x: 0, y: 0, width: 50, height: 70),
                        to: CGRect = CGRect(x: 100, y: 100, width: 50, height: 70),
                        angle: Double = 0) -> Flight {
        Flight(id: id ?? "f-\(card.identity)", card: card, from: from, to: to, angle: angle)
    }

    /// "The cards travel back the way they came": last motion first (reverse
    /// group order), each flight flipped end for end - it starts where the card
    /// is RESTING NOW (the original `to`), lands at its source, runs its tilt
    /// backwards, and is marked red. Any of those flipped fields silently kept
    /// forward-way-round would fly the reversal in the wrong direction or paint
    /// it as an ordinary move.
    func testTheReversalFliesBackTheWayItCameLastMotionFirst() throws {
        let first = flight(c(0, 6), from: CGRect(x: 0, y: 0, width: 50, height: 70),
                           to: CGRect(x: 200, y: 40, width: 50, height: 70), angle: 96)
        let second = flight(c(1, 8), from: CGRect(x: 10, y: 300, width: 50, height: 70),
                            to: CGRect(x: 220, y: 60, width: 50, height: 70))
        let steps = MessageTableView.reversalSteps(
            debt: [[FlownMotion(flight: first, dest: .table)],
                   [FlownMotion(flight: second, dest: .table)]],
            facts: .unknown)
        XCTAssertEqual(steps.count, 2)
        XCTAssertEqual(steps[0].first?.card, second.card, "the LAST motion reverses FIRST")
        XCTAssertEqual(steps[1].first?.card, first.card)
        let r = try XCTUnwrap(steps[1].first)
        XCTAssertEqual(r.from, first.to, "it lifts from where the card is resting now")
        XCTAssertEqual(r.to, first.from, "…and lands where the card came from")
        XCTAssertEqual(r.fromAngle, first.angle, "the tilt runs backwards too")
        XCTAssertEqual(r.angle, first.fromAngle)
        XCTAssertTrue(r.revert, "a reversal without the red is indistinguishable from a play")
    }

    /// The verdicts gate the flights: in one flown group, only the card the
    /// newest chain disowns flies back - the card its replay moves (CLEAR) and
    /// the card its board vouches for (KEEP) build nothing, and a group left
    /// with nothing to fly is dropped rather than played as a beat of silence.
    func testOnlyDisownedMotionFliesBack() {
        let disowned = c(0, 9), cleared = c(1, 7), standing = c(2, 10)
        let group = [FlownMotion(flight: flight(disowned), dest: .table),
                     FlownMotion(flight: flight(cleared), dest: .table),
                     FlownMotion(flight: flight(standing), dest: .table)]
        let steps = MessageTableView.reversalSteps(
            debt: [group, [FlownMotion(flight: flight(c(3, 12)), dest: .pool)]],
            facts: facts(moved: [cleared], table: [standing]))
        XCTAssertEqual(steps.count, 1, "the all-keep pool group plays no beat of silence")
        XCTAssertEqual(steps[0].map { $0.card?.identity }, [disowned.identity])
    }

    /// A flight is an ordinary play unless something says otherwise - the red
    /// must be impossible to acquire by accident.
    func testAFlightIsNotARevertByDefault() {
        XCTAssertFalse(flight(c(0, 6)).revert)
    }

    // MARK: - the retraction handshake (controller)

    private let joins = [MessageJoin(seat: 0, name: "Eva"), MessageJoin(seat: 1, name: "Alex")]
    private let zero8 = Data(repeating: 0, count: 8)

    /// A 2p board where I am the defender with a staged cover, plus the chain it
    /// opened - the same searched deal SendWindowTests uses, because a staged
    /// cover is the commonest thing an arrival lands on.
    private func stagedBoard() async throws -> (c: MessageTurnController, opened: Data) {
        let k = MessageKernel.shared
        for salt in UInt8(1)...UInt8(60) {
            try await k.newGame(seed: Data(repeating: salt, count: 32), players: 2)
            var opener = -1
            var first: Move?
            for s in 0..<2 {
                if let m = (await k.residentLegal(seat: s)).first(where: { $0.type == .attack }) {
                    opener = s; first = m; break
                }
            }
            guard let atk = first else { continue }
            try await k.apply(seat: opener, move: atk)
            let me = 1 - opener
            guard (await k.residentLegal(seat: me)).contains(where: { $0.type == .cover })
            else { continue }
            let opened = try await k.seal(phase: 2, lastActorSeat: opener, gameId: 0xC01,
                                          parent8: zero8, joins: joins)
            let env = try await MessageEnvelope.decode(payload: opened, viewer: -1)
            let c = MessageTurnController(parentPayload: opened, parent: env, mySeat: me)
            await c.begin()
            let cover = try XCTUnwrap(c.legal.first { $0.type == .cover })
            await c.apply(cover)
            XCTAssertEqual(c.pending.count, 1)
            return (c, opened)
        }
        throw XCTSkip("no 2p deal in 60 tries let the defender cover")
    }

    /// A chain one legal move PAST `base`, played by any seat that is not
    /// `notSeat`, sealed like a real bubble.
    private func chainAfter(_ base: Data, notSeat: Int) async throws -> (payload: Data, env: MessageEnvelope)? {
        let k = MessageKernel.shared
        _ = try await MessageEnvelope.decode(payload: base, viewer: -1)
        for s in 0..<2 where s != notSeat {
            guard let m = (await k.residentLegal(seat: s)).first(where: { $0.type != .wait })
            else { continue }
            try await k.apply(seat: s, move: m)
            let sealed = try await k.seal(phase: 2, lastActorSeat: s, gameId: 0xC01,
                                          parent8: zero8, joins: joins)
            let env = try await MessageEnvelope.decode(payload: sealed, viewer: -1)
            return (sealed, env)
        }
        return nil
    }

    /// THE ORDERING, which is the whole model: an arrival over a staged move
    /// does NOT adopt - it retracts. Between the offer and the finish the
    /// controller must read as the OLD base with nothing staged (the board the
    /// red flight plays against), never as the new base and never as
    /// new-base-with-old-staged-moves; the adopt happens only at
    /// `finishConflictAdopt`. If this were wrong the staged card would vanish
    /// between two frames again (adopt-immediately), or the board would rebuild
    /// stale moves onto a parent they were never legal against (adopt-with-
    /// pending), which are the two defects the model replaces.
    func testAnArrivalOverAStagedMoveRetractsBeforeItAdopts() async throws {
        let (c, opened) = try await stagedBoard()
        XCTAssertNotNil(c.view?.battles.first?.defense, "my staged cover is on the table")
        guard let arriving = try await chainAfter(opened, notSeat: c.mySeat)
        else { throw XCTSkip("no opponent move on this deal") }

        c.setBoardWatching(true)
        defer { c.setBoardWatching(false) }
        await c.offerArrival(payload: arriving.payload, parent: arriving.env)

        // Between offer and finish: the OLD base, nothing staged, verdict facts
        // published for the board, and the arrival latched - not adopted.
        XCTAssertTrue(c.conflictRetracting)
        XCTAssertTrue(c.pending.isEmpty, "the retraction dropped the staged move")
        XCTAssertNil(c.view?.battles.first?.defense, "the board stands at the base - cover retracted")
        XCTAssertEqual(c.basePayload, opened, "the new chain is NOT adopted while the red flight plays")
        XCTAssertNotNil(c.conflictFacts, "the board needs the verdicts to know what flies")
        XCTAssertTrue(c.lastChangeWasUndo, "the board routes this through its reverse-flight machinery")

        await c.finishConflictAdopt()
        XCTAssertFalse(c.conflictRetracting)
        XCTAssertEqual(c.basePayload, arriving.payload, "…and the finish adopts the latched arrival")
        XCTAssertTrue(c.pending.isEmpty)
    }

    /// ROUND 40: A MOVE REFUSED MID-RETRACTION SAYS SO.
    ///
    /// The retraction guard at the top of `apply` returns in silence - no view
    /// change, no `rejectTick` - because "the tap simply does nothing, exactly
    /// as it would have a frame later when the arrival's board is up". Which
    /// was true of the KERNEL and false of the BOARD: `MessageTableView.playAt`
    /// has already veiled the tapped cards and planted a resting ghost by the
    /// time this is called (synchronously, which is the only moment early
    /// enough to beat the paint), and the only things that ever take that veil
    /// down again are the view change and `rejectTick`. With neither, the cards
    /// stayed pre-hidden for the LIFE OF THE BOARD - `handSlotDeferred` keeps
    /// them out of a centred fan, so the hand lays out fewer cards than it
    /// holds with nothing animating. That is the owner's device breadcrumb,
    /// term for term:
    ///
    ///     fan-rows 1 rows laid=1 hand=4 width=398 deferred=3 veiled=3
    ///              preHidden=3 hidden=3 settled=true seq=0
    ///
    /// So the refusal is a FACT handed back rather than a silence: `apply`
    /// answers false, and the board gives the veil back on the spot.
    func testAMoveRefusedMidRetractionReportsThatItDidNotHappen() async throws {
        let (c, opened) = try await stagedBoard()
        guard let arriving = try await chainAfter(opened, notSeat: c.mySeat)
        else { throw XCTSkip("no opponent move on this deal") }
        c.setBoardWatching(true)
        defer { c.setBoardWatching(false) }
        await c.offerArrival(payload: arriving.payload, parent: arriving.env)
        XCTAssertTrue(c.conflictRetracting, "the fixture did not pose a retraction")

        // Any move at all - the guard is ahead of every rule, and the kernel is
        // never asked. `c.legal` is the retracted base's own menu.
        let move = try XCTUnwrap(c.legal.first { $0.type != .wait })
        let tick = c.rejectTick
        let applied = await c.apply(move)

        XCTAssertFalse(applied,
                       "a move refused mid-retraction reported success, so the board "
                       + "never gives back the veil it raised over the tapped cards - "
                       + "they stay laid out nowhere for the life of the board")
        XCTAssertEqual(c.rejectTick, tick,
                       "the refusal stays SILENT on screen (no toast, no haptic) - "
                       + "the answer is the return value, not a rejection")
        XCTAssertTrue(c.pending.isEmpty, "the refused move must not have been staged")
    }

    /// The other half of the same contract, so `false` cannot be read as "this
    /// always says no": a move the kernel really applies answers true. The
    /// board hangs its veil release off that answer, so a `false` here would
    /// hand back the veil over a card that IS on its way to the table - the
    /// original bug with the sign flipped.
    func testAnAppliedMoveReportsThatItHappened() async throws {
        // The same searched deal `stagedBoard` uses, stopped one step earlier:
        // the chain as it was DEALT, opened by the seat that may attack.
        let k = MessageKernel.shared
        for salt in UInt8(1)...UInt8(60) {
            try await k.newGame(seed: Data(repeating: salt, count: 32), players: 2)
            var opener = -1
            for s in 0..<2 {
                if (await k.residentLegal(seat: s)).contains(where: { $0.type == .attack }) {
                    opener = s; break
                }
            }
            guard opener >= 0 else { continue }
            let dealt = try await k.seal(phase: 2, lastActorSeat: opener, gameId: 0xC02,
                                         parent8: zero8, joins: joins)
            let env = try await MessageEnvelope.decode(payload: dealt, viewer: -1)
            let c = MessageTurnController(parentPayload: dealt, parent: env, mySeat: opener)
            await c.begin()
            guard let attack = c.legal.first(where: { $0.type == .attack }) else { continue }

            let applied = await c.apply(attack)

            XCTAssertTrue(applied, "a move the kernel accepted reported a refusal")
            XCTAssertEqual(c.pending.count, 1)
            return
        }
        throw XCTSkip("no 2p deal in 60 tries let its opener attack")
    }

    /// A duplicate delivery of the chain my staged move is built on is NOT a
    /// conflict: the staged move was composed against exactly those bytes and
    /// must survive. Before the conflict model this fell into `adopt`'s
    /// conservative rebuild and silently wiped the staged move; under the model
    /// that shrug would have become a RED RETRACTION fired by Messages
    /// re-delivering a bubble, which is a retraction with nothing to retract
    /// for. (This deliberately extends the old duplicate guard, which only
    /// covered the nothing-staged case.)
    func testADuplicateOfTheChainUnderMyStagedMoveIsNoConflict() async throws {
        let (c, opened) = try await stagedBoard()
        let staged = try XCTUnwrap(c.view)
        c.setBoardWatching(true)
        defer { c.setBoardWatching(false) }

        let env = try await MessageEnvelope.decode(payload: opened, viewer: -1)
        await c.offerArrival(payload: opened, parent: env)

        XCTAssertFalse(c.conflictRetracting)
        XCTAssertEqual(c.pending.count, 1, "the staged move survives a duplicate delivery")
        XCTAssertEqual(c.view, staged, "…and the board never moved")
    }

    /// With no board mounted there is nobody to fly the retraction, so an
    /// arrival adopts immediately - the conflict model is about a board that is
    /// showing something. A deferral here would stall every compact-drawer
    /// arrival behind the failsafe for nothing.
    func testWithNoBoardWatchingAnArrivalAdoptsImmediately() async throws {
        let (c, opened) = try await stagedBoard()
        guard let arriving = try await chainAfter(opened, notSeat: c.mySeat)
        else { throw XCTSkip("no opponent move on this deal") }

        await c.offerArrival(payload: arriving.payload, parent: arriving.env)

        XCTAssertFalse(c.conflictRetracting)
        XCTAssertEqual(c.basePayload, arriving.payload, "no board, no theatre - adopt as ever")
        XCTAssertTrue(c.pending.isEmpty)
    }

    /// The no-board safety net: if nothing ever reports the reversal landed
    /// (the board died mid-flight), the latched arrival still lands. A dropped
    /// arrival is a board frozen one move in the past for good - a far worse
    /// failure than a skipped animation.
    func testTheFailsafeLandsTheArrivalWhenNoBoardFinishes() async throws {
        let was = MessageTurnController.conflictFailsafeSeconds
        MessageTurnController.conflictFailsafeSeconds = 0.15
        defer { MessageTurnController.conflictFailsafeSeconds = was }

        let (c, opened) = try await stagedBoard()
        guard let arriving = try await chainAfter(opened, notSeat: c.mySeat)
        else { throw XCTSkip("no opponent move on this deal") }
        c.setBoardWatching(true)
        defer { c.setBoardWatching(false) }

        await c.offerArrival(payload: arriving.payload, parent: arriving.env)
        XCTAssertTrue(c.conflictRetracting)
        XCTAssertEqual(c.basePayload, opened)

        try await Task.sleep(nanoseconds: 600_000_000)
        XCTAssertFalse(c.conflictRetracting)
        XCTAssertEqual(c.basePayload, arriving.payload, "the failsafe adopted the latched arrival")
    }

    /// The burst rule mid-retraction: a newer arrival replaces the LATCH and
    /// nothing else - the reversal is not restarted (it already returns the
    /// board to the parent state) and the finish adopts the NEWEST chain, so
    /// the intermediate board is never replayed. Explicitly not a queue.
    func testANewerArrivalMidRetractionWinsTheLatch() async throws {
        let (c, opened) = try await stagedBoard()
        guard let first = try await chainAfter(opened, notSeat: c.mySeat)
        else { throw XCTSkip("no opponent move on this deal") }
        guard let second = try await chainAfter(first.payload, notSeat: -1)
        else { throw XCTSkip("no second move on this deal") }
        c.setBoardWatching(true)
        defer { c.setBoardWatching(false) }

        await c.offerArrival(payload: first.payload, parent: first.env)
        XCTAssertTrue(c.conflictRetracting)
        await c.offerArrival(payload: second.payload, parent: second.env)
        XCTAssertTrue(c.conflictRetracting, "one retraction covers the burst")

        await c.finishConflictAdopt()
        XCTAssertEqual(c.basePayload, second.payload,
                       "the finish adopts the newest chain, never the intermediate one")
    }

    /// Mid-retraction the chain on screen is being replaced: a move staged in
    /// that window would be composed against a base the latched arrival is
    /// about to supersede. This pins the APPLY door - without its guard the tap
    /// would stage a move onto the doomed base and the assertions below read
    /// it. The undo call documents the matching no-op; its own guard's only
    /// distinguishable window is inside `offerArrival`'s peek suspension
    /// (before `pending` empties), which no deterministic test can sit in - so
    /// that guard is belt-and-braces, pinned here only as "nothing moves".
    func testMovesAndUndoAreRefusedMidRetraction() async throws {
        let (c, opened) = try await stagedBoard()
        guard let arriving = try await chainAfter(opened, notSeat: c.mySeat)
        else { throw XCTSkip("no opponent move on this deal") }
        c.setBoardWatching(true)
        defer { c.setBoardWatching(false) }
        await c.offerArrival(payload: arriving.payload, parent: arriving.env)
        XCTAssertTrue(c.conflictRetracting)
        let mid = c.view

        if let m = c.legal.first(where: { $0.type != .wait }) { await c.apply(m) }
        XCTAssertTrue(c.pending.isEmpty, "a tap mid-retraction stages nothing")
        XCTAssertEqual(c.view, mid, "…and moves nothing")
        await c.undo()
        XCTAssertEqual(c.view, mid, "an undo mid-retraction is refused - one is already flying")

        await c.finishConflictAdopt()
        XCTAssertEqual(c.basePayload, arriving.payload)
    }
}
