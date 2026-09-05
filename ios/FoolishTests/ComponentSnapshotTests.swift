// ComponentSnapshotTests.swift — snapshot coverage for DesignSystem components
// in light/dark, Dynamic Type, and ru/ko strings (§13, §16.A6). Uses
// pointfreeco/swift-snapshot-testing.
//
// THE REFERENCES ARE LOCAL TO WHICHEVER MAC RECORDED THEM. `__Snapshots__/` is
// gitignored (.gitignore:63) and the workflow's Xcode job is still commented
// out (.github/workflows/ios.yml), so nothing but this machine has ever held
// them and nothing but this machine has ever checked them. A fresh clone
// records its own on the first run - which fails once, by design, and passes
// after.
//
// ROUND 30 corrects what this header used to claim ("commit the __Snapshots__
// folder ... the CI Mac job owns the reference images"). Neither half was true,
// and believing it is how two of these sat red for six weeks: a picture nobody
// else can reproduce, failing for a reason nobody read. When a deliberate
// visual change lands, RE-RECORD - flip `record` (or just the affected case) to
// true, look at the new image, and flip it back. That is not a workaround, it
// is the whole maintenance model for a local reference.
//
// To record: set `record = true`, run, set it back.

import XCTest
import SwiftUI
import SnapshotTesting
@testable import FoolishKit

final class ComponentSnapshotTests: XCTestCase {

    // Flip to true on a Mac to (re)record references, then flip back and commit.
    private let record = false

    /// HOW EXACT A MATCH HAS TO BE, and the reason it is not "exactly".
    ///
    /// ROUND 30. Two of these references had been red for six weeks and kept
    /// being deferred, and when finally read they were two different failures
    /// wearing the same message. The trump glyph had genuinely MOVED - e61952c
    /// deliberately sat the bare mark's ink square in its corner, and the
    /// reference predates it. The board had not moved at all: 0.4% of its
    /// pixels differed, all of them on anti-aliased edges of the same badges
    /// and the same shield, at a rasterisation the reference (16 July) and this
    /// Xcode simply do not agree on down to the last subpixel.
    ///
    /// A picture test that fails on the second kind teaches you to ignore it,
    /// which is exactly what happened - and while it was being ignored it could
    /// not report the first kind either. So: `perceptualPrecision` lets a pixel
    /// be a shade off without being wrong (0.98 is about a 2% perceptual
    /// difference, which covers edge anti-aliasing and nothing structural), and
    /// `precision` still demands that essentially every pixel clear that bar.
    ///
    /// Deliberately NOT loose enough to hide a real change - proven by
    /// mutation, not by argument: with these numbers, reverting the trump ink
    /// offset and re-tinting the shield both still fail (see the commit).
    private static let imagePrecision: Float = 0.999
    private static let imagePerceptual: Float = 0.98

    /// The table surface these references were rendered against, restored in
    /// `tearDown`. nil until `setUp` has run.
    private var tableBefore: TableSurface?

    override func setUp() {
        super.setUp()
        // isRecording is global to the library.
        // SnapshotTesting.isRecording = record
        //
        // PIN THE TABLE, for exactly the reason `host` pins the colour scheme.
        //
        // `TableBackground` reads `FPrefs.shared.table`, which is a PERSISTED
        // user preference (`ios.table.surface` in UserDefaults.standard, added
        // with the table picker in 1.0(33)) - so it survives between runs, and
        // whatever the simulator last had is what these images render against.
        // Nothing in this class was declaring it, so the reference PNGs
        // recorded whatever the RECORDING machine happened to be set to.
        //
        // That is not hypothetical. `testRoleMarksReadAsOneFamily` went red on
        // 2026-09-04 with no source change and passed forty seconds earlier: its
        // reference (recorded 2026-09-03) is green FELT, and felt is the option,
        // not the baseline - `FPrefs.table` defaults to `.wool`. So the
        // reference had captured a session in which somebody had switched the
        // table, and the test only kept passing for as long as that preference
        // happened to survive on this machine.
        //
        // `.wool` because it is the product's default AND what this class is
        // actually about - the role-marks test's own doc says the marks are
        // drawn "on the wool they have to survive".
        //
        // Worth knowing while reading this: `ios/FoolishTests/__Snapshots__` is
        // git-ignored, so every machine records its own references on first run.
        // That makes an unpinned ambient input worse than flaky - it is
        // per-machine, and no two checkouts are asserting the same picture.
        // `FPrefs` is @MainActor and these overrides are not (the class is not,
        // unlike Round30NitTests). XCTest runs setUp/tearDown on the main thread
        // for a UI-rendering suite, which is also where every `host(_:)` below
        // already relies on being, so the isolation is a fact to assert rather
        // than a hop to make - a hop would need async overrides and would not be
        // ordered against the test body anyway.
        MainActor.assumeIsolated {
            tableBefore = FPrefs.shared.table
            FPrefs.shared.setTable(.wool)
        }
    }

