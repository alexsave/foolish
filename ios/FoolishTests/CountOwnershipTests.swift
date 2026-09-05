// THE FROZEN COUNTS BELONG TO WHOEVER IS ANIMATING - rounds 42, 43 and 44.
//
// A running sequence freezes the seat badges, the deck, the discard, the out
// badges and the role marks to the board BEFORE its move and walks them
// forward one step per landing flight (`runEventStream`). A LIVE PLAY of mine
// that lands in the middle of that used to write over the same state twice:
// `freezeCounts` re-froze it to a board several steps further on, and
// `releaseCounts` then cleared it outright. The stream's next step put it back.
//
// MEASURED ON THE RIG, and this is where the numbers come from - scenario
// `take`, 4 players, HARNESS_AUTOMOVE + HARNESS_AUTOMOVE_NOWAIT (which is the
// rig playing INTO a running replay, the one lifecycle nothing else poses),
// HARNESS_SLOWMO=8 so the beats are separable. Seat 3's badge, one card ever
// leaving that seat:
//
//              step 1     my play     step 2     step 3
//   before       6           5          6          5      <- down, back UP, down
//   after        6           6          6          5      <- one card, one step
//
// And seat 0's, filmed at t=4.6s: before the fix the badge read 4 with an EMPTY
// table - it had counted down a card that had not started moving.
//
// ROUND 43 FOUND A THIRD WRITER. `releaseLivePlayVeil` - the one path for a
// play that comes to nothing - made the very same writes with no guard at all,
// so round 42's fix was undone one line after it was applied: `play` deferred
// the freeze (correctly) and then released what it had just declined to touch.
// Its bluntest reproduction is a tap during a red conflict retraction, because
// `MessageTurnController.apply` returns false for the whole of that window and
// `flyUndoReturn` holds it open at `sequenceDepth >= 1`.
//
// ROUND 44 STOPPED COUNTING WRITERS. Two rounds, two instances, one shape: the
// rule lived as a `guard` line copy-pasted into three functions, and there were
// twenty-odd assignments across six thousand lines that could each have been a
// fourth. So the five fields moved into `ShownLedger` (its own file, `private`
// storage), and the only way to change them is `write(_:)`, which takes a
// CLAIM. A new writer in MessageTableView.swift cannot assign to the fields at
// all - that is the compiler's job now, not a reviewer's - and cannot call
// `write` without picking a claim off the list and reading what each one means.
//
// WHAT IS LEFT FOR A SOURCE TEST, and why these are still source tests. The
// compiler enforces the funnel; it cannot enforce that the funnel stays a
// funnel (somebody making the fields settable, or handing `&fields` out
// somewhere else), nor that each caller claims what it actually IS. Both are
// facts about WHERE something is written, which is exactly what this can check
// - the same choice, for the same reason, as WoodHitRegionTests. The behaviour
// itself is pinned by the rig measurement above, which is reproducible from the
// command line in that header.
//
// FUNCTION BODIES ARE CUT ON INDENTATION, not on a brace count. Round 43's
// version of this file walked braces, which cannot tell a `{` in a comment from
// a `{` in code - and these functions are more comment than code. Every method
// here sits at four spaces inside `MessageTableView` and closes with a line
// that is exactly `    }`, so that line is the end of it. (`releaseCounts` is a
// nested func at eight.)
//
// MUTATION-CHECKED, round 44, fourteen mutants, all of them red before the
// code went back - the claim on each of the six owners swapped for a wrong one
// (six mutants, including `replayLastMoveOnOpen` demoted to `.bystander`, which
// is the trap this round was warned about, and `syncRoles` demoted, which would
// silently drop a pass's shield flight); a ledger write added in a seventh
// function; a second `&fields` hand-out; a field made settable and then
// assigned from the board; `clearSweep()` unhooked from the ledger's verdict;
// the role seed turned into an override; `allows` reduced to `true`, which IS
// rounds 42 and 43 undone; the guard re-spelled with the counter; and a loose
// `@State` override put back on the board.
//
// AND ONE THAT NEVER REACHED A TEST, which is the point of the whole shape:
// assigning `ledger.deck` from the board without first making the property
// settable does not compile at all - "cannot assign to property: 'deck' is a
// get-only property". The tests below cover what the compiler cannot: somebody
// making it settable, or claiming to be somebody they are not.

import XCTest
@testable import FoolishKit

final class CountOwnershipTests: XCTestCase {

    // MARK: - reading the source

