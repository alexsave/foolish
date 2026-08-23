// What the extension actually spends its memory budget on (round 16).
//
// The owner: "sometimes it just hangs. Even on newer devices... Maybe you could
// check memory and figure it out. I do suspect it's textures."
//
// An iMessage extension's memory ceiling is a FIXED budget, not a share of the
// device's RAM, so a newer phone buys nothing - which is why a memory kill is
// the one failure that looks device-independent, exactly as reported. This file
// measures the real cost of each thing the extension holds, against
// `phys_footprint` (the figure iOS jetsams on, and the one FlightRecorder
// records), and fails if any of them grows past what it is budgeted here.
//
// The numbers it prints are the analysis; the assertions are what keeps the
// analysis from going stale. Run:
//   xcodebuild ... -only-testing:FoolishTests/MemoryProfileTests test
//
// Caveat, stated because it decides how to read the output: the SIMULATOR has
// no extension memory cap, so this cannot reproduce the kill. What it measures
// is the ALLOCATION, which is identical on device - the budget is the part that
// differs.
import XCTest
import SwiftUI
@testable import FoolishKit

@MainActor
final class MemoryProfileTests: XCTestCase {

    private func mb() -> Double { FlightRecorder.footprintMB() }

    /// Footprint delta across `body`, after letting an autorelease pool drain -
    /// without the pool, a UIImage's backing store is still alive at the
    /// measurement and every step reads as leaking.
    private func cost(_ label: String, _ body: () -> Void) -> Double {
        let before = mb()
        autoreleasepool { body() }
        let after = mb()
        let d = after - before
        print(String(format: "  %-34@ %+7.2f MB   (now %.1f)", label as NSString, d, after))
        return d
    }

    /// Force the decode a UIImage defers until first draw, and HOLD it - the
    /// JPEG on disk is ~600KB, the bitmap it becomes is megabytes, and only the
    /// second number matters to a memory budget.
    ///
    /// `byPreparingForDisplay` and not "draw it into a small context": a small
    /// destination lets CoreGraphics decode a SUBSAMPLED image, so the first
    /// version of this measured every texture at a fraction of its real cost
    /// (wool read as -0.25 MB, which should have been the tell). This is the
    /// same call UIKit makes when the image reaches the screen.
    @discardableResult
    private func forceDecode(_ img: UIImage?) -> UIImage? {
        img?.preparingForDisplay()
    }

