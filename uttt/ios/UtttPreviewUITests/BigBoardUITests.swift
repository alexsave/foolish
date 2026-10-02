#if UTTT_BIG_BOARD
import XCTest

/// THE 243 BOARD UNDER A FINGER (docs/BIG_BOARD.md): the preview harness's
/// 243 screen (UtttPreview/BigBoardPreview.swift) driven with real gestures -
/// pinch, swipe, double tap, tap - on a simulator. idb has no pinch, so this
/// is the only automated proof that the scroll view's own pinch reaches the
/// board.
///
/// The board reports itself in its accessibility value: "zoom Z offset X,Y
/// whole W centre C" (UtttBigBoardView.accessibilityValue); the harness
/// writes the last tapped cell, the memory footprint and its geometry
/// self-check as text above it.
///
/// MUTATION CHECKED (2026-10-01): with maximumZoomScale clamped to 1 in
/// UtttBigBoardView.layoutSubviews, the run went red at the pinch assertion
/// (zoom 1.0 is not greater than 1.5, "pinch did not zoom in"); restored,
/// green. The footprint gate went red on its own first: the board was a
/// CATiledLayer, which peaked at 167-183 MB during a double-tap zoom.
final class BigBoardUITests: XCTestCase {
    private struct Seen {
        var zoom: Double
        var x: Double
        var y: Double
        var whole: Bool
        var centre: Int
    }

    private var app: XCUIApplication!

    override func setUp() {
        continueAfterFailure = false
        app = XCUIApplication()
        app.launchArguments = ["--screen", "243"]
        app.launch()
    }

    private func seen(_ board: XCUIElement) -> Seen {
        let v = (board.value as? String) ?? ""
        let w = v.split(separator: " ").map(String.init)
        func after(_ k: String) -> String { (w.firstIndex(of: k).map { $0 + 1 < w.count ? w[$0 + 1] : "" }) ?? "" }
        let off = after("offset").split(separator: ",").compactMap { Double($0) }
        return Seen(zoom: Double(after("zoom")) ?? -1,
                    x: off.first ?? .nan, y: off.count > 1 ? off[1] : .nan,
                    whole: after("whole") == "1", centre: Int(after("centre")) ?? -2)
    }

    /// The value once it stops changing (a zoom animates, a pan decelerates).
    private func settled(_ board: XCUIElement) -> Seen {
        var prev = (board.value as? String) ?? ""
        for _ in 0..<40 {
            Thread.sleep(forTimeInterval: 0.25)
            let now = (board.value as? String) ?? ""
            if now == prev { break }
            prev = now
        }
        return seen(board)
    }

    /// The harness's memory line, printed per step so a peak can be placed.
    private func memory(_ step: String) {
        print("BIGBOARD MEMORY \(step): \(app.staticTexts["big.memory"].label)")
    }

    private func shot(_ name: String) {
        memory(name)
        let a = XCTAttachment(screenshot: app.screenshot())
        a.name = name
        a.lifetime = .keepAlways
        add(a)
    }

    func testPinchPanDoubleTapAndTap() {
        let board = app.descendants(matching: .any)["big.board"]
        XCTAssertTrue(board.waitForExistence(timeout: 15), "no board")
        XCTAssertEqual(app.staticTexts["big.geometry"].label, "geometry ok")

        /* fitted: the whole board on screen at zoom 1 */
        var s = settled(board)
        XCTAssertEqual(s.zoom, 1, accuracy: 0.01, "initial zoom")
        XCTAssertTrue(s.whole, "the whole board is not on screen at first")
        Thread.sleep(forTimeInterval: 1)
        shot("1-fit")

        /* a pinch out of the scroll view's own recogniser */
        board.pinch(withScale: 4, velocity: 2)
        s = settled(board)
        XCTAssertGreaterThan(s.zoom, 1.5, "pinch did not zoom in")
        XCTAssertFalse(s.whole, "zoomed in, yet the whole board is on screen")
        Thread.sleep(forTimeInterval: 1)
        shot("2-mid")

        /* a pan moves the visible part */
        let before = s
        board.swipeLeft()
        s = settled(board)
        XCTAssertTrue(abs(s.x - before.x) > 10 || abs(s.y - before.y) > 10,
                      "swipe did not pan: \(before.x),\(before.y) -> \(s.x),\(s.y)")

        /* and back out to the whole board */
        board.pinch(withScale: 0.1, velocity: -4)
        s = settled(board)
        XCTAssertEqual(s.zoom, 1, accuracy: 0.05, "pinch in did not return to the fit")

        /* double taps: in by three each, to the deepest zoom, then out */
        board.doubleTap()
        s = settled(board)
        XCTAssertEqual(s.zoom, 3, accuracy: 0.05, "double tap did not zoom in by three")
        memory("double tap 1")
        board.doubleTap()
        _ = settled(board)
        memory("double tap 2")
        board.doubleTap()
        s = settled(board)
        memory("double tap 3")
        let deep = s.zoom
        XCTAssertGreaterThan(deep, 15, "three double taps did not reach the deepest zoom")
        Thread.sleep(forTimeInterval: 1.5)
        shot("3-max")

        /* the deepest zoom while panning: the footprint stays far under the
         * extension's jetsam limit (about 120 MB) */
        for i in 0..<8 {
            if i % 2 == 0 { board.swipeLeft() } else { board.swipeUp() }
            memory("pan \(i)")
        }
        _ = settled(board)
        Thread.sleep(forTimeInterval: 1)
        shot("4-max-panned")
        let mem = app.staticTexts["big.memory"].label
        let words = mem.split(separator: " ")
        let peak = words.firstIndex(of: "peak").flatMap { Double(words[$0 + 1]) } ?? -1
        let note = XCTAttachment(string: mem)
        note.name = "memory"
        note.lifetime = .keepAlways
        add(note)
        print("BIGBOARD MEMORY \(mem)")
        XCTAssertGreaterThan(peak, 0, "no footprint reading: \(mem)")
        XCTAssertLessThan(peak, 120, "footprint at the deepest zoom: \(mem)")

        /* a tap at the deepest zoom names the cell under it */
        s = settled(board)
        XCTAssertGreaterThanOrEqual(s.centre, 0)
        board.tap()
        let tapped = app.staticTexts["tapped \(s.centre)"]
        XCTAssertTrue(tapped.waitForExistence(timeout: 5),
                      "tap at the centre did not report cell \(s.centre): \(app.staticTexts["big.tapped"].label)")
        XCTAssertEqual(settled(board).zoom, deep, accuracy: 0.01, "a single tap changed the zoom")
        Thread.sleep(forTimeInterval: 0.5)
        shot("5-tapped")

        /* a double tap at the deepest zoom goes back to the whole board */
        board.doubleTap()
        s = settled(board)
        XCTAssertEqual(s.zoom, 1, accuracy: 0.01, "double tap at the deepest zoom did not fit")
        XCTAssertTrue(s.whole)
    }
}
#endif
