import SwiftUI
import XCTest
@testable import FoolishKit

/// A throw-in SLIDES the table cards already down; it never jumps them.
///
/// Owner, on build 71: "when I throw in, sometimes the cards on the table jump to
/// the position that they're gonna be in instead of smoothly transitioning."
/// Filmed on the Pro Max simulator (2 players, A/Q and Q/10 covered, throwing in
/// the ace of spades): both pairs moved 36pt left between two consecutive frames,
/// 14ms apart, and the ace's flight began ~190ms after that.
///
/// A throw-in APPENDS a pair, and the grid re-centres every row it lays out, so
/// the pairs already down have to move. The board adds the new pair veiled (its
/// card is still in the air) with no animation around the change, so the row
/// re-centred in a single frame.
///
/// This hosts the real grid in a real window, makes that same change the same
/// way - a plain state change, with the new pair's card `hidden` - and samples
/// where the first pair is drawn on every display frame after it.
@MainActor
final class TableThrowInSlideTests: XCTestCase {

    private final class Table: ObservableObject {
        @Published var battles: [BattleView]
        @Published var hidden: Set<String> = []
        @Published var ghost = false
        init(_ battles: [BattleView]) { self.battles = battles }
    }

    private struct Host: View {
        @ObservedObject var table: Table
        let slides: Bool
        var body: some View {
            FBattleGrid(battles: table.battles, trumpSuit: nil, hidden: table.hidden,
                        showGhostSlot: table.ghost, slides: slides, slidesPreview: slides)
                .frame(width: Self.size.width, height: Self.size.height)
                .background(Color.white)
        }
        static let size = CGSize(width: 390, height: 160)
    }

    private static let down = [
        BattleView(attack: Card(s: 1, v: 14), defense: Card(s: 2, v: 12)),   // A covered by Q
        BattleView(attack: Card(s: 3, v: 12), defense: Card(s: 2, v: 10)),   // Q covered by 10
    ]
    private static let thrown = Card(s: 0, v: 14)                             // the ace thrown in

    /// The leftmost column holding any ink, in points - the first pair's
    /// attack card, the one furthest left on the table.
    private static func leftEdge(_ window: UIWindow) -> CGFloat? {
        let size = Host.size
        let image = UIGraphicsImageRenderer(size: size).image { _ in
            window.drawHierarchy(in: CGRect(origin: .zero, size: window.bounds.size),
                                 afterScreenUpdates: false)
        }
        guard let cg = image.cgImage else { return nil }
        let w = cg.width, h = cg.height
        var px = [UInt8](repeating: 0, count: w * h * 4)
        guard let ctx = CGContext(data: &px, width: w, height: h, bitsPerComponent: 8,
                                  bytesPerRow: w * 4, space: CGColorSpaceCreateDeviceRGB(),
                                  bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)
        else { return nil }
        ctx.draw(cg, in: CGRect(x: 0, y: 0, width: w, height: h))
        for x in 0..<w {
            for y in stride(from: 0, to: h, by: 2) {
                let i = (y * w + x) * 4
                if px[i] < 128 && px[i + 1] < 128 && px[i + 2] < 128 {
                    return CGFloat(x) * size.width / CGFloat(w)
                }
            }
        }
        return nil
    }

    private func window(_ table: Table, slides: Bool) throws -> UIWindow {
        let scene = try XCTUnwrap(UIApplication.shared.connectedScenes
            .compactMap { $0 as? UIWindowScene }.first,
            "no window scene - this test needs the app test host")
        let window = UIWindow(windowScene: scene)
        window.frame = CGRect(origin: .zero, size: Host.size)
        window.overrideUserInterfaceStyle = .light
        window.rootViewController = UIHostingController(rootView: Host(table: table, slides: slides))
        window.makeKeyAndVisible()
        RunLoop.current.run(until: Date().addingTimeInterval(0.4))
        return window
    }

