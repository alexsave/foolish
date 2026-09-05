// THE FROZEN COUNTS BELONG TO WHOEVER IS ANIMATING - round 42.
//
// A running sequence freezes the seat badges, the deck and the discard to the
// board BEFORE its move and walks them forward one step per landing flight
// (`runEventStream`). A LIVE PLAY of mine that lands in the middle of that used
// to write over the same three pieces of state twice: `freezeCounts` re-froze
// them to a board several steps further on, and `releaseCounts` then cleared
// them outright. The stream's next step put them back.
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
// WHY A SOURCE TEST. Both are `private func`s over `@State` on a SwiftUI view;
// there is no seam to call them through and no value they return. The rule they
// have to keep is one line long and entirely about WHERE it is written, which is
// exactly what this can check - the same choice, for the same reason, as
// WoodHitRegionTests. The behaviour itself is pinned by the rig measurement
// above, which is reproducible from the command line in that header.
//
// ROUND 43 FOUND A THIRD WRITER. `releaseLivePlayVeil` - the one path for a play
// that comes to nothing - made the very same writes with no guard at all, so
// round 42's fix was undone one line after it was applied: `play` deferred the
// freeze (correctly) and then released what it had just declined to touch. Its
// bluntest reproduction is a tap during a red conflict retraction, because
// `MessageTurnController.apply` returns false for the whole of that window and
// `flyUndoReturn` holds it open at `sequenceDepth >= 1`. The three tests below
// the round-42 pair pin that, plus the half that must STAY unconditional (the
// refused play's own cards) and the single spelling of the predicate.
//
// MUTATION-CHECKED: deleting any of the three guards fails its own test here
// (verified for all three), and reverting the rig run reproduces the 6-5-6-5
// timeline.

import XCTest
@testable import FoolishKit

final class CountOwnershipTests: XCTestCase {

    private func source(_ name: String) throws -> String {
        // #filePath is this file; the board sits two directories over.
        let here = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
        let url = here.deletingLastPathComponent()
            .appendingPathComponent("FoolishKit/Boards/\(name)")
        return try String(contentsOf: url, encoding: .utf8)
    }

    /// The body of a `func <name>(` up to the matching close - crude, but the
    /// two functions under test are short and flat, and a brace counter is
    /// enough to keep this from reading the whole file and passing on somebody
    /// else's guard.
    private func body(of fn: String, in src: String) throws -> String {
        let head = try XCTUnwrap(src.range(of: "func \(fn)("), "no func \(fn)")
        var depth = 0, started = false
        var out = ""
        for ch in src[head.lowerBound...] {
            out.append(ch)
            if ch == "{" { depth += 1; started = true }
            if ch == "}" { depth -= 1; if started && depth == 0 { break } }
        }
        return out
    }

    /// THE RULE, both halves. Neither may touch the count overrides while a
    /// sequence is running: the stream that froze them is still walking them
    /// forward and its teardown is still coming.
    func testNeitherFreezeNorReleaseTouchesTheCountsWhileASequenceRuns() throws {
        let src = try source("MessageTableView.swift")
        for fn in ["freezeCounts", "releaseCounts"] {
            let b = try body(of: fn, in: src)
            XCTAssertTrue(b.contains("BoardAnimator.sequenceDepth == 0"),
                          "\(fn) must stand down while a sequence owns the counts")
            // …and stand down BEFORE writing, not after. A guard below the first
            // assignment would have already done the damage.
            let guardAt = try XCTUnwrap(b.range(of: "BoardAnimator.sequenceDepth == 0")).lowerBound
            for write in ["deckCountOverride", "seatCountOverride", "discardCountOverride"] {
                if let w = b.range(of: write)?.lowerBound {
                    XCTAssertLessThan(guardAt, w,
                                      "\(fn) writes \(write) before checking who owns it")
                }
            }
        }
    }

    /// Brace depth of the first `needle` inside `src`, counting the function's
    /// own opening brace as depth 1. This is how "unconditionally" is spelled in
    /// a source test: a write at depth 1 runs on every call, a write at depth 2
    /// is inside the ownership `if`. Returns nil when the needle is absent.
    private func depth(of needle: String, in src: String) -> Int? {
        guard let at = src.range(of: needle)?.lowerBound else { return nil }
        var d = 0
        for ch in src[..<at] {
            if ch == "{" { d += 1 }
            if ch == "}" { d -= 1 }
        }
        return d
    }

