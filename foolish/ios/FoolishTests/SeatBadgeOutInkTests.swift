// A player who is OUT is said with ink, not with transparency (round 16).
//
// The owner: "players text that are out are invisible against the wool
// background light mode. Make them just dark gray instead of decreasing
// opacity."
//
// They were invisible twice over - a dim sage picked for a dark board, and then
// the whole badge at 0.45 opacity on top of it, which on the pale weave left
// roughly nothing. The rule now is that the badge always draws at full
// strength and the out state is a COLOUR, which is what this pins: the ink for
// each ground, and that no ground gets a see-through one.
import XCTest
import SwiftUI
@testable import FoolishKit

final class SeatBadgeOutInkTests: XCTestCase {

    /// THE REPORT: light-mode wool. An out name must be the dark grey, not the
    /// sage that vanishes on a pale weave.
    func testAnOutNameOnTheLightWeaveIsDarkGrey() {
        XCTAssertEqual(FSeatBadge.nameInk(isOut: true, onLight: false, scheme: .light),
                       FColor.textOut)
        XCTAssertNotEqual(FSeatBadge.nameInk(isOut: true, onLight: false, scheme: .light),
                          FColor.textDim, "the sage is what was invisible")
    }

    /// The beige message bubble is a light ground in EITHER scheme - it is a
    /// painted bubble, not the weave - so it takes the same dark grey.
    func testTheBeigeBubbleIsALightGroundInBothSchemes() {
        for scheme in [ColorScheme.light, .dark] {
            XCTAssertTrue(FSeatBadge.onLightGround(onLight: true, scheme: scheme))
            XCTAssertEqual(FSeatBadge.nameInk(isOut: true, onLight: true, scheme: scheme),
                           FColor.textOut)
        }
    }

    /// Dark mode is deliberately untouched: the dark weave is where the sage
    /// reads, and a dark grey there would be the same mistake pointing the other
    /// way. The fix is that it is no longer ALSO dimmed to 0.45.
    func testTheDarkWeaveKeepsItsDimInk() {
        XCTAssertFalse(FSeatBadge.onLightGround(onLight: false, scheme: .dark))
        XCTAssertEqual(FSeatBadge.nameInk(isOut: true, onLight: false, scheme: .dark),
                       FColor.textDim)
    }

    /// A player still IN is unchanged in every combination - the bone name on
    /// the weave, dark on the bubble.
    func testAPlayerStillInIsUnchanged() {
        for scheme in [ColorScheme.light, .dark] {
            XCTAssertEqual(FSeatBadge.nameInk(isOut: false, onLight: false, scheme: scheme),
                           FColor.textPrimary)
            XCTAssertEqual(FSeatBadge.nameShadow(isOut: false, onLight: false, scheme: scheme),
                           Color.black.opacity(0.85), "the bone name is carried by its shadow")
        }
    }

    /// The hard black shadow exists to carry LIGHT ink on a light ground. Dark
    /// ink does not need it and is muddied by it, so an out name on a light
    /// ground drops it - and only there.
    func testTheShadowGoesExactlyWhereTheInkWentDark() {
        XCTAssertEqual(FSeatBadge.nameShadow(isOut: true, onLight: false, scheme: .light), .clear)
        XCTAssertEqual(FSeatBadge.nameShadow(isOut: true, onLight: true, scheme: .dark), .clear)
        XCTAssertEqual(FSeatBadge.nameShadow(isOut: true, onLight: false, scheme: .dark),
                       Color.black.opacity(0.85), "the dark weave still needs it")
    }

    /// NO INK IS SEE-THROUGH. The defect was a legible colour dimmed until it
    /// was not; if a future tweak reaches for opacity again on any ground, this
    /// is where it fails. `.opacity()` on a Color is observable through its
    /// resolved alpha, so this asks the real question rather than comparing
    /// against a hard-coded list of allowed colours.
    func testNoOutInkIsPartlyTransparent() {
        for scheme in [ColorScheme.light, .dark] {
            for onLight in [true, false] {
                let ink = FSeatBadge.nameInk(isOut: true, onLight: onLight, scheme: scheme)
                let alpha = UIColor(ink).cgColor.alpha
                XCTAssertEqual(alpha, 1.0, accuracy: 0.001,
                               "out ink is \(alpha) opaque on \(onLight ? "bubble" : "weave")/\(scheme)")
            }
        }
    }
}