    override func tearDown() {
        MainActor.assumeIsolated {
            if let tableBefore { FPrefs.shared.setTable(tableBefore) }
        }
        tableBefore = nil
        super.tearDown()
    }

    private func host<V: View>(_ view: V, width: CGFloat = 320, height: CGFloat = 200) -> UIViewController {
        // PINNED to light, and that pin is what keeps these references stable
        // now that the board has a dark mode (round-7). Every component below
        // reads `@Environment(\.colorScheme)` somewhere - FCard inverts its
        // face, WoodFill loads a different bake, `onTableText` flips its ink -
        // and an unpinned UIHostingController inherits the SIMULATOR's current
        // appearance, so `simctl ui <udid> appearance dark` would silently
        // fail every image here without a line of code having changed.
        //
        // Light specifically, because that is what the committed references
        // were recorded in: pinning it is a no-op against them, which is the
        // point. A dark twin of these cases would need its own recorded
        // references and is deliberately NOT added here (the owner records).
        let content = view
            .frame(width: width, height: height)
            .background(FColor.table)
            .environment(\.colorScheme, .light)
        let vc = UIHostingController(rootView: content)
        vc.view.frame = CGRect(x: 0, y: 0, width: width, height: height)
        return vc
    }

    func testFCardFace() {
        assertSnapshot(of: host(FCard(card: Card(s: 1, v: 13), trump: true), width: 120, height: 140),
                       as: .image(precision: Self.imagePrecision,
                                  perceptualPrecision: Self.imagePerceptual),
                       record: record)
    }

    func testFCardBack() {
        assertSnapshot(of: host(FCard(card: nil, backSeed: 42), width: 120, height: 140),
                       as: .image(precision: Self.imagePrecision,
                                  perceptualPrecision: Self.imagePerceptual),
                       record: record)
    }

    func testFHandFan() {
        let cards = [Card(s: 0, v: 7), Card(s: 1, v: 13), Card(s: 2, v: 6), Card(s: 3, v: 11)]
        assertSnapshot(of: host(FHandFan(cards: cards, trumpSuit: .hearts,
                                         selection: .constant([]), onTap: { _ in }),
                                width: 320, height: 140),
                       as: .image(precision: Self.imagePrecision,
                                  perceptualPrecision: Self.imagePerceptual),
                       record: record)
    }

    func testFActionBarRu() {
        FStrings.override = .ru
        defer { FStrings.override = .en }   // no more .system; restore to English default
        assertSnapshot(of: host(FActionBar(canPass: true, canPickup: true, canDone: true,
                                           onPass: {}, onPickup: {}, onDone: {}),
                                width: 340, height: 80),
                       as: .image(precision: Self.imagePrecision,
                                  perceptualPrecision: Self.imagePerceptual),
                       record: record)
    }

    /// ROUND 12: the three role marks together, at the sizes the board draws
    /// them (`FRoleMark`), on the wool they have to survive.
    ///
    /// Together on purpose, and at real size on purpose. Each one alone proves
    /// nothing about the thing that was actually wrong: they were three
    /// unrelated colour schemes (a near-black sword that flipped to steel in
    /// dark mode, a mid-grey shield, a bare green check) at three sizes chosen
    /// in three different rounds, so at a glance the board carried three
    /// unrelated objects. The sword and shield now share one white-on-black ink;
    /// the check keeps its green — a check says something HAPPENED where the
    /// other two say which role you hold — and takes the black rim so it still
    /// belongs to the family.
    func testRoleMarksReadAsOneFamily() {
        let marks = HStack(spacing: 18) {
            FSword(size: FRoleMark.sword)
            FShield(size: FRoleMark.shield)
            FCheck(size: FRoleMark.check)
        }
        .frame(height: FRoleMark.rowHeight)
        .padding(20)
        .background(TableBackground())
        assertSnapshot(of: host(marks, width: 260, height: 100), as: .image(precision: Self.imagePrecision,
                                  perceptualPrecision: Self.imagePerceptual),
                       record: record)
    }

