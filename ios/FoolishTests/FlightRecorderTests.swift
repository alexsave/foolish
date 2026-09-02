// The flight recorder has to work on the run where everything else did not.
//
// The owner: "sometimes it just hangs. Even on newer devices. Can't tell why...
// if there's a crash or something make it appear as a diagnostic dump in the UI
// so I can check next time it happens."
//
// Which makes this a diagnostic that is only ever read after a failure, and
// therefore one whose own failures are invisible: a recorder that silently
// writes nothing produces an empty panel, and an empty panel reads as "nothing
// went wrong". So the properties pinned here are the ones nobody would notice
// breaking - that an ABANDONED session is reported as abandoned, that the trail
// survives into the next session rather than being overwritten by it, and that
// the verdict does not claim more than the evidence supports.
import XCTest
@testable import FoolishKit

final class FlightRecorderTests: XCTestCase {

    private var dir: URL!

    override func setUp() {
        super.setUp()
        dir = FileManager.default.temporaryDirectory
            .appendingPathComponent("flight-\(UUID().uuidString)")
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        FlightRecorder.directoryOverride = dir
        FlightRecorder.reset()
    }

    override func tearDown() {
        FlightRecorder.reset()
        FlightRecorder.directoryOverride = nil
        try? FileManager.default.removeItem(at: dir)
        super.tearDown()
    }

    /// Force the async breadcrumb queue to drain - `note` is deliberately
    /// asynchronous so it never blocks a move, which means a test that reads
    /// immediately reads too early.
    private func settle() {
        let e = expectation(description: "drained")
        DispatchQueue(label: "t").asyncAfter(deadline: .now() + 0.15) { e.fulfill() }
        wait(for: [e], timeout: 2)
    }

    /// THE CASE THE FEATURE EXISTS FOR. A session that is killed writes no
    /// goodbye line, and the NEXT launch must find its trail intact and know it
    /// ended badly. Both halves matter: the rotation has to preserve the dead
    /// session rather than the live one clobbering it.
    func testAKilledSessionIsRecoveredAndReportedAsAbrupt() {
        FlightRecorder.begin("first")
        FlightRecorder.note("adopt", "turn 12")
        FlightRecorder.note("seal", "1 staged")
        settle()
        FlightRecorder.abandonForTesting()      // …killed here. No `end`.

        FlightRecorder.begin("second")           // the next launch
        settle()
        let prev = try? XCTUnwrap(FlightRecorder.previousSession())
        let p = try! XCTUnwrap(prev)
        XCTAssertFalse(p.endedCleanly, "a killed session reported itself as closed")
        XCTAssertEqual(p.notes.map(\.event), ["begin", "adopt", "seal"],
                       "the dead session's trail did not survive into the next launch")
        XCTAssertEqual(p.notes.first { $0.event == "adopt" }?.detail, "turn 12",
                       "the detail - what it was doing - is the whole point")
    }

    /// …and a session that closes properly is NOT reported as a failure. Without
    /// this the banner fires on every ordinary dismissal and the owner learns to
    /// ignore it, which costs more than having no banner at all.
    func testACleanlyClosedSessionIsNotAlarming() {
        FlightRecorder.begin("first")
        FlightRecorder.note("active")
        FlightRecorder.end("resigned")
        FlightRecorder.abandonForTesting()

        FlightRecorder.begin("second")
        settle()
        let p = try! XCTUnwrap(FlightRecorder.previousSession())
        XCTAssertTrue(p.endedCleanly)
        XCTAssertFalse(FlightRecorder.isAlarming(p))
        XCTAssertTrue(FlightRecorder.verdict(p).contains("closed normally"))
    }

    /// AN ABRUPT END ALONE IS NOT ALARMING. Messages tears the extension down
    /// whenever it likes, so most abrupt ends are ordinary. It becomes alarming
    /// when a memory warning led up to it - which is the memory-kill signature,
    /// and the only form of evidence a SIGKILL leaves behind.
    func testAnAbruptEndIsOnlyAlarmingWhenSomethingLedUpToIt() {
        FlightRecorder.begin("first")
        FlightRecorder.note("active")
        settle()
        FlightRecorder.abandonForTesting()
        FlightRecorder.begin("second")
        settle()
        let plain = try! XCTUnwrap(FlightRecorder.previousSession())
        XCTAssertFalse(plain.endedCleanly)
        XCTAssertFalse(FlightRecorder.isAlarming(plain),
                       "an ordinary teardown must not raise the banner")
        XCTAssertTrue(FlightRecorder.verdict(plain).contains("ended abruptly"))

        FlightRecorder.abandonForTesting()
        FlightRecorder.begin("third")
        FlightRecorder.note("memory-warning", "dropped 3 textures")
        settle()
        FlightRecorder.abandonForTesting()
        FlightRecorder.begin("fourth")
        settle()
        let starved = try! XCTUnwrap(FlightRecorder.previousSession())
        XCTAssertTrue(FlightRecorder.isAlarming(starved),
                      "an abrupt end after a memory warning is the memory-kill signature")
        XCTAssertTrue(FlightRecorder.verdict(starved).contains("memory limit"),
                      "the verdict must name the reading: \(FlightRecorder.verdict(starved))")
    }

