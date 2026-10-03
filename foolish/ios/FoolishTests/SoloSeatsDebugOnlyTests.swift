// THE SOLO ROW AND ITS DEV FLAG ARE DEBUG-ONLY, AND STAY THAT WAY.
//
// The lobby's "Add player (testing)" / solo Start row exists so one device can
// reach a startable game. It bypasses the kernel's authorship gate on purpose,
// which is exactly why it must never ship: in a real chat it would hand Start
// to whoever has two names in the roster. `lobby.soloseats=0` in `dev.flags`
// turns it off on a debug build so the rig can film the lobby a shipping build
// shows - and that read goes through `MessageDevBoard`, which only exists in a
// debug build.
//
// WHY A SOURCE SCAN. The property under test is "this code is not compiled
// into Release", and a unit test only ever runs a DEBUG build, where all of it
// is present. What can be pinned is the shape that makes it true: every
// mention of the flag key and of `MessageDebugFlags.soloSeats` sits inside an
// `#if DEBUG` (or `#if DEBUG || SOLO_TESTING`) region, and the two functions
// that hand the lobby its solo row answer "no" on their release branch. The
// shipping-scheme build that `mac_tests.sh` runs is the proof that the release
// branches compile; this is the guard that a read cannot quietly move out.
//
// WHY NOT A BEHAVIOURAL TEST OF THE FLAG. `MessageDevBoard.devFlags` is a
// process-wide `static let` read once from the App Group the rig also writes,
// so a test can only set it by writing the rig's `dev.flags` before anything
// else in the test process touches the board - racy, and it clobbers a live
// rig. The scan below covers the reads instead.
import XCTest

final class SoloSeatsDebugOnlyTests: XCTestCase {

    // MARK: - the scanner

