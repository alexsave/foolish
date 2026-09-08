// THE DIAGNOSTIC DUMP MUST NOT PRINT THE PAYLOAD IN A SHIPPING BUILD.
//
// Owner, round 46: "can you make sure that you can't long hold on Settings to
// get the dump... if you get the dump you can get the replay code that contains
// the entire game including others cards."
//
// He is right, and the chain is short. Every MsgEnvelope carries `seed[32]`
// (c/src/msg_wire.h), repeated by every seal; `deal_rng` makes the whole deal a
// deterministic function of it ("a whole deal is a function of one seed... and
// makes it reproducible from a stored seed", c/src/deal_rng.h). So the payload
// is not a view of the game, it IS the game - every hand and the deck order.
// `diagnosticPanel` printed it as selectable hex AND as a foolish.cards/m/
// link, behind a five-second hold on the gear, in the SHIPPING build.
//
// WHY A SOURCE SCAN, again. The panel is a SwiftUI `some View` behind a gesture
// and a `#if`; there is no value to assert and no way to render it in a unit
// test with the compilation conditions of a Release build. What CAN be pinned
// is the invariant: the two lines that print payload bytes sit inside a
// the `mayDumpPayload` gate, whose release branch requires an error. That is
// the same kind of guard `NameFieldKeyboardTests`
// takes, and it is honest about being a guard rather than a proof - the actual
// proof is the Release build, which carries no `foolish.cards/m/` string.
import XCTest
@testable import FoolishKit

final class DumpLeaksTheDealTests: XCTestCase {

    private func source() throws -> [String] {
        let here = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
        return try String(contentsOf: here.deletingLastPathComponent()
            .appendingPathComponent("FoolishKit/Messages/MessagesRootView.swift"),
            encoding: .utf8).components(separatedBy: "\n")
    }

    /// Both payload printers sit behind `mayDumpPayload`, never bare.
    func testPayloadHexAndLinkAreGated() throws {
        let src = try source()
        var inGate = false, braces = 0, checked = 0
        for line in src {
            let t = line.trimmingCharacters(in: .whitespaces)
            if t.hasPrefix("if mayDumpPayload") { inGate = true; braces = 0 }
            if inGate {
                braces += line.filter { $0 == "{" }.count - line.filter { $0 == "}" }.count
                if braces <= 0 && !t.hasPrefix("if mayDumpPayload") { inGate = false }
            }
            if t.contains("Text(hex).textSelection") || t.contains("Text(u).textSelection") {
                checked += 1
                XCTAssertTrue(inGate,
                              "the dump prints the payload without the error gate, which hands a "
                              + "player the seed and so every opponent's hand: \(t)")
            }
        }
        XCTAssertEqual(checked, 2,
                       "expected exactly the hex and URL printers - the panel changed shape, re-check this test")
    }

    /// And the gate's RELEASE branch requires an error. A `#if` that returned
    /// true on both sides would pass the test above while changing nothing.
    func testTheGateRequiresAnErrorInReleaseBuilds() throws {
        let src = try source()
        guard let i = src.firstIndex(where: { $0.contains("private var mayDumpPayload") }) else {
            return XCTFail("mayDumpPayload is gone - this test needs rewriting")
        }
        let body = src[i...min(i + 8, src.count - 1)].joined(separator: "\n")
        guard let elseAt = body.range(of: "#else") else {
            return XCTFail("mayDumpPayload has no release branch")
        }
        let release = String(body[elseAt.upperBound...])
        XCTAssertTrue(release.contains("diagError"),
                      "the release branch does not require an error, so a healthy game dumps its deal")
        XCTAssertFalse(release.contains("return true"),
                       "the release branch returns true unconditionally")
    }

    /// The health report is NOT gated: it carries no game state and it is the
    /// round-16 hang tool, whose whole value is running in the shipped build.
    func testHealthReportStaysInReleaseBuilds() throws {
        let src = try source()
        guard let i = src.firstIndex(where: { $0.contains("private var healthDump") }) else {
            return XCTFail("healthDump is gone - this test needs rewriting")
        }
        let body = src[i...min(i + 25, src.count - 1)].joined(separator: "\n")
        XCTAssertFalse(body.contains("#if DEBUG"),
                       "healthDump must stay in release - it is the hang diagnostic and holds no cards")
        XCTAssertTrue(body.contains("FlightRecorder.report"))
    }
}
