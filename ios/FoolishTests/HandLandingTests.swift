// WHERE A CARD LANDS IN MY HAND - the order of the three answers, round 43.
//
// `handLanding` collapses a chain that was written out verbatim at four call
// sites: the live draw, the replayed deal, the replayed pickup, and the resting
// ghost a play leaves behind. Four copies is four chances to get the order
// wrong, and the ORDER is the entire content of the thing:
//
//   1. the ANALYTICAL slot, computed off the fan's own geometry
//   2. the PUBLISHED frame, only when the analytical route cannot answer
//   3. a ROUGH SPREAD, staggered by index
//
// Put the published frame first and every flight aims at a moving target. That
// is round 7's finding: the make-room and the arrival are meant to play at the
// same time, so the fan is still re-centring while the card is in the air, and
// the published preference describes where the hand USED to be going. The
// analytical slot is the settled answer from the first instant, which is why it
// has to be asked first. Round 12 is the mirror: read the slots off the kernel
// hand rather than the DISPLAY order and every dealt card flies to the slot it
// would have had in an unsorted hand, then snaps.
//
// Drop rung 3 and an unresolved card silently appears in the hand instead of
// flying - that is round-7 #1's "it just suddenly appears in hand", and the
// stagger is what stops several of them piling onto one point and snapping
// apart afterwards.
//
// WHY A SOURCE TEST. `handLanding` reads `handFrame` and `handCardFrames`,
// which are `@State` on a SwiftUI view - there is no seam to call it through
// and no value it returns without a laid-out board. What can be checked is that
// the rule is written once and in the right order, which is exactly what went
// wrong when it was written four times. Same technique, and the same reason, as
// CountOwnershipTests next door.
//
// MUTATION-CHECKED: swapping rungs 1 and 2 fails `testTheLandingChainAsksInOrder`;
// deleting rung 3 fails it; re-inlining the chain at a call site fails
// `testNoCallSiteReDerivesTheLandingChain`.

import XCTest
@testable import FoolishKit

final class HandLandingTests: XCTestCase {

    private func source() throws -> String {
        let here = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
        return try String(contentsOf: here.deletingLastPathComponent()
            .appendingPathComponent("FoolishKit/Boards/MessageTableView.swift"), encoding: .utf8)
    }

    /// Lines of `src` that are code, not comment - these files carry more prose
    /// than code, and every rung below is also NAMED in the prose around it.
    private func code(_ src: String) -> String {
        src.split(separator: "\n", omittingEmptySubsequences: false)
            .filter { !$0.trimmingCharacters(in: .whitespaces).hasPrefix("//") }
            .joined(separator: "\n")
    }

    private func body(of fn: String, in src: String) throws -> String {
        let head = try XCTUnwrap(src.range(of: "private func \(fn)("), "no func \(fn)")
        var depth = 0, started = false, out = ""
        for ch in src[head.lowerBound...] {
            out.append(ch)
            if ch == "{" { depth += 1; started = true }
            if ch == "}" { depth -= 1; if started && depth == 0 { break } }
        }
        return out
    }

    /// The three rungs, in the one order that works.
    func testTheLandingChainAsksInOrder() throws {
        let b = code(try body(of: "handLanding", in: try source()))
        let slot = try XCTUnwrap(b.range(of: "handLandingSlot("), "rung 1: the analytical slot")
        let published = try XCTUnwrap(b.range(of: "handCardFrames["), "rung 2: the published frame")
        let approx = try XCTUnwrap(b.range(of: "handApproxLanding("), "rung 3: the rough spread")

        XCTAssertTrue(slot.lowerBound < published.lowerBound,
                      "the ANALYTICAL slot must be asked before the published frame. A "
                      + "preference lags a layout pass, and the fan is still re-centring "
                      + "while the card is in the air, so asking it first aims the flight "
                      + "at where the hand used to be going (round 7).")
        XCTAssertTrue(published.lowerBound < approx.lowerBound,
                      "the rough spread is the LAST resort, not the second")
    }

    /// Rung 3 spreads by index. Without that, several unresolved cards fly to
    /// one point and snap apart on arrival (round-7's first-open "bunch").
    func testTheRoughSpreadIsStaggered() throws {
        let b = code(try body(of: "handLanding", in: try source()))
        XCTAssertTrue(b.contains("handApproxLanding(index:"),
                      "the fallback must be spread by index, or unresolved cards pile up")
    }

    /// …and nobody re-derives it. One rule, one place - that is the whole point
    /// of collapsing four copies.
    ///
    /// `flyUndoReturn` is the one site that deliberately does NOT use the chain
    /// and is exempt: cards coming BACK into the hand have no published frame to
    /// find, so its middle rung would answer nil every time, and it gates the
    /// rough spread on `lastChance` so it polls for the exact slot first. That
    /// is a different rule, written down at the site.
    func testNoCallSiteReDerivesTheLandingChain() throws {
        // Everything EXCEPT `handLanding`'s own body, which is the one place
        // the chain belongs. Scanning the whole file matches the definition and
        // reports the rule as a violation of itself - which is what the first
        // version of this test did.
        let whole = code(try source())
        let mine = code(try body(of: "handLanding", in: try source()))
        let src = whole.replacingOccurrences(of: mine, with: "")
        let inlined = src.components(separatedBy: "handLandingSlot(").dropFirst()
            .filter { $0.prefix(120).contains("handCardFrames[") }
        XCTAssertTrue(inlined.isEmpty,
                      "a call site re-derives the landing chain by hand. Use `handLanding` - "
                      + "the order of its rungs is the whole content of the rule, and four "
                      + "hand-written copies is four chances to get it wrong.")
    }
}