    /// THE DUMP. Every texture, then the bubble snapshot, then the kernel -
    /// printed with the arithmetic beside the measurement so a number that
    /// looks wrong can be argued with.
    func testMemoryProfileOfEverythingTheExtensionHolds() throws {
        print("\n=== FOOLISH MEMORY PROFILE ===")
        print(String(format: "screen scale %.0fx, baseline %.1f MB", UIScreen.main.scale, mb()))

        print("\n-- baked textures (decoded bitmap = w*h*4) --")
        let variants: [(String, FTextures.Variant)] =
            [("wool classic", .classic), ("wool dark", .dark),
             ("felt classic", .felt), ("felt dark", .feltDark)]
        // Held for the whole measurement: a decode that is released before the
        // next reading is a decode this never sees, and the extension holds
        // these for the life of the process (FTextures.Cache is a `static let`).
        var decoded: [UIImage] = []
        var textureTotal = 0.0
        for (name, v) in variants {
            let img = FTextures.table(v)
            let px = (img?.size.width ?? 0) * (img?.size.height ?? 0)
            textureTotal += cost(String(format: "%@ %.0fx%.0f (arith %.2f MB)", name,
                                        img?.size.width ?? 0, img?.size.height ?? 0,
                                        px * 4 / 1_048_576)) {
                if let d = forceDecode(img) { decoded.append(d) }
            }
        }
        for (name, v) in [("wood classic", FTextures.Variant.classic), ("wood dark", .dark)] {
            let img = FTextures.wood(v)
            textureTotal += cost("\(name)") { if let d = forceDecode(img) { decoded.append(d) } }
        }
        textureTotal += cost("fern card back") {
            if let d = forceDecode(FTextures.fernBack) { decoded.append(d) }
        }
        XCTAssertEqual(decoded.count, 7, "a texture failed to load - the profile is short")
        print(String(format: "  ALL SEVEN TEXTURES RESIDENT:       %.2f MB", textureTotal))

        print("\n-- the bubble snapshot (one per staged move) --")
        let view = try XCTUnwrap(sampleBoard(), "could not build a sample board")
        let first = cost("first render") { _ = BubbleSnapshot.render(publicView: view) }
        let twenty = cost("20 more renders, not retained") {
            for _ in 0..<20 { _ = BubbleSnapshot.render(publicView: view) }
        }
        var held: [UIImage] = []
        let retained = cost("20 renders RETAINED") {
            for _ in 0..<20 { if let i = BubbleSnapshot.render(publicView: view) { held.append(i) } }
        }
        print(String(format: "  per retained snapshot:             %.2f MB", retained / 20))
        held.removeAll()

        // DOES IT PLATEAU OR CLIMB? The extension re-renders this picture on
        // EVERY stage - every move, and again for every re-stage after a
        // throw-in or an undo - so a per-render cost that is never reclaimed is
        // the shape that ends a long game in a memory kill, while a cache that
        // levels off is merely a fixed cost. Only a long run tells them apart.
        print("\n-- snapshot growth over a long game (nothing retained) --")
        var marks: [Double] = [mb()]
        for batch in 1...4 {
            autoreleasepool {
                for _ in 0..<50 { _ = BubbleSnapshot.render(publicView: view) }
            }
            marks.append(mb())
            print(String(format: "  after %3d renders                  %.1f MB  (+%.2f this batch)",
                         batch * 50, marks[batch], marks[batch] - marks[batch - 1]))
        }
        let firstBatch = marks[1] - marks[0]
        let lastBatch = marks[4] - marks[3]

        // WHICH PART of the snapshot is it? Three renders of the same size,
        // differing only in what is inside them, each run 100 times. A plain
        // colour isolates ImageRenderer itself; the wool alone isolates the
        // magnified texture; the whole board is what ships.
        print("\n-- what inside the snapshot accumulates (100 renders each) --")
        for (label, content) in Self.controls(view) {
            let before = mb()
            autoreleasepool {
                for _ in 0..<100 {
                    let r = ImageRenderer(content: content)
                    r.scale = UIScreen.main.scale
                    r.isOpaque = true
                    _ = r.uiImage
                }
            }
            print(String(format: "  %-34@ %+7.2f MB   (now %.1f)", label as NSString,
                         mb() - before, mb()))
        }

        print("\n-- the kernel --")
        _ = cost("60-move game + seal + decode") {
            let e = expectation(description: "kernel")
            Task { await self.churnKernel(); e.fulfill() }
            self.wait(for: [e], timeout: 30)
        }
        print(String(format: "\nTOTAL NOW: %.1f MB\n", mb()))

        // THE BUDGETS. Generous - they are a tripwire for a change that adds a
        // texture or starts holding snapshots, not a tuned target. The one
        // number that matters is the first: every table bake is the same shape,
        // so a fifth variant or a bigger bake shows up here first.
        XCTAssertLessThan(textureTotal, 20,
                          "the baked textures now cost \(textureTotal) MB resident")
        XCTAssertLessThan(twenty, 8,
                          "unretained bubble snapshots accumulate: 20 cost \(twenty) MB")
        XCTAssertGreaterThan(first, 0, "the snapshot measured as free - the harness is not working")
        // THE ONE THAT WOULD KILL A LONG GAME. A cache is allowed to cost
        // something once; a per-render cost that never comes back is a countdown
        // to the extension's memory ceiling, and the bubble is re-rendered on
        // every stage. So the LAST batch of 50 must cost far less than the
        // first: levelling off is the property, not the absolute number.
        XCTAssertLessThan(lastBatch, max(1.0, firstBatch * 0.5),
                          "bubble snapshots are still growing after 200 renders "
                          + "(first 50: \(firstBatch) MB, last 50: \(lastBatch) MB)")
    }

