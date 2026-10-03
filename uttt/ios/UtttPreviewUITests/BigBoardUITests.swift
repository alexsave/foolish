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
/// testResizeKeepsZoom, the same day: with the size-change path in
/// UtttBigBoardView.layoutSubviews made to re-fit (the zoom restore skipped),
/// it went red at "shrinking the board changed the zoom" (1.0 is not 3.0);
/// restored, green.
/// testGridHoldTogglesOnlyAFourSecondStill, 2026-10-01: with UtttModeHold's
/// minimumPressDuration made 1 s it went red at "a 3 s hold fired the door"
/// ("hold fired 2" is not "hold fired 1"); with its allowableMovement made
/// 10,000 it went red at "a hold that drifted 40 points fired the door" (the
/// same numbers); each restored from a copy, green.
/// SmallBoardHoldUITests, the same day: with UtttBoardView.setModeHold made
/// to install nothing it went red at "a 4.6 s still hold on the 9 x 9 board
/// did not fire: hold fired 0"; restored, green.
final class BigBoardUITests: XCTestCase {
    private struct Seen {
        var zoom: Double
        var x: Double
        var y: Double
        var whole: Bool
        var centre: Int
        /// The board's rect in the board view's own points.
        var board: CGRect
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
        let b = after("board").split(separator: ",").compactMap { Double($0) }
        return Seen(zoom: Double(after("zoom")) ?? -1,
                    x: off.first ?? .nan, y: off.count > 1 ? off[1] : .nan,
                    whole: after("whole") == "1", centre: Int(after("centre")) ?? -2,
                    board: b.count == 4 ? CGRect(x: b[0], y: b[1], width: b[2], height: b[3]) : .null)
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

    /// THE BOARD PANS PAST ITS EDGES (docs/BIG_BOARD.md; owner, build
    /// 1.1(16): "the very bottom left corner is hard to pan to"). For each
    /// corner: from the fit, three double taps on the corner zoom to the
    /// deepest zoom there; swipes toward the corner until the board stops;
    /// the corner cell is then in the middle half of the view, a tap on it
    /// names it, and a double tap goes back to exactly the first fit.
    func testEveryCornerCellPansToTheMiddleAndTaps() {
        let board = app.descendants(matching: .any)["big.board"]
        XCTAssertTrue(board.waitForExistence(timeout: 15), "no board")
        let fit = settled(board)
        XCTAssertEqual(fit.zoom, 1, accuracy: 0.01, "initial zoom")
        XCTAssertTrue(fit.whole && !fit.board.isNull, "the fit: \(fit)")
        let view = board.frame
        /* a point of the board view's own coordinates, as a tap */
        func at(_ p: CGPoint) -> XCUICoordinate {
            board.coordinate(withNormalizedOffset: .zero).withOffset(CGVector(dx: p.x, dy: p.y))
        }
        /* the centre of cell (col, row) in the board view's points */
        func cell(_ s: Seen, _ col: Int, _ row: Int) -> CGPoint {
            let c = s.board.width / 243
            return CGPoint(x: s.board.minX + (CGFloat(col) + 0.5) * c, y: s.board.minY + (CGFloat(row) + 0.5) * c)
        }
        let corners: [(name: String, col: Int, row: Int, index: Int)] = [
            ("top left", 0, 0, 0), ("top right", 242, 0, 14_762),
            ("bottom left", 0, 242, 44_286), ("bottom right", 242, 242, 59_048),
        ]
        for k in corners {
            /* 1. in on the corner, three double taps; a point off screen
             * is pulled in to the nearest one 20 points inside */
            for _ in 0..<3 {
                let p = cell(settled(board), k.col, k.row)
                at(CGPoint(x: min(max(p.x, 20), view.width - 20),
                           y: min(max(p.y, 20), view.height - 20))).doubleTap()
            }
            var s = settled(board)
            XCTAssertGreaterThan(s.zoom, 15, "\(k.name): three double taps did not reach the deepest zoom")

            /* 2. swipe the board toward the corner until it stops */
            for _ in 0..<12 {
                let before = s
                if k.col == 0 { board.swipeRight() } else { board.swipeLeft() }
                if k.row == 0 { board.swipeDown() } else { board.swipeUp() }
                s = settled(board)
                if abs(s.x - before.x) < 1 && abs(s.y - before.y) < 1 { break }
            }
            let p = cell(s, k.col, k.row)
            print("BIGBOARD CORNER \(k.name): zoom \(s.zoom) cell centre \(p) in a view \(view.size), board \(s.board)")
            Thread.sleep(forTimeInterval: 0.5)
            shot("corner-\(k.name)")

            /* 3. past the edge: the corner cell is in the middle half of the view */
            XCTAssertTrue(p.x > view.width / 4 && p.x < view.width * 3 / 4 &&
                          p.y > view.height / 4 && p.y < view.height * 3 / 4,
                          "\(k.name): the corner cell stopped at \(p), not in the middle of \(view.size)")

            /* 4. a tap on it names it */
            at(p).tap()
            XCTAssertTrue(app.staticTexts["tapped \(k.index)"].waitForExistence(timeout: 5),
                          "\(k.name): the tap did not name cell \(k.index): \(app.staticTexts["big.tapped"].label)")

            /* 5. a double tap at the deepest zoom is the first fit again, exactly */
            at(p).doubleTap()
            s = settled(board)
            XCTAssertEqual(s.zoom, 1, accuracy: 0.01, "\(k.name): the double tap did not fit")
            XCTAssertTrue(s.whole, "\(k.name): not the whole board after the fit")
            XCTAssertEqual(s.x, 0, accuracy: 0.5, "\(k.name): the fit is off centre: \(s)")
            XCTAssertEqual(s.y, 0, accuracy: 0.5, "\(k.name): the fit is off centre: \(s)")
            XCTAssertEqual(s.board.minX, fit.board.minX, accuracy: 0.5, "\(k.name): not the first fit: \(s.board)")
            XCTAssertEqual(s.board.minY, fit.board.minY, accuracy: 0.5, "\(k.name): not the first fit: \(s.board)")
            XCTAssertEqual(s.board.width, fit.board.width, accuracy: 0.5, "\(k.name): not the first fit: \(s.board)")
        }
        memory("corners")
        let words = app.staticTexts["big.memory"].label.split(separator: " ")
        let peak = words.firstIndex(of: "peak").flatMap { Double(words[$0 + 1]) } ?? -1
        XCTAssertTrue(peak > 0 && peak < 120, "footprint over the corners: \(app.staticTexts["big.memory"].label)")
    }