    /// Throws in, then samples the first pair's left edge on every display
    /// frame for a flight and a bit: (edge before, every edge after).
    private func throwIn(slides: Bool) throws -> (before: CGFloat, seen: [CGFloat]) {
        let table = Table(Self.down)
        let window = try window(table, slides: slides)
        defer { window.isHidden = true }
        let before = try XCTUnwrap(Self.leftEdge(window), "the table drew nothing")

        // The throw-in, as the board makes it: the pair appended, its card veiled.
        // The veil goes up in a turn of its own first: the slide is about which
        // pairs are down, and must not depend on the veil changing with them.
        table.hidden = [Self.thrown.identity]
        RunLoop.current.run(until: Date().addingTimeInterval(0.1))
        table.battles = Self.down + [BattleView(attack: Self.thrown, defense: nil)]

        var seen: [CGFloat] = []
        let end = Date().addingTimeInterval(flightTime + 0.4)
        while Date() < end {
            RunLoop.current.run(until: Date().addingTimeInterval(1.0 / 60))
            if let x = Self.leftEdge(window) { seen.append(x) }
        }
        return (before, seen)
    }

    /// MUTANTS: the grid's `.animation` removed; its value keyed on `hidden`.
    func testAThrowInSlidesThePairsAlreadyDown() throws {
        let (before, seen) = try throwIn(slides: true)
        let after = try XCTUnwrap(seen.last)

        // The premise: appending a pair really does move the pairs already down.
        XCTAssertLessThan(after, before - 20,
                          "the row did not re-centre at all (\(before) -> \(after))")
        // A slide passes through the positions in between; a jump has none.
        let between = Set(seen.filter { $0 < before - 1 && $0 > after + 1 }.map { Int($0) })
        XCTAssertGreaterThanOrEqual(
            between.count, 3,
            "the first pair went \(before) -> \(after)pt through \(between.sorted()) - "
            + "a jump, not a slide (samples: \(seen.map { Int($0) }))")
    }

    /// THE PASS PREVIEW SLIDES TOO. Filmed dragging a card to pass: the dashed
    /// preview slot appeared and both pairs jumped 36pt in one frame, then back
    /// as the card crossed a pair (the slot goes while a cover target is under
    /// the finger), four times in one drag. The slot is a cell of the same
    /// centred row, so it moves the pairs exactly as a throw-in does.
    /// MUTANT: the preview's animation removed.
    func testThePassPreviewSlotSlidesThePairs() throws {
        let table = Table(Self.down)
        let window = try window(table, slides: true)
        defer { window.isHidden = true }
        let before = try XCTUnwrap(Self.leftEdge(window), "the table drew nothing")
        table.ghost = true
        var seen: [CGFloat] = []
        let end = Date().addingTimeInterval(flightTime + 0.4)
        while Date() < end {
            RunLoop.current.run(until: Date().addingTimeInterval(1.0 / 60))
            if let x = Self.leftEdge(window) { seen.append(x) }
        }
        let after = try XCTUnwrap(seen.last)
        XCTAssertLessThan(after, before - 20, "the preview slot did not re-centre the row")
        let between = Set(seen.filter { $0 < before - 1 && $0 > after + 1 }.map { Int($0) })
        XCTAssertGreaterThanOrEqual(between.count, 3,
            "the pairs jumped \(before) -> \(after)pt when the pass preview appeared (samples: \(seen.map { Int($0) }))")
    }

    /// The flag's OTHER state is build 71's jump, kept for comparison - and a
    /// flag whose two states draw the same thing is not a flag.
    /// MUTANT: the animation applied whatever `slides` says.
    func testTheFlagOffPathStillJumps() throws {
        let (before, seen) = try throwIn(slides: false)
        let after = try XCTUnwrap(seen.last)
        XCTAssertLessThan(after, before - 20)
        let between = seen.filter { $0 < before - 1 && $0 > after + 1 }
        XCTAssertEqual(between, [], "flag off should re-centre in one frame, as build 71 did")
    }

    /// Ships on, a debug build with no `dev.flags` runs what ships, and the
    /// live table is the grid that asks.
    /// MUTANTS: `slideByDefault = false`; the live table passing `slides: false`.
    func testTheSlideShipsOnAndTheLiveTableAsks() throws {
        XCTAssertTrue(FBattleGrid.slideByDefault)
        #if DEBUG || SOLO_TESTING
        XCTAssertEqual(FBattleGrid.slidesLive, FBattleGrid.slideByDefault)
        #endif
        let board = try BoardSource.text()
        XCTAssertTrue(board.contains("slides: FBattleGrid.slidesLive"),
                      "the live table no longer reads the table.slide flag")
        XCTAssertTrue(FBattleGrid.slidePreviewByDefault)
        XCTAssertTrue(board.contains("slidesPreview: FBattleGrid.slidesPreviewLive"),
                      "the live table no longer reads the table.slidepreview flag")
    }
}
