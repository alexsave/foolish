// WoodHitRegionTests - A TEXTURE THAT HIDES ITS OVERFLOW MUST HIDE IT FROM
// TOUCHES TOO.
//
// ROUND 39. The owner, 1.0(38), on the pre-game screens in the compact drawer:
//
//   "Can't fucking got create game un collapsed, only expanded. Create game.
//    Name field. Passing setting are all offset like I need to press a bit
//    higher than the actual button to hit it"
//   "Settings can't even open wtf. Tapping it opens rules"
//
// Both are ONE fault, and it is not a layout fault: measured on the live
// extension (iPhone 16, compact drawer, the New-game screen), every control's
// layout rect and its painted pixels agreed to within half a point. What
// disagreed was the TOUCH REGION. `WoodFill` paints a 448x288pt baked swatch
// inside whatever plank it fills and hides the overflow with `.clipped()` -
// which bounds the PAINT and not the hit test, so a 40pt square answered taps
// ~70pt past each of its own edges. The rulebook square (painted x81..121)
// answered from x11 to x191, which swallows the gear 16pt to its left whole:
// tapping Settings opened the rules because the touch never reached Settings.
// The cure is one clause, `.contentShape(Rectangle())`, and this file exists to
// keep it there.
//
// WHY THIS IS A SOURCE GUARD AND NOT A BEHAVIOUR TEST. Two better tests were
// written and thrown away:
//
//   A SNAPSHOT proves nothing here, and worse, it proves the wrong thing
//   confidently. The paint was never wrong - every picture test in this suite
//   was green all the way through the bug and would have stayed green.
//
//   A hitTest ASSERTION was the obvious one: host `WoodFill` in a `UIWindow`
//   and ask whether a point in the bare air beside the plank is claimed. It
//   does not work. `_UIHostingView` answers `hitTest` with ITSELF for every
//   point inside its own bounds, claimed by SwiftUI or not (verified: the
//   assertion fails identically with the fix in and out), so the question a
//   UIKit hit test can ask is not the question this bug is about. SwiftUI's own
//   hit region is not reachable from XCTest, and there is no XCUITest target in
//   this project to tap a real button in.
//
// So: a scan of the one file, which is crude, and which does fail against the
// one mutation that matters - deleting the clause reintroduces the owner's bug
// and reddens this test. MUTATION-CHECKED both ways.
import XCTest
@testable import FoolishKit

final class WoodHitRegionTests: XCTestCase {

    /// Materials.swift, read off disk. `#filePath` is this file, and the source
    /// tree is where the suite is built from, so the sibling is one hop away.
    private func materialsSource() throws -> String {
        let here = URL(fileURLWithPath: #filePath)          // ios/FoolishTests/…
        let file = here.deletingLastPathComponent()          // ios/FoolishTests
            .deletingLastPathComponent()                     // ios
            .appendingPathComponent("FoolishKit/DesignSystem/Materials.swift")
        return try String(contentsOf: file, encoding: .utf8)
    }

    /// Every `.clipped()` in Materials.swift is hiding an oversized texture, and
    /// every one of them therefore owes a `.contentShape(Rectangle())` - the
    /// clause that says the touch region is the frame you can see. Checked as
    /// "the next non-comment line", because that is where it has to be to bind
    /// to the same view, and because a file-wide count would pass while the two
    /// clauses sat on the wrong views.
    func testEveryClippedTextureBoundsItsOwnTouchRegion() throws {
        let lines = try materialsSource().split(separator: "\n", omittingEmptySubsequences: false)
        var checked = 0
        for (i, line) in lines.enumerated()
        where line.contains(".clipped()")
            // …the CALL, not the several comments in that file that name it.
            && !line.trimmingCharacters(in: .whitespaces).hasPrefix("//") {
            checked += 1
            let next = lines[(i + 1)...].first {
                let t = $0.trimmingCharacters(in: .whitespaces)
                return !t.isEmpty && !t.hasPrefix("//")
            } ?? ""
            XCTAssertTrue(next.contains(".contentShape("),
                          """
                          Materials.swift:\(i + 1) clips an oversized texture but does not \
                          bound its touch region. `.clipped()` hides the paint only - the \
                          hidden overflow goes on answering taps ~70pt past the plank, which \
                          is the 1.0(38) report ("Tapping it opens rules"). Add \
                          `.contentShape(Rectangle())` on the next line.
                          """)
        }
        XCTAssertEqual(checked, 2,
                       "Materials.swift should have exactly two clipped textures (TableWeave, WoodFill) - if that changed, this guard needs re-reading, not re-numbering")
    }

    /// The premise, so the guard above cannot quietly become vacuous: the baked
    /// swatch really is far larger than the controls it fills, which is why the
    /// overflow is enormous rather than a rounding error. 448x288 against a 40pt
    /// settings square is the ~70pt slab, measured.
    func testTheBakedWoodSwatchDwarfsTheControlsItFills() {
        XCTAssertEqual(WoodTexture.pointsPerTexel, 1, "one texel is one point - the swatch's texels ARE its points")
        XCTAssertGreaterThan(CGFloat(WoodTexture.renderCanvas.w), 400)
        XCTAssertGreaterThan(CGFloat(WoodTexture.renderCanvas.h), 250)
        // The square the owner could not tap. If this ever stops being much
        // smaller than the swatch, the overflow - and this whole class - is
        // about something else.
        XCTAssertLessThan(SettingsHelpSquares.reservedHeight, CGFloat(WoodTexture.renderCanvas.h) / 4)
    }
}