    /// ROUND 43, THE THIRD WRITER. `releaseLivePlayVeil` performed the very same
    /// writes as `releaseCounts` with no guard at all, and all three of its call
    /// sites reach it mid-sequence - most bluntly `apply` returning false, which
    /// it does for the WHOLE of a red conflict retraction
    /// (`MessageTurnController.conflictRetracting`), a window `flyUndoReturn`
    /// holds open at `sequenceDepth >= 1`. So a tap during a retraction had
    /// `play` correctly defer the freeze and then release it one line later:
    /// badges forward to the retracted base, back again on the arrival's replay.
    ///
    /// Note this asserts MORE than the two above: `roleShown` and `outShown` are
    /// in the same ownership (nilling `roleShown` mid-sequence leaves `syncRoles`
    /// with nothing to hand over, so the shield teleports).
    func testReleaseLivePlayVeilDefersToARunningSequenceToo() throws {
        let src = try source("MessageTableView.swift")
        let b = try body(of: "releaseLivePlayVeil", in: src)
        XCTAssertTrue(b.contains("BoardAnimator.sequenceDepth == 0"),
                      "releaseLivePlayVeil must stand down while a sequence owns the counts")
        for write in ["deckCountOverride", "discardCountOverride", "seatCountOverride",
                      "roleShown = nil", "outShown = nil", "clearSweep()"] {
            XCTAssertEqual(depth(of: write, in: b), 2,
                           "\(write) must sit INSIDE the ownership check, not run on every call")
        }
    }

    /// …and the other half of the same rule, which a guard placed too high would
    /// break just as badly: the cards THIS play hid are its own property. No
    /// sequence knows about them and nothing else will ever hand them back, so a
    /// refusal that lands mid-flight must still reveal them or they are gone
    /// from the fan for the life of the board (that is round 40's leak, and it
    /// must not be re-opened in the name of round 43).
    func testTheRefusedPlacementIsStillGivenBackUnconditionally() throws {
        let src = try source("MessageTableView.swift")
        let b = try body(of: "releaseLivePlayVeil", in: src)
        for write in ["handBeforeMyMove = nil", "pendingPlacement = nil",
                      "animator.cancelHeld(ids)", "animator.reveal(ids)"] {
            XCTAssertEqual(depth(of: write, in: b), 1,
                           "\(write) belongs to this play alone and must run on every call")
        }
    }

    /// One spelling of one predicate. `BoardAnimator.isSequencing` is defined as
    /// `sequenceDepth > 0` (BoardFlight.swift), so `!isSequencing` and
    /// `sequenceDepth == 0` are the same test - and this function used to carry
    /// both, which reads like two different rules about two different things.
    func testTheOwnershipTestIsSpelledOneWay() throws {
        let src = try source("MessageTableView.swift")
        // CODE ONLY. The body's own comment explains the equivalence by naming
        // `BoardAnimator.isSequencing`, and a test that cannot tell a comment
        // from a statement would fail on the very sentence that documents it.
        let code = try body(of: "releaseLivePlayVeil", in: src)
            .split(separator: "\n", omittingEmptySubsequences: false)
            .filter { !$0.trimmingCharacters(in: .whitespaces).hasPrefix("//") }
            .joined(separator: "\n")
        XCTAssertFalse(code.contains("isSequencing"),
                       "use `sequenceDepth == 0`, the spelling the other two guards use")
        XCTAssertTrue(code.contains("BoardAnimator.sequenceDepth == 0"),
                      "…and the guard is a statement, not only a comment about one")
    }

    /// The counts are a SEQUENCE's property, so exactly one place may hand them
    /// back unconditionally: `runEventStream`'s teardown, and only on the branch
    /// that has established it is the newest sequence. Pinned because the whole
    /// fix above rests on that release still happening - a board whose badges
    /// never come back off their overrides is frozen for good, which is a worse
    /// defect than the twitch.
    func testTheStreamTeardownStillReleasesThemUnconditionally() throws {
        let src = try source("MessageTableView.swift")
        let teardown = try XCTUnwrap(src.range(of: "if mySeq == animSequenceToken {"))
        let after = String(src[teardown.upperBound...].prefix(600))
        XCTAssertTrue(after.contains("deckCountOverride = nil"),
                      "the newest sequence's teardown must still hand the counts back")
        XCTAssertTrue(after.contains("seatCountOverride = [:]"))
        XCTAssertFalse(after.contains("BoardAnimator.sequenceDepth == 0"),
                       "the teardown runs INSIDE its own sequence - guarding it there "
                       + "would mean the counts are never released at all")
    }
}