    /// A STALL IS ALARMING EVEN IF THE SESSION THEN CLOSES NORMALLY - it is the
    /// only record of the failure the owner actually described ("it just
    /// hangs"), and a hang the human waits out and then dismisses ends as
    /// cleanly as any other session.
    func testAStallIsAlarmingEvenWhenTheSessionClosesCleanly() {
        FlightRecorder.begin("first")
        FlightRecorder.note("stall", "main thread 3.4s")
        FlightRecorder.end("resigned")
        settle()
        FlightRecorder.abandonForTesting()
        FlightRecorder.begin("second")
        settle()
        let p = try! XCTUnwrap(FlightRecorder.previousSession())
        XCTAssertTrue(p.stalled)
        XCTAssertTrue(FlightRecorder.isAlarming(p))
        XCTAssertTrue(FlightRecorder.verdict(p).contains("STALLED"))
    }

    /// Every breadcrumb carries the footprint, because "what was it doing" and
    /// "how much memory did it have" only answer the question together.
    func testEveryNoteCarriesAFootprint() {
        FlightRecorder.begin("first")
        FlightRecorder.note("adopt")
        settle()
        let c = try! XCTUnwrap(FlightRecorder.currentSession())
        XCTAssertGreaterThan(c.notes.count, 1)
        for n in c.notes {
            XCTAssertGreaterThan(n.mb, 0, "\(n.event) recorded no memory reading")
        }
        XCTAssertGreaterThan(c.peakMB, 0)
    }

    /// A DETAIL WITH SPACES IN IT MUST NOT SHIFT A FIELD. The line format is
    /// space-separated with the free text last, and "dropped 3 textures" or
    /// "main thread 3.4s" are exactly the details being written - so a parser
    /// that split naively would read the memory column out of the message.
    func testADetailContainingSpacesSurvivesTheRoundTrip() {
        FlightRecorder.begin("first")
        FlightRecorder.note("adopt", "turn 12, 3 to animate")
        settle()
        let c = try! XCTUnwrap(FlightRecorder.currentSession())
        let n = try! XCTUnwrap(c.notes.first { $0.event == "adopt" })
        XCTAssertEqual(n.detail, "turn 12, 3 to animate")
        XCTAssertGreaterThan(n.mb, 0, "the detail's spaces ate the memory column")
    }

    /// A LONG SESSION KEEPS ITS END, not its beginning: the last thing that
    /// happened is what says how it died, and a report has to fit on a phone.
    func testAnOverlongTrailKeepsTheEndNotTheStart() {
        FlightRecorder.begin("first")
        for i in 0..<(FlightRecorder.maxNotes + 40) { FlightRecorder.note("move", "\(i)") }
        settle()
        let c = try! XCTUnwrap(FlightRecorder.currentSession())
        XCTAssertEqual(c.notes.count, FlightRecorder.maxNotes)
        XCTAssertEqual(c.notes.last?.detail, "\(FlightRecorder.maxNotes + 39)",
                       "the trail was trimmed from the wrong end")
    }

    /// The report is what the panel puts on screen, so it has to mention the
    /// previous session's verdict and the live footprint - the two things the
    /// owner opens it to read.
    func testTheReportSaysWhatHappenedAndWhereMemoryIsNow() {
        FlightRecorder.begin("first")
        FlightRecorder.note("memory-warning", "dropped 3 textures")
        settle()
        FlightRecorder.abandonForTesting()
        FlightRecorder.begin("second")
        FlightRecorder.note("adopt", "turn 1")
        settle()
        let text = FlightRecorder.report(previous: FlightRecorder.previousSession(),
                                         current: FlightRecorder.currentSession())
        XCTAssertTrue(text.contains("previous session"))
        XCTAssertTrue(text.contains("memory-warning"))
        XCTAssertTrue(text.contains("this session"))
        XCTAssertTrue(text.contains("now"), "the live footprint is missing")
    }

    /// With no trail at all (a first launch) the panel must say so rather than
    /// render an empty block that reads as a broken panel.
    func testAFirstLaunchSaysThereIsNothingToReport() {
        FlightRecorder.begin("first")
        settle()
        let text = FlightRecorder.report(previous: nil,
                                         current: FlightRecorder.currentSession())
        XCTAssertTrue(text.contains("no previous session recorded"))
    }
}