    private func source(_ name: String) throws -> [String] {
        // #filePath is this file; the board sits two directories over.
        let here = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
        let url = here.deletingLastPathComponent()
            .appendingPathComponent("FoolishKit/Boards/\(name)")
        return try String(contentsOf: url, encoding: .utf8)
            .components(separatedBy: "\n")
    }

    /// The lines of `func <name>(`, from its declaration to the line that closes
    /// it - which at this indentation is the whole of the function and nothing
    /// else. See the header for why this is not a brace count.
    private func body(of fn: String, in src: [String], indent: Int = 4) throws -> [String] {
        let pad = String(repeating: " ", count: indent)
        let decl = try XCTUnwrap(src.firstIndex { line in
            line.hasPrefix(pad) && !line.hasPrefix(pad + " ")
                && line.contains("func \(fn)(")
        }, "no func \(fn) at indent \(indent)")
        let close = try XCTUnwrap(src[decl...].firstIndex(of: pad + "}"),
                                  "func \(fn) never closes at indent \(indent)")
        return Array(src[decl...close])
    }

    /// Code only - every `//` comment stripped. Most of this file's assertions
    /// are about statements, and these two files explain themselves at such
    /// length that a test which cannot tell a comment from a statement would
    /// fail on the very sentence documenting the thing it is checking.
    private func code(_ lines: [String]) -> [String] {
        lines.filter { !$0.trimmingCharacters(in: .whitespaces).hasPrefix("//") }
    }

    private let owners = [("freezeCounts", 4), ("releaseCounts", 8),
                          ("releaseLivePlayVeil", 4), ("runEventStream", 4),
                          ("replayLastMoveOnOpen", 4), ("syncRoles", 4)]

    // MARK: - the funnel

    /// THE FIVE FIELDS LIVE IN ONE PLACE. Not five pieces of `@State` on the
    /// board any more: one ledger, whose storage the board cannot reach.
    func testTheBoardHoldsOneLedgerAndNoLooseOverrides() throws {
        let src = try code(source("MessageTableView.swift"))
        XCTAssertEqual(src.filter { $0.contains("@State private var ledger = ShownLedger()") }.count, 1,
                       "the board should hold exactly one ShownLedger")
        for gone in ["deckCountOverride", "discardCountOverride", "seatCountOverride",
                     "outShown", "roleShown"] {
            XCTAssertFalse(src.contains { $0.contains("@State") && $0.contains(gone) },
                           "\(gone) is back as loose @State - it belongs in ShownLedger, "
                           + "where a write has to name its claim")
        }
    }