    /// The three contents the control run above renders, all at bubble size and
    /// all through the same ImageRenderer, so the ONLY difference between them
    /// is what they draw.
    @MainActor
    private static func controls(_ view: GameView) -> [(String, AnyView)] {
        let size = BubbleSnapshot.size
        return [
            ("flat colour only", AnyView(Color.red.frame(width: size.width, height: size.height))),
            ("wool only", AnyView(TableWeave().frame(width: size.width, height: size.height))),
            ("board, no wool", AnyView(MessageBoardView(view: view, names: [:])
                                        .frame(width: size.width, height: size.height))),
        ]
    }

    /// A dealt public board to render - the same shape a real bubble carries.
    private func sampleBoard() -> GameView? {
        let e = expectation(description: "deal")
        var out: GameView?
        Task {
            let k = MessageKernel.shared
            try? await k.newGame(seed: Data((0..<32).map { UInt8($0) | 1 }), players: 4)
            out = await k.residentView(viewer: -1)
            e.fulfill()
        }
        wait(for: [e], timeout: 10)
        return out
    }

    private func churnKernel() async {
        let k = MessageKernel.shared
        try? await k.newGame(seed: Data((0..<32).map { UInt8($0 &* 7) | 1 }), players: 4)
        for _ in 0..<60 {
            var acted = false
            for seat in 0..<4 {
                let legal = await k.residentLegal(seat: seat)
                guard let m = legal.first(where: { $0.type != .wait }) else { continue }
                try? await k.apply(seat: seat, move: m)
                acted = true
                break
            }
            if !acted { break }
        }
        if let p = try? await k.seal(phase: 2, lastActorSeat: 0, gameId: 1,
                                     parent8: Data(repeating: 0, count: 8),
                                     joins: (0..<4).map { MessageJoin(seat: $0, name: "P\($0)") }) {
            _ = try? await k.decode(payload: p, viewer: -1)
            _ = await k.lastMoveEvents(viewer: -1)
        }
    }
}

/// Giving the textures back (round 16).
///
/// The profile above measures ~17.9 MB of decoded bitmap with every bake
/// resident, on a memory budget an iMessage extension cannot grow. Until this
/// round the cache was seven `static let`s, so not one byte of that could ever
/// be returned - and the extension had no `didReceiveMemoryWarning` at all, so
/// iOS's one warning arrived and nothing happened. These pin the giving-back.
@MainActor
final class TexturePurgeTests: XCTestCase {

    /// Everything except what is on screen goes, and what IS on screen stays -
    /// dropping the live texture would be a visible hitch bought for nothing,
    /// since it is about to be re-read immediately.
    func testAMemoryWarningDropsEveryTextureExceptTheOneOnScreen() {
        for v in [FTextures.Variant.classic, .dark, .felt, .feltDark] {
            XCTAssertNotNil(FTextures.table(v), "fixture: \(v) failed to load")
            _ = FTextures.wood(v)
        }
        _ = FTextures.fernBack

        let dropped = FTextures.purgeUnusedTextures(keeping: .classic)
        XCTAssertEqual(dropped, 4,
                       "expected the three unused tables and the unused wood to go, got \(dropped)")

        // Idempotent: a second warning a moment later has nothing left to give,
        // which is what says the kept set is exactly the live one and not a
        // lucky subset.
        XCTAssertEqual(FTextures.purgeUnusedTextures(keeping: .classic), 0)
    }

    /// …and a dropped texture comes straight back. This is the whole reason it
    /// is safe to drop: `load` is a file read, the same work the first draw did,
    /// so the cost of being wrong about the purge is a hitch and not a blank
    /// board.
    func testADroppedTextureReloadsOnTheNextDraw() {
        _ = FTextures.table(.dark)
        _ = FTextures.purgeUnusedTextures(keeping: .classic)
        XCTAssertNotNil(FTextures.table(.dark), "a purged texture did not come back")
        XCTAssertNotNil(FTextures.table(.classic), "the LIVE texture was dropped")
        XCTAssertNotNil(FTextures.fernBack, "the card back is never droppable - cards are always up")
    }
}