    /// THE MODE'S DOOR (UtttModeHold, docs/BIG_BOARD.md): only a still hold
    /// of 4 s fires it. A shorter hold, a hold that drifts 40 points, a tap,
    /// a double tap and a pinch never do, and each of those still does its
    /// own thing. The harness counts the door in "hold fired N".
    func testGridHoldTogglesOnlyAFourSecondStill() {
        let board = app.descendants(matching: .any)["big.board"]
        XCTAssertTrue(board.waitForExistence(timeout: 15), "no board")
        let count = app.staticTexts["hold.count"]
        XCTAssertEqual(count.label, "hold fired 0")
        XCTAssertEqual(settled(board).zoom, 1, accuracy: 0.01, "initial zoom")

        /* 1. a still 4.6 s hold fires the door once, and taps nothing */
        board.press(forDuration: 4.6)
        XCTAssertTrue(app.staticTexts["hold fired 1"].waitForExistence(timeout: 3),
                      "a 4.6 s still hold did not fire: \(count.label)")
        XCTAssertEqual(app.staticTexts["big.tapped"].label, "tapped none", "the hold also tapped a cell")

        /* 2. a 3 s hold is not one */
        board.press(forDuration: 3.0)
        Thread.sleep(forTimeInterval: 1.5)
        XCTAssertEqual(count.label, "hold fired 1", "a 3 s hold fired the door")

        /* 3. a press that drags 40 points and is still down at 4.6 s is not one */
        let from = board.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.5))
        from.press(forDuration: 0.3, thenDragTo: from.withOffset(CGVector(dx: 40, dy: 0)),
                   withVelocity: 40, thenHoldForDuration: 3.3)
        Thread.sleep(forTimeInterval: 1.5)
        XCTAssertEqual(count.label, "hold fired 1", "a hold that drifted 40 points fired the door")
        _ = settled(board)