    /// ROUND 12: the bare trump glyph beside the same suit on a card face.
    ///
    /// When the stock and the flipped card are both gone, `FDeckWell` draws the
    /// trump suit as a lone glyph — the only suit on the whole board that is NOT
    /// drawn by `FCard`. It was in the SYSTEM font while every card is Georgia,
    /// and the two typefaces do not draw the same shape (SF's heart is narrow
    /// and straight-shouldered, Georgia's round and full), so the trump mark and
    /// the trump cards under it read as two different suits: "upper right trump
    /// suit icon should match shape of card suits icon. Currently like hearts
    /// look different for example".
    ///
    /// Rendered TOGETHER on purpose. A snapshot of the glyph alone would pin
    /// whatever font it happens to use; the claim worth pinning is that these
    /// two agree, and only a picture with both in it can be read that way.
    func testBareTrumpGlyphMatchesTheCardSuit() {
        let pair = HStack(spacing: 24) {
            FDeckWell(deckCount: 0, flipped: nil, hasFlipped: false, trumpSuit: .hearts)
            FCard(card: Card(s: 1, v: 13), size: CGSize(width: 80, height: 112))
        }
        assertSnapshot(of: host(pair, width: 320, height: 140), as: .image(precision: Self.imagePrecision,
                                  perceptualPrecision: Self.imagePerceptual),
                       record: record)
    }

    // The iMessage expanded-bubble board (read-only public view). A mid-game 2p
    // state: one uncovered ♠7 attack, ♦ trump, deck 20, discard 6.
    func testMessageBoardMidGame() {
        let players = [
            PlayerView(seat: 0, name: "", status: 2, handCount: 6,
                       awaitingAttack: false, strategyKey: 0, hand: nil),
            PlayerView(seat: 1, name: "", status: 2, handCount: 5,
                       awaitingAttack: false, strategyKey: 0, hand: nil),
        ]
        let view = GameView(
            status: 1, numPlayers: 2, powerSuit: 1, deckCount: 20, discardCount: 6,
            hasFlipped: true, firstAttacker: 1, defender: 0, viewer: -1,
            goodMask: 0, gameOver: -1, flipped: Card(s: 1, v: 11),
            battles: [BattleView(attack: Card(s: 0, v: 7), defense: nil)],
            eliminationOrder: [], players: players)
        assertSnapshot(of: host(MessageBoardView(view: view, names: [0: "Sveta", 1: "Alex"]),
                                width: 360, height: 260),
                       as: .image(precision: Self.imagePrecision,
                                  perceptualPrecision: Self.imagePerceptual),
                       record: record)
    }

    /// The bubble image (§10/§11.3) actually renders headless — ImageRenderer over
    /// MessageBoardView — at Apple's 300×195pt template size, and it is a public
    /// board (viewer -1 fixture ⇒ no hand can appear). Not a reference snapshot;
    /// it proves the compose path produces a usable UIImage.
    @MainActor
    func testBubbleSnapshotRendersPublicBoard() {
        let players = [
            PlayerView(seat: 0, name: "", status: 2, handCount: 6,
                       awaitingAttack: false, strategyKey: 0, hand: nil),
            PlayerView(seat: 1, name: "", status: 2, handCount: 5,
                       awaitingAttack: false, strategyKey: 0, hand: nil),
        ]
        let view = GameView(
            status: 1, numPlayers: 2, powerSuit: 1, deckCount: 20, discardCount: 6,
            hasFlipped: true, firstAttacker: 1, defender: 0, viewer: -1,
            goodMask: 0, gameOver: -1, flipped: Card(s: 1, v: 11),
            battles: [BattleView(attack: Card(s: 0, v: 7), defense: nil)],
            eliminationOrder: [], players: players)
        let img = BubbleSnapshot.render(publicView: view, names: [0: "Sveta", 1: "Alex"])
        XCTAssertNotNil(img, "the bubble image renders")
        XCTAssertEqual(img?.size, BubbleSnapshot.size, "at the §11.3 template size")
        XCTAssertTrue(view.players.allSatisfy { $0.hand == nil }, "the source is a no-hand public view")
    }
}
