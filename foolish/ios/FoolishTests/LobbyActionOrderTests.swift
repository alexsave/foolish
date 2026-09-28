// A LOBBY ACTION PLAYS ITS SURFACE, THEN STAGES. NEVER THE OTHER WAY. - 1.1(69).
//
// THE REPORT, 1.1(68), owner: "do lobby animation (fade/rotate/snap) THEN
// collapse. I notice that the leave snap and the collapse also seem to happen at
// the same time." `onSend` is not a notification - it is the host's
// `stage(payload:mySeat:)`, which composes the bubble, bumps `collapseSignal`,
// asks Messages for `.compact` and does not return until that transition has
// settled. So anything written BELOW it lands after the drawer has already gone.
//
// Three of the four actions were fixed for that. JOIN was missed (U12), and
// missed silently: it is the action whose beat is no beat at all - the roster
// adopt IS the snap - so there was nothing to notice going missing except a
// snap that happened somewhere behind a closing drawer.
//
// WHY A SOURCE TEST. What is asserted is WHERE a call is written inside a
// function body. There is no value returned, no seam to call, and the functions
// are `private` on a `private` SwiftUI view - the rule is entirely about order
// in the file. Same choice, and the same reasoning, as
// FlightNamespaceInvariantTests, CountOwnershipTests and WoodHitRegionTests next
// door.
//
// AND THE LIST IS DERIVED, NOT TYPED. "Every private func that stages a bubble"
// is found by reading the file, so the next lobby action anybody writes is held
// to this the day it is written - which is the only way a rule that was missed
// once does not get missed again. The first version of this file DID type the
// list, and got it wrong (`setLobbyPassing` is a coalescing lane; the reseal is
// `stageLobbyPassing`) - the extractor's self-check is what said so.
//
// MUTATION-CHECKED, every line of it actually run: restoring joinLobby's 1.1(68)
// shape - `await onSend` above the roster, no `holdSurface` - fails
// `testEveryLobbyActionHoldsTheSurfaceBeforeItStages` AND
// `testAJoinsRosterLandsBeforeTheDrawerMoves`; replacing its `surfacePlan` call
// with `SurfacePlan.none` fails `testEveryLobbyActionAsksTheKernelWhatToPlay`;
// putting the revert back on a bare `.delta`/`.newGame` choice fails
// `testTheReversalRoutesBeforeItReadsAPlan` and
// `testTheBodiesExtractedAreTheFunctionsNamed`; breaking the scanner's
// `await onSend(` token so it derives an EMPTY list fails
// `testTheScanFindsEveryLobbyActionThatExists` and nothing else - which is the
// whole reason that floor test is there.
//
// WHAT IT DOES NOT REACH: that `holdSurface` sleeps for the right length. That
// is `SurfacePlan.settle`, which is the kernel's and is pinned in C
// (anim_plan_test) and in StagedRevertTests.
import XCTest
@testable import FoolishKit

final class LobbyActionOrderTests: XCTestCase {

    /// Functions that stage a bubble and are NOT lobby actions, each for a
    /// reason that has to be said out loud rather than left to a list nobody
    /// re-reads:
    ///
    ///   * `openSeededBoard` is the rig's `dev.stage` hook (DEBUG only). It
    ///     puts a CANNED board in the transcript; there is no surface change to
    ///     play, because the board was seated a few lines above by the same
    ///     function.
    ///
    /// The lobby's "Send invite" is absent for a different reason and needs no
    /// entry: it is a closure in `expandedContent`, not a `private func`, and it
    /// re-sends the lobby UNCHANGED - nothing moved, so there is nothing to play.
    private static let notLobbyActions: Set<String> = ["openSeededBoard"]

    /// The six as of 1.1(69). Asserted as a FLOOR, so a scanner that quietly
    /// stops finding bodies cannot pass this file vacuously.
    private static let knownActions: Set<String> = [
        "createRematchLobby", "createWaiting",
        "joinLobby", "leaveLobby", "stageLobbyPassing", "startGame",
    ]

    private func source() throws -> String {
        let here = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
        let url = here.deletingLastPathComponent()
            .appendingPathComponent("FoolishKit/Messages/GameSurface.swift")
        return try String(contentsOf: url, encoding: .utf8)
    }

    /// The body of `private func <name>`, by brace depth from its opening `{`.
    ///
    /// Crude, and deliberately SELF-CHECKING: `testTheBodiesExtractedAreTheFunctionsNamed`
    /// asserts a landmark inside each one, so a body this mis-extracts fails
    /// loudly rather than quietly passing on a slice of the wrong function.
    private func body(of fn: String, in src: String) throws -> String {
        let head = try XCTUnwrap(src.range(of: "private func \(fn)("),
                                 "\(fn) is not a private func in GameSurface.swift")
        var depth = 0, started = false, out = ""
        for ch in src[head.lowerBound...] {
            if ch == "{" { depth += 1; started = true }
            if started { out.append(ch) }
            if ch == "}" {
                depth -= 1
                if started && depth == 0 { break }
            }
        }
        XCTAssertTrue(started, "no body found for \(fn)")
        return out
    }

