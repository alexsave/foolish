// A BOARD EITHER VEILS CARDS OR SHARES A NAMESPACE. NEVER BOTH. - round 43.
//
// This is the statement that replaced two guards, and it is the reason both
// could go. The guards were `hidden.contains(id) ? nil : namespace`, written
// once in FHandFan and once in FBattleGrid by round-7 #2, to stop SwiftUI
// flying a card into the hand with matchedGeometry while `BoardAnimator`'s
// overlay was already flying it there - the "double animation" for pickup.
//
// The RULE is real and still load-bearing. The per-card TEST for it was not:
//
//   * the message board veils cards (`hidden:` is a live set) and passed
//     `namespace: cardNS`, where `cardNS` was a computed property returning
//     nil. So the ternary always chose nil.
//   * the offline board shares a real `@Namespace` and passes no `hidden:` at
//     all, so the set is empty and the ternary always chose the namespace.
//
// Neither caller could produce the combination the guard existed to handle. So
// three lines of nil-passing ceremony and two per-card branches collapse into
// one sentence, stated at `FlightID` and pinned here.
//
// WHY A SOURCE TEST. What is being asserted is which ARGUMENTS a SwiftUI view
// hands its children - there is no value returned and no seam to call, and the
// rule is entirely about where it is written. Same choice, and the same
// reasoning, as CountOwnershipTests and WoodHitRegionTests next door.
//
// MUTATION-CHECKED: giving MessageTableView a real `@Namespace` and passing it
// fails `testTheVeilingBoardSharesNoNamespace`; passing a `hidden:` set from
// the offline TableView fails `testTheNamespaceBoardVeilsNothing`. Restoring
// the old ternary fails `testTheGuardIsNotWrittenPerCardAnyMore`.

import XCTest
@testable import FoolishKit

final class FlightNamespaceInvariantTests: XCTestCase {

    private func source(_ path: String) throws -> String {
        let here = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
        let url = here.deletingLastPathComponent().appendingPathComponent(path)
        return try String(contentsOf: url, encoding: .utf8)
    }

    /// The arguments of every `FHandFan(` / `FBattleGrid(` construction in a
    /// file, as one blob per call - crude, but these are flat call sites and it
    /// is enough to tell "passes `namespace:`" from "mentions the word".
    private func calls(to view: String, in src: String) -> [String] {
        var out: [String] = []
        var rest = Substring(src)
        while let head = rest.range(of: "\(view)(") {
            var depth = 0, started = false, blob = ""
            for ch in rest[head.lowerBound...] {
                blob.append(ch)
                if ch == "(" { depth += 1; started = true }
                if ch == ")" { depth -= 1; if started && depth == 0 { break } }
            }
            out.append(blob)
            rest = rest[head.upperBound...]
        }
        return out
    }

    // MARK: the two halves of the invariant

    /// The message board veils cards, so it may not share a namespace.
    ///
    /// A namespace here would fly every card twice - once by the overlay, once
    /// by SwiftUI - and the cross-fade between the two copies is an opacity
    /// animation on a card, which the owner's standing rule forbids outright
    /// ("we should NEVER fade cards in this game. Real life cards don't ever
    /// fade like that! EVER!").
    func testTheVeilingBoardSharesNoNamespace() throws {
        let src = try source("FoolishKit/Boards/MessageTableView.swift")
        let sites = calls(to: "FHandFan", in: src) + calls(to: "FBattleGrid", in: src)
        XCTAssertFalse(sites.isEmpty, "the board must build a hand and a grid")
        for site in sites {
            XCTAssertTrue(site.contains("hidden:"),
                          "this board veils cards - every call site passes `hidden:`")
            XCTAssertFalse(site.contains("namespace:"),
                           "a veiling board may not share a matchedGeometry namespace: "
                           + "the overlay already owns every flight, so SwiftUI would fly "
                           + "each card a second time and cross-fade the two copies")
        }
        // In CODE, not in prose - the comment where `cardNS` used to be names
        // the type on purpose, so that whoever wonders where it went can find
        // out. (This assertion first failed on that very comment, which is the
        // right failure for the wrong reason.)
        let code = src.split(separator: "\n", omittingEmptySubsequences: false)
            .filter { !$0.trimmingCharacters(in: .whitespaces).hasPrefix("//") }
            .joined(separator: "\n")
        XCTAssertFalse(code.contains("Namespace.ID"),
                       "the board should not declare or pass a namespace any more")
    }

    /// …and the mirror. The offline board's flights ARE matchedGeometry, so it
    /// shares a namespace - and therefore must veil nothing, or it would be
    /// asking for the same double animation from the other direction.
    func testTheNamespaceBoardVeilsNothing() throws {
        let src = try source("FoolishKit/Boards/TableView.swift")
        let sites = calls(to: "FHandFan", in: src) + calls(to: "FBattleGrid", in: src)
        XCTAssertFalse(sites.isEmpty, "the offline board must build a hand and a grid")
        for site in sites {
            XCTAssertTrue(site.contains("namespace:"),
                          "the offline board's flights are matchedGeometry")
            XCTAssertFalse(site.contains("hidden:"),
                           "a namespace board may not also veil cards - that is the "
                           + "combination the deleted per-card guard existed for")
        }
    }

    /// And the collapse itself: the rule is stated once, not re-derived per card.
    func testTheGuardIsNotWrittenPerCardAnyMore() throws {
        for path in ["FoolishKit/DesignSystem/FHandFan.swift",
                     "FoolishKit/Boards/FBattleGrid.swift"] {
            let src = try source(path)
            // Comments explain the history and are allowed to quote it; code is
            // not. Strip whole-line comments before looking.
            let code = src.split(separator: "\n", omittingEmptySubsequences: false)
                .filter { !$0.trimmingCharacters(in: .whitespaces).hasPrefix("//") }
                .joined(separator: "\n")
            XCTAssertFalse(code.contains("? nil : namespace"),
                           "\(path) re-derives the namespace rule per card. It belongs "
                           + "once, at FlightID - and it cannot fire here, because a "
                           + "board that veils passes no namespace and a board with a "
                           + "namespace veils nothing (the other two tests).")
        }
    }

    /// The statement itself has to survive, or the two tests above are pinning
    /// a rule nobody can find the reason for.
    func testTheInvariantIsWrittenDownWhereTheEffectIsApplied() throws {
        let src = try source("FoolishKit/Boards/BoardDrag.swift")
        XCTAssertTrue(src.contains("NEVER BOTH"),
                      "FlightID carries the invariant that lets the guards go")
    }
}