    /// `foolish/` - three directories up from this file.
    private static var productRoot: URL {
        URL(fileURLWithPath: #filePath).deletingLastPathComponent()   // FoolishTests
                                      .deletingLastPathComponent()    // ios
                                      .deletingLastPathComponent()    // foolish
    }

    /// Every Swift file that can end up in a shipping product: all of `ios/`
    /// except the two test targets, plus the shared Swift the products link.
    private static func shippingSources() -> [URL] {
        let roots = [productRoot.appendingPathComponent("ios"),
                     productRoot.deletingLastPathComponent().appendingPathComponent("shared/swift")]
        let skip: Set<String> = ["FoolishTests", "HarnessTests", "Foolish.xcodeproj",
                                 ".build", "DerivedData", "build"]
        var out: [URL] = []
        for root in roots {
            guard let walk = FileManager.default.enumerator(
                at: root, includingPropertiesForKeys: [.isDirectoryKey]) else { continue }
            for case let url as URL in walk {
                if skip.contains(url.lastPathComponent) { walk.skipDescendants(); continue }
                if url.pathExtension == "swift" { out.append(url) }
            }
        }
        return out.sorted { $0.path < $1.path }
    }

    /// Is a `#if` / `#elseif` condition true ONLY in a debug build? Each
    /// `||` alternative must itself require DEBUG or SOLO_TESTING (as one of
    /// its `&&` terms), or the region can compile into Release.
    static func isDebugOnly(_ cond: String) -> Bool {
        let c = cond.replacingOccurrences(of: "(", with: " ")
                    .replacingOccurrences(of: ")", with: " ")
        let alts = c.components(separatedBy: "||")
        return !alts.isEmpty && alts.allSatisfy { alt in
            alt.components(separatedBy: "&&").contains {
                let t = $0.trimmingCharacters(in: .whitespaces)
                return t == "DEBUG" || t == "SOLO_TESTING"
            }
        }
    }

    struct Line { let n: Int; let code: String; let debugOnly: Bool }

    /// The file as code lines (comments stripped), each tagged with whether it
    /// sits in a debug-only region. A `#else` branch is never debug-only: it is
    /// the branch a build takes when the conditions above it are false.
    static func scan(_ text: String, file: String) -> [Line] {
        var stack: [Bool] = []
        var out: [Line] = []
        for (i, raw) in text.components(separatedBy: "\n").enumerated() {
            var code = raw
            if let r = code.range(of: "//") { code = String(code[..<r.lowerBound]) }
            let t = code.trimmingCharacters(in: .whitespaces)
            if t.hasPrefix("#if ") {
                stack.append(isDebugOnly(String(t.dropFirst(4))))
            } else if t.hasPrefix("#elseif ") {
                if stack.isEmpty { XCTFail("\(file):\(i + 1) #elseif with no #if"); return out }
                stack[stack.count - 1] = isDebugOnly(String(t.dropFirst(8)))
            } else if t == "#else" {
                if stack.isEmpty { XCTFail("\(file):\(i + 1) #else with no #if"); return out }
                stack[stack.count - 1] = false
            } else if t == "#endif" {
                if stack.isEmpty { XCTFail("\(file):\(i + 1) #endif with no #if"); return out }
                stack.removeLast()
            } else {
                out.append(Line(n: i + 1, code: code, debugOnly: stack.contains(true)))
            }
        }
        XCTAssertTrue(stack.isEmpty, "\(file): an #if is never closed")
        return out
    }

    private static func scanned(_ url: URL) throws -> [Line] {
        scan(try String(contentsOf: url, encoding: .utf8), file: url.lastPathComponent)
    }

    private static func source(_ rel: String) throws -> [Line] {
        let url = productRoot.appendingPathComponent(rel)
        guard FileManager.default.fileExists(atPath: url.path) else {
            XCTFail("\(rel) is gone - this test needs its path updated")
            return []
        }
        return try scanned(url)
    }

    // MARK: - the scanner itself

    /// The scanner is the whole test, so it gets its own: a release branch, a
    /// non-debug `#if`, and an `||` with a non-debug term are all SHIPPING.
    func testScannerTellsDebugRegionsFromShippingOnes() {
        let src = """
        a
        #if DEBUG || SOLO_TESTING
        b
        #if os(iOS)
        c
        #endif
        #else
        d
        #endif
        #if DEBUG || RIG
        e
        #elseif DEBUG
        f
        #endif
        #if SOLO_TESTING && RIG_RESEED
        g // h
        #endif
        """
        let tags = Dictionary(uniqueKeysWithValues: Self.scan(src, file: "fixture").map {
            ($0.code.trimmingCharacters(in: .whitespaces), $0.debugOnly)
        })
        XCTAssertEqual(tags, ["a": false, "b": true, "c": true, "d": false,
                              "e": false, "f": true, "g": true])
    }

    // MARK: - (2) no shipping path reads the flag

    /// Every reference to `lobby.soloseats` and to `MessageDebugFlags.soloSeats`,
    /// in every Swift file a product can link, is inside a debug-only region.
    func testSoloSeatsIsReadOnlyInsideDebugRegions() throws {
        var seen = ["\"lobby.soloseats\"": 0, "MessageDebugFlags.soloSeats": 0]
        for url in Self.shippingSources() {
            for line in try Self.scanned(url) {
                for needle in seen.keys where line.code.contains(needle) {
                    seen[needle]! += 1
                    XCTAssertTrue(line.debugOnly,
                                  "\(url.lastPathComponent):\(line.n) reads \(needle) outside "
                                  + "#if DEBUG, so a shipping build can offer the solo row: "
                                  + line.code.trimmingCharacters(in: .whitespaces))
                }
            }
        }
        // A scan that found nothing proves nothing: the read moved or was renamed.
        for (needle, n) in seen {
            XCTAssertGreaterThan(n, 0, "no code mentions \(needle) any more - re-aim this test")
        }
    }

    /// The types those reads go through exist only in a debug build, so a
    /// read that escaped its `#if` would also fail to compile in Release.
    func testFlagTypesAreDebugOnly() throws {
        let decls: [(String, [String])] = [
            ("ios/FoolishKit/Messages/MessageDevBoard.swift",
             ["public enum MessageDevBoard", "public static func flag(", "\"dev.flags\""]),
            ("ios/FoolishKit/Messages/MessageDebugFlags.swift",
             ["public enum MessageDebugFlags", "public static var soloSeats"]),
        ]
        for (file, needles) in decls {
            let lines = try Self.source(file)
            for needle in needles {
                let hits = lines.filter { $0.code.contains(needle) }
                XCTAssertFalse(hits.isEmpty, "\(file) no longer declares \(needle)")
                for h in hits {
                    XCTAssertTrue(h.debugOnly, "\(file):\(h.n) \(needle) is outside #if DEBUG")
                }
            }
        }
    }

    // MARK: - (1) the solo row exists only in a debug build

    /// The two switches that put the solo row on screen both say NO on their
    /// release branch: the lobby's `soloSeatsEnabled` returns false and the
    /// surface's `soloSeatAction` hands the lobby no action.
    func testSoloRowSwitchesAreOffInRelease() throws {
        try assertReleaseBranch(file: "ios/FoolishKit/Messages/LobbyScreens.swift",
                                decl: "private var soloSeatsEnabled: Bool",
                                debugMust: "MessageDevBoard.flag(\"lobby.soloseats\"",
                                releaseIs: "return false")
        try assertReleaseBranch(file: "ios/FoolishKit/Messages/GameSurface.swift",
                                decl: "private func soloSeatAction(",
                                debugMust: "addSoloSeat(",
                                releaseIs: "return nil")
    }

    /// And the row is drawn only behind `soloSeatsEnabled`: every call to
    /// `soloControls` is under an `if` that checks it.
    func testSoloControlsAreDrawnOnlyBehindTheSwitch() throws {
        let lines = try Self.source("ios/FoolishKit/Messages/LobbyScreens.swift")
        let calls = lines.indices.filter {
            lines[$0].code.contains("soloControls(") && !lines[$0].code.contains("func soloControls(")
        }
        XCTAssertFalse(calls.isEmpty, "the lobby no longer draws soloControls - re-aim this test")
        for i in calls {
            let guardLine = i > 0 ? lines[i - 1].code : ""
            XCTAssertTrue(guardLine.contains("if ") && guardLine.contains("soloSeatsEnabled"),
                          "LobbyScreens.swift:\(lines[i].n) draws the solo row without "
                          + "checking soloSeatsEnabled")
        }
    }

    /// No build configuration this product ships defines either condition.
    func testNoConfigDefinesTheDebugConditionsForRelease() throws {
        for rel in ["ios/Config/Release.xcconfig", "ios/Config/Base.xcconfig", "ios/project.yml"] {
            let text = try String(contentsOf: Self.productRoot.appendingPathComponent(rel),
                                  encoding: .utf8)
            XCTAssertFalse(text.contains("SOLO_TESTING"),
                           "\(rel) defines SOLO_TESTING, which compiles the solo row in")
        }
    }

    /// `decl`'s body is an `#if DEBUG...#else...#endif` whose debug branch
    /// mentions `debugMust` and whose release branch is exactly `releaseIs`.
    private func assertReleaseBranch(file: String, decl: String, debugMust: String,
                                     releaseIs: String) throws {
        let raw = try String(contentsOf: Self.productRoot.appendingPathComponent(file),
                             encoding: .utf8).components(separatedBy: "\n")
        guard let at = raw.firstIndex(where: { $0.contains(decl) }) else {
            return XCTFail("\(file) no longer has \(decl) - re-aim this test")
        }
        let body = raw[at...].prefix(while: { $0.trimmingCharacters(in: .whitespaces) != "}" })
                             .map { $0.trimmingCharacters(in: .whitespaces) }
        guard let ifAt = body.firstIndex(where: { $0.hasPrefix("#if ") }),
              Self.isDebugOnly(String(body[ifAt].dropFirst(4))),
              let elseAt = body.firstIndex(of: "#else"),
              let endAt = body.firstIndex(of: "#endif"), ifAt < elseAt, elseAt < endAt else {
            return XCTFail("\(file): \(decl) is not an #if DEBUG / #else / #endif any more")
        }
        XCTAssertTrue(body[ifAt..<elseAt].contains { $0.contains(debugMust) },
                      "\(file): \(decl)'s debug branch no longer reads \(debugMust)")
        XCTAssertEqual(Array(body[(elseAt + 1)..<endAt]), [releaseIs],
                       "\(file): \(decl)'s release branch is not just `\(releaseIs)`")
    }
}