        /* 4. a tap names its cell at once, and is not a hold */
        board.tap()
        let tapped = app.staticTexts["big.tapped"]
        let named = NSPredicate(format: "label != %@", "tapped none")
        XCTAssertEqual(XCTWaiter.wait(for: [expectation(for: named, evaluatedWith: tapped)], timeout: 3), .completed,
                       "a tap did not reach the board")
        XCTAssertEqual(count.label, "hold fired 1", "a tap fired the door")

        /* 5. a double tap zooms, and is not a hold */
        let before = settled(board).zoom
        board.doubleTap()
        let zoomed = settled(board).zoom
        XCTAssertGreaterThan(zoomed, before * 2, "the double tap did not zoom")
        XCTAssertEqual(count.label, "hold fired 1", "a double tap fired the door")

        /* 6. a pinch zooms, and is not a hold */
        board.pinch(withScale: 0.2, velocity: -3)
        XCTAssertLessThan(settled(board).zoom, zoomed, "the pinch did not zoom out")
        Thread.sleep(forTimeInterval: 1)
        XCTAssertEqual(count.label, "hold fired 1", "a pinch fired the door")
    }

    /// A drawer resize (expanded to compact and back) keeps the zoom and the
    /// cell under the centre: the board fits only on its first layout. The
    /// harness's "resize" button makes the board 340 points tall and back.
    func testResizeKeepsZoom() {
        let board = app.descendants(matching: .any)["big.board"]
        XCTAssertTrue(board.waitForExistence(timeout: 15), "no board")
        var s = settled(board)
        XCTAssertEqual(s.zoom, 1, accuracy: 0.01, "initial zoom")
        let tall = board.frame.height

        board.doubleTap()
        s = settled(board)
        XCTAssertEqual(s.zoom, 3, accuracy: 0.05, "double tap did not zoom in by three")
        let zoom = s.zoom, centre = s.centre

        let resize = app.buttons["big.resize"]
        resize.tap()
        s = settled(board)
        XCTAssertLessThan(board.frame.height, tall - 50, "resize did not shrink the board")
        XCTAssertEqual(s.zoom, zoom, accuracy: 0.01, "shrinking the board changed the zoom")
        XCTAssertEqual(s.centre, centre, "shrinking the board moved the centre")

        resize.tap()
        s = settled(board)
        XCTAssertEqual(board.frame.height, tall, accuracy: 1, "resize did not grow the board back")
        XCTAssertEqual(s.zoom, zoom, accuracy: 0.01, "growing the board changed the zoom")
        XCTAssertEqual(s.centre, centre, "growing the board moved the centre")
    }
}
/// THE MODE'S DOOR ON THE 9 x 9 BOARD (UtttBoardView.setModeHold, through
/// UtttGameScreen.setGridHold): a 4.6 s still hold on a square I may play
/// fires the door and plays nothing; a tap on the same square still plays it.
final class SmallBoardHoldUITests: XCTestCase {
    private var app: XCUIApplication!

    override func setUp() {
        continueAfterFailure = false
        app = XCUIApplication()
        app.launchArguments = ["--size", "expanded"]
        app.launch()
    }

    func testGridHoldOnTheNineByNine() {
        let legal = app.staticTexts["small.legal"]
        XCTAssertTrue(legal.waitForExistence(timeout: 15), "no legal square named")
        let filled = NSPredicate(format: "label != %@", "")
        XCTAssertEqual(XCTWaiter.wait(for: [expectation(for: filled, evaluatedWith: legal)], timeout: 5), .completed)
        let square = app.buttons[legal.label]
        XCTAssertTrue(square.waitForExistence(timeout: 5), "no square '\(legal.label)'")
        let count = app.staticTexts["hold.count"], moves = app.staticTexts["small.moves"]
        XCTAssertEqual(count.label, "hold fired 0")
        XCTAssertEqual(moves.label, "moves 0")

        square.press(forDuration: 4.6)
        XCTAssertTrue(app.staticTexts["hold fired 1"].waitForExistence(timeout: 3),
                      "a 4.6 s still hold on the 9 x 9 board did not fire: \(count.label)")
        Thread.sleep(forTimeInterval: 1)
        XCTAssertEqual(moves.label, "moves 0", "the hold also played the square")

        square.tap()
        XCTAssertTrue(app.staticTexts["moves 1"].waitForExistence(timeout: 3),
                      "a tap on a legal square did not play it: \(moves.label)")
        XCTAssertEqual(count.label, "hold fired 1", "the tap fired the door")
    }
}
#endif