    /// Every `private func` in the file that stages a bubble, minus the
    /// exemptions above. Read out of the source rather than typed.
    private func lobbyActions(in src: String) throws -> [(name: String, body: String)] {
        var names: [String] = []
        var cursor = src.startIndex
        while let head = src.range(of: "private func ", range: cursor..<src.endIndex) {
            cursor = head.upperBound
            let tail = src[head.upperBound...]
            guard let paren = tail.firstIndex(of: "(") else { break }
            let name = String(tail[tail.startIndex..<paren])
            // A name, not a fragment: anything with whitespace in it means the
            // scan has wandered off a declaration.
            guard !name.isEmpty,
                  name.rangeOfCharacter(from: .whitespacesAndNewlines) == nil,
                  !names.contains(name)
            else { continue }
            names.append(name)
        }
        XCTAssertGreaterThan(names.count, 20, "the private-func scan found almost nothing")
        var out: [(String, String)] = []
        for n in names where !Self.notLobbyActions.contains(n) {
            let b = try body(of: n, in: src)
            if b.contains("await onSend(") { out.append((n, b)) }
        }
        return out
    }

    /// The scan finds at least the six that exist today. Without this, a
    /// scanner that found NOTHING would pass every other test in this file.
    func testTheScanFindsEveryLobbyActionThatExists() throws {
        let found = Set(try lobbyActions(in: try source()).map(\.name))
        XCTAssertTrue(Self.knownActions.isSubset(of: found),
                      "missing: \(Self.knownActions.subtracting(found).sorted())")
    }

    /// THE RULE. The surface is played out - and the human has had the length of
    /// it to look - before the bubble that collapses the drawer is composed.
    func testEveryLobbyActionHoldsTheSurfaceBeforeItStages() throws {
        for (name, b) in try lobbyActions(in: try source()) {
            let send = try XCTUnwrap(b.range(of: "await onSend("))
            let hold = try XCTUnwrap(b.range(of: "await holdSurface("),
                                     "\(name) stages without ever holding the surface (U12)")
            XCTAssertLessThan(hold.lowerBound, send.lowerBound,
                              "\(name) stages before it has played - the collapse eats the beat")
        }
    }

    /// AND IT ASKS RATHER THAN TYPES. The length of that hold is the kernel's
    /// answer to "how long must this surface be looked at" - a number written
    /// here instead would be a second timing policy nothing compares against the
    /// first (anim_plan.h).
    func testEveryLobbyActionAsksTheKernelWhatToPlay() throws {
        for (name, b) in try lobbyActions(in: try source()) {
            XCTAssertTrue(b.contains("surfacePlan(") || b.contains("surfaceSwap("),
                          "\(name) holds a surface without asking the kernel what is on it")
        }
    }

    /// U12 ITSELF, named. The roster only moved on the line BELOW `onSend`, so
    /// the one frame the human asked to see - their name arriving in the seat -
    /// was drawn into a drawer that had already collapsed.
    func testAJoinsRosterLandsBeforeTheDrawerMoves() throws {
        let b = try body(of: "joinLobby", in: try source())
        let roster = try XCTUnwrap(b.range(of: "lobby = Lobby("),
                                   "joinLobby no longer seats anybody - body mis-extracted?")
        let send = try XCTUnwrap(b.range(of: "await onSend("))
        XCTAssertLessThan(roster.lowerBound, send.lowerBound)
    }

    /// The self-check for the extractor: each body really is the function it
    /// names, not a slice of the one before or after it.
    func testTheBodiesExtractedAreTheFunctionsNamed() throws {
        let src = try source()
        let landmarks = [
            "joinLobby": "NicknameGate.isTaken",
            "leaveLobby": "onAnnounceLeave",
            "stageLobbyPassing": "resealLobby",
            "startGame": "startFromLobby",
            "createWaiting": "chatIsDM ? 2 : 8",
            "createRematchLobby": "armRematchCarry",
            "revertStagedSurface": "StagedRevert.route(",
        ]
        for (fn, landmark) in landmarks {
            XCTAssertTrue(try body(of: fn, in: src).contains(landmark),
                          "\(fn)'s body does not contain \(landmark)")
        }
    }

    /// AND THE REVERSAL IS ROUTED, not read off a zero settle (U2). The arm that
    /// swallowed the rematch was `guard plan.settle > 0 else { return }` applied
    /// to a plan across two different games; it survives only on the arm where a
    /// zero really does mean "the same table".
    func testTheReversalRoutesBeforeItReadsAPlan() throws {
        let b = try body(of: "revertStagedSurface", in: try source())
        let route = try XCTUnwrap(b.range(of: "StagedRevert.route("),
                                  "the revert must ask which KIND of reversal this is first")
        let plan = try XCTUnwrap(b.range(of: "surfacePlan("),
                                 "the delta arm still asks for a plan")
        XCTAssertLessThan(route.lowerBound, plan.lowerBound,
                          "a plan asked across two games answers zero, which reads as a no-op")
    }
}
