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
// MUTATION-CHECKED: deleting either guard fails its own test here, and reverting
// the rig run reproduces the 6-5-6-5 timeline.

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