    /// …AND THE BOARD CANNOT ASSIGN THEM. The compiler already says so (they are
    /// read-only computed properties over `private` storage in another file);
    /// this says it too, so that making one settable is a red test and not a
    /// quiet re-opening of rounds 42 and 43.
    func testNothingAssignsALedgerFieldDirectly() throws {
        let src = try code(source("MessageTableView.swift"))
        // `ledger.deck =`, `ledger.hand[3] =`, and so on. `==` is a read.
        let write = try NSRegularExpression(
            pattern: #"ledger\.(deck|discard|hand|out|roles)(\[[^\]]*\])?\s*=[^=]"#)
        for (i, line) in src.enumerated() {
            let r = NSRange(line.startIndex..., in: line)
            XCTAssertNil(write.firstMatch(in: line, range: r),
                         "line \(i + 1) assigns a ledger field directly: \(line.trimmingCharacters(in: .whitespaces))")
        }
    }

    /// THE FUNNEL STAYS A FUNNEL. `write` is handed the storage as `inout`, and
    /// that is the one hand-out: a second `&fields` anywhere in the file - a
    /// convenience mutator, a "just this once" escape - would let a caller
    /// change what the badges show without ever naming a claim, which is the
    /// whole of what rounds 42 and 43 cost.
    func testTheStorageEscapesOnlyThroughWrite() throws {
        let src = try source("ShownLedger.swift")
        XCTAssertTrue(code(src).contains { $0.contains("private var fields = Fields()") },
                      "the five fields must be private storage, not a settable property")
        let handOuts = code(src).enumerated().filter { $0.element.contains("&fields") }
        XCTAssertEqual(handOuts.count, 1, "exactly one place may hand the storage out")
        let write = try body(of: "write", in: src)
        XCTAssertTrue(write.contains { $0.contains("&fields") },
                      "…and that place is `write`, which is where the claim is checked")
    }

    // MARK: - who claims what

    /// NO FOURTH WRITER, SILENTLY. Every ledger write in the board must sit
    /// inside one of the six functions that are known to own or arm the ledger.
    /// A new one somewhere else fails here and has to be argued for in the
    /// claim list - which is the review this rule never got the first two times.
    func testEveryLedgerWriteSitsInsideAKnownOwner() throws {
        let src = try source("MessageTableView.swift")
        var accounted: [String] = []
        for (fn, indent) in owners {
            accounted += code(try body(of: fn, in: src, indent: indent))
                .filter { $0.contains("ledger.write(") }
        }
        let all = code(src).filter { $0.contains("ledger.write(") }
        XCTAssertFalse(all.isEmpty, "the board writes the ledger somewhere")
        XCTAssertEqual(all.count, accounted.count,
                       "a ledger write outside freezeCounts / releaseCounts / "
                       + "releaseLivePlayVeil / runEventStream / replayLastMoveOnOpen / "
                       + "syncRoles - say which of the four claims it is and add it here")
    }

    /// AND EACH ONE CLAIMS WHAT IT IS. One line per function is the whole rule
    /// rounds 42-44 were about, and it is now checkable per call site rather
    /// than "is there a guard in here somewhere".
    func testEachOwnerClaimsWhatItActuallyIs() throws {
        let src = try source("MessageTableView.swift")
        let expected: [String: (String, Int)] = [
            // The three bystanders: a live play, a plain-move release, a
            // refusal. All three defer to a running sequence (round 42/43).
            "freezeCounts": (".bystander", 4),
            "releaseCounts": (".bystander", 8),
            "releaseLivePlayVeil": (".bystander", 4),
            // The owner. Guarding ANY of these would freeze every badge on the
            // board for good, which is far worse than the twitch they fix.
            "runEventStream": (".sequence", 4),
            // Arming a ledger for a sequence that has not claimed
            // `animSequenceToken` yet - see ShownClaim.arming for the
            // regression a `.bystander` here would cause.
            "replayLastMoveOnOpen": (".arming", 4),
            // The one diff-aware advance, and the only animator of a pass's
            // shield hand-off when no sequence is running.
            "syncRoles": (".handOff", 4)
        ]
        let claims = [".sequence", ".arming", ".handOff", ".bystander"]
        for (fn, (want, indent)) in expected {
            let lines = code(try body(of: fn, in: src, indent: indent))
                .filter { $0.contains("ledger.write(") }
            XCTAssertFalse(lines.isEmpty, "\(fn) should still write the ledger")
            for line in lines {
                for claim in claims {
                    XCTAssertEqual(line.contains("ledger.write(\(claim)"), claim == want,
                                   "\(fn) must claim \(want): \(line.trimmingCharacters(in: .whitespaces))")
                }
            }
        }
    }

    /// THE RULE ITSELF, with no board and no running sequence anywhere near it.
    /// Exactly one of the four claims ever stands down; the other three are the
    /// owner in its three shapes.
    func testOnlyABystanderStandsDownForARunningSequence() {
        for claim in [ShownClaim.sequence, .arming, .handOff] {
            XCTAssertTrue(ShownLedger.allows(claim, sequencing: true),
                          "\(claim) owns the ledger and must never be refused")
            XCTAssertTrue(ShownLedger.allows(claim, sequencing: false))
        }
        XCTAssertFalse(ShownLedger.allows(.bystander, sequencing: true),
                       "a bystander must not write over a running sequence's ledger")
        XCTAssertTrue(ShownLedger.allows(.bystander, sequencing: false),
                      "…and must write normally on a board at rest, which is the common case")
    }

    // MARK: - the halves that must NOT defer

    /// The other half of the same rule, which a guard placed too high would
    /// break just as badly: the cards THIS play hid are its own property. No
    /// sequence knows about them and nothing else will ever hand them back, so a
    /// refusal that lands mid-flight must still reveal them or they are gone
    /// from the fan for the life of the board (that is round 40's leak, and it
    /// must not be re-opened in the name of round 43). Eight spaces = the
    /// function's own top level = it runs on every call.
    func testTheRefusedPlacementIsStillGivenBackUnconditionally() throws {
        let b = try body(of: "releaseLivePlayVeil", in: try source("MessageTableView.swift"))
        for write in ["handBeforeMyMove = nil", "pendingPlacement = nil",
                      "pendingCover = nil", "animator.cancelHeld(ids)",
                      "animator.reveal(ids)"] {
            XCTAssertTrue(b.contains("        \(write)"),
                          "\(write) belongs to this play alone and must run on every call")
        }
    }

    /// …and the piece that is NOT the ledger but is handed back on exactly the
    /// same terms. The pre-bout table `play` laid out is frozen by the same play
    /// and dropped by the same sequence teardown, so it goes back if and only if
    /// the ledger did - which is what `write` returning a Bool is for. Written
    /// out as one line so this cannot pass on a `clearSweep()` that has drifted
    /// out of the verdict.
    func testTheSweptTableIsHandedBackOnTheSameTermsAsTheLedger() throws {
        let b = code(try body(of: "releaseLivePlayVeil", in: try source("MessageTableView.swift")))
        XCTAssertTrue(b.contains("        if released { clearSweep() }"),
                      "the swept table must follow the ledger's own verdict")
        XCTAssertFalse(b.contains("        clearSweep()"),
                       "an unconditional clearSweep takes a running sequence's cards off the table")
    }

    /// The ledger is a SEQUENCE's property, so exactly one place may hand it
    /// back unconditionally: `runEventStream`'s teardown, and only on the branch
    /// that has established it is the newest sequence. Pinned because the whole
    /// fix above rests on that release still happening - a board whose badges
    /// never come back off their overrides is frozen for good, which is a worse
    /// defect than the twitch.
    func testTheStreamTeardownStillReleasesItUnconditionally() throws {
        let src = try source("MessageTableView.swift")
        let stream = code(try body(of: "runEventStream", in: src))
        let teardown = try XCTUnwrap(stream.firstIndex { $0.contains("if mySeq == animSequenceToken {") },
                                     "the teardown's newest-sequence branch")
        let after = stream[teardown...].prefix(12).joined(separator: "\n")
        XCTAssertTrue(after.contains("ledger.write(.sequence)"),
                      "the newest sequence's teardown must still hand the ledger back")
        XCTAssertTrue(after.contains("l.deck = nil"))
        XCTAssertTrue(after.contains("l.hand = [:]"))
        XCTAssertFalse(after.contains(".bystander"),
                       "the teardown runs INSIDE its own sequence - claiming bystander there "
                       + "would mean the ledger is never released at all")
    }

    /// A SEED IS NOT AN OVERRIDE, and that semantic has to survive the move into
    /// the ledger. `freezeCounts` may only seed the roles when there are none:
    /// writing the current view over them would freeze the marks to a board that
    /// has ALREADY rotated, and the hand-off the running sequence was about to
    /// play would find nothing to hand over. `runEventStream`'s cold-open seeds
    /// are the same shape for the same reason - a board that was already frozen
    /// keeps what it froze.
    func testTheSeedsAreStillSeeds() throws {
        let src = try source("MessageTableView.swift")
        let freeze = code(try body(of: "freezeCounts", in: src)).joined(separator: "\n")
        XCTAssertTrue(freeze.contains("if l.roles == nil { l.roles = RoleState(v) }"),
                      "freezeCounts seeds the roles, it does not overwrite them")
        let stream = code(try body(of: "runEventStream", in: src)).joined(separator: "\n")
        XCTAssertTrue(stream.contains("if l.roles == nil"),
                      "the cold-open role seed must not overwrite a frozen board")
        XCTAssertTrue(stream.contains("if l.out == nil"),
                      "…nor the out-badge seed")
        let open = code(try body(of: "replayLastMoveOnOpen", in: src)).joined(separator: "\n")
        XCTAssertTrue(open.contains("if l.roles == nil"),
                      "the open replay's role arming is a seed too")
    }

    // MARK: - one spelling

    /// ONE SPELLING OF ONE PREDICATE. `BoardAnimator.isSequencing` is defined as
    /// `sequenceDepth > 0` (BoardFlight.swift), so the two are the same test -
    /// and the board used to carry both, which reads like two different rules
    /// about two different things. The question "is a sequence running" is now
    /// `isSequencing` everywhere; `sequenceDepth` is left only for the three
    /// uses that are about the NUMBER - claiming it, the nested-wait floor in
    /// `drainOtherSequences`, and printing it in a trace.
    func testTheOwnershipTestIsSpelledOneWay() throws {
        for file in ["MessageTableView.swift", "ShownLedger.swift"] {
            for line in code(try source(file)) {
                XCTAssertFalse(line.contains("sequenceDepth == 0") || line.contains("sequenceDepth > 0"),
                               "\(file): ask `BoardAnimator.isSequencing`, not the counter: "
                               + line.trimmingCharacters(in: .whitespaces))
            }
        }
        let write = code(try body(of: "write", in: try source("ShownLedger.swift")))
            .joined(separator: "\n")
        XCTAssertTrue(write.contains("BoardAnimator.isSequencing"),
                      "…and the ownership guard is a statement, not only a comment about one")
    }
}
