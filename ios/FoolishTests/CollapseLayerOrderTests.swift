import XCTest
@testable import FoolishKit

/// Two rules that nothing could see fail.
///
/// Both were checked by deleting the thing under test and running the suite: a
/// hand deleted from the board and the response knob disconnected again both
/// left 759 tests green. A suite that cannot fail against a change is not
/// evidence about that change, which is lesson 1 of docs/WINS_AND_LESSONS.md
/// and was the reason these exist.
///
/// Source tests, like `BoardSpringOverrideTests` and `CollapseTweenTests`
/// beside them, and for the same reason: the subject is WHERE a view is written
/// in a `body`, and a rendered view has no seam that reports its own z.
final class CollapseLayerOrderTests: XCTestCase {

    private func source(_ path: String) throws -> [String] {
        let here = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
        return try String(contentsOf: here.deletingLastPathComponent()
            .appendingPathComponent(path), encoding: .utf8)
            .components(separatedBy: "\n")
    }

    /// Flying cards, then my status mark, then my own cards - the owner's order
    /// on round 47. In SwiftUI that is written as the order the three appear in,
    /// so the test is that order.
    ///
    /// MUTANT: put `hand(view, reserveNoSlot:)` back in the board's ZStack (it
    /// sat just after the settings squares) and this fails - the hand then comes
    /// BEFORE the flight layer instead of after the mark.
    func testMyCardsAreDrawnAfterMyStatusMark() throws {
        let lines = try BoardSource.lines()
        let flights = try XCTUnwrap(lines.firstIndex { $0.contains("FlyingCardsLayer(animator:") },
                                    "the flight layer is gone")
        let mark = try XCTUnwrap(lines.firstIndex { $0.contains("selfRoleIndicator(v)") },
                                 "the self role mark is gone")
        let hand = try XCTUnwrap(lines.firstIndex { $0.contains("hand(view, reserveNoSlot:") },
                                 "my hand is not drawn anywhere")
        XCTAssertLessThan(flights, mark,
                          "round 41: a card in the air must not hide the status mark")
        XCTAssertLessThan(mark, hand,
                          "round 47: my own cards draw OVER my own status mark, so the "
                          + "hand must be written after it")
    }

    /// The action pills and the settings squares share the hand's level, so the
    /// opponent ring cannot come out between them. Under the slide each seat is
    /// hosted in a UIHostingController of its own and composites above plain
    /// SwiftUI siblings whatever this ZStack says, which is how the badges ended
    /// up over the buttons and under the cards.
    ///
    /// MUTANT: leave `actionBar`, `undoSlot` or `settingsHelpBar` in the board's
    /// ZStack and this fails.
    func testTheButtonsShareTheHandsLevel() throws {
        let lines = try BoardSource.lines()
        let mark = try XCTUnwrap(lines.firstIndex { $0.contains("selfRoleIndicator(v)") })
        for name in ["actionBar(view)", "undoSlot", "settingsHelpBar"] {
            let at = try XCTUnwrap(lines.firstIndex { $0.hasSuffix(name) },
                                   "\(name) is gone")
            XCTAssertGreaterThan(at, mark,
                                 "\(name) must be drawn with the hand, after the mark - "
                                 + "left in the board's stack the hosted seat badges "
                                 + "composite over it")
        }
    }

    /// The collapse's response reaches BOTH curves it drives. It reached
    /// neither: `slideOffsets` has a defaulted `response`, so the hosting
    /// layer's keyframes and the table group's both ran on the constant while
    /// `dev.collapse`'s `resp=` moved only the retired driver - the one path in
    /// use could not be swept at all.
    ///
    /// MUTANT: drop `response:` from either call and this fails. Swept once it
    /// was connected: 0.300 scores an MSE of 7682, 0.338 scores 119, 0.376
    /// scores 2009.
    func testTheResponseKnobReachesEveryCurveItDrives() throws {
        for path in ["FoolishMessages/MessagesViewController.swift",
                     "FoolishKit/Messages/CollapseLayer.swift"] {
            let text = try source(path).joined(separator: "\n")
            guard let call = text.range(of: "CollapseTween.slideOffsets(") else {
                XCTFail("\(path) no longer asks for the slide's offsets"); continue
            }
            let args = String(text[call.upperBound...].prefix(160))
            XCTAssertTrue(args.contains("response:"),
                          "\(path) takes slideOffsets' DEFAULT response, so the knob "
                          + "cannot reach it and the curve it runs is whatever the "
                          + "constant happens to be")
        }
    }
}

/// The collapse the product ships is the slide, and debug builds agree with it.
///
/// Build 70 went to a real device with the slide off, and bounced. Two switches
/// decide the path: `CollapseTween.slideByDefault` for Release, and
/// `MessageDevBoard.CollapseKnobs.slide` for DEBUG when no `dev.collapse` file
/// says otherwise. The second was a hardcoded `false`, so turning the product on
/// would have left every debug install and rig take on the old path.
///
/// MUTANTS: `slideByDefault = false` fails the first assertion; a literal
/// `slide = false` in CollapseKnobs fails the second.
final class CollapseShipsTheSlideTests: XCTestCase {
    func testTheShippingCollapseIsTheSlide() {
        XCTAssertTrue(CollapseTween.slideByDefault,
                      "build 70 shipped the old timer path and it bounced on a real device")
    }

    #if DEBUG || SOLO_TESTING
    func testADebugBuildDefaultsToTheShippingPath() {
        XCTAssertEqual(MessageDevBoard.CollapseKnobs().slide, CollapseTween.slideByDefault,
                       "a debug install with no dev.collapse file must run the path the "
                       + "product ships, or every rig take measures the wrong collapse")
    }
    #endif
}
