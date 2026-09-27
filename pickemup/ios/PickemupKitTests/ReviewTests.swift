// ReviewTests.swift - the architecture review's fixes (IOS_DECISIONS I29 to
// I36): no other seat's count reaches the accessibility tree, the fan's tap is
// the kernel's, a mismatched pair reads nothing, the zones are pk_lay_zone's,
// the rules are as many as the kernel has, and an older timer never hides a
// newer "drawn cards stay".
//
// Each test names the mutation it must go red on (MUTATE:). None of them has
// run yet (every simulator on the host hangs, ORCHESTRATION B2 and B3), so
// each is listed under "Not mutated" in pickemup/ios/TESTS_MUTATED.md.

import CPickemup
import SwiftUI
import XCTest
@testable import PickemupKit

@MainActor
final class NoCountLeakTests: XCTestCase {

    // MUTATE: SeatBadge's fan label appends its backs
    // (`Pk.words(PK_API_W_SPOKEN_FAN, seat) + " \(backs)"`) -> "no digit in
    // another seat's label".
    func testNoOtherSeatsLabelCarriesADigit() throws {
        XCTAssertTrue(Phones.threeStartedByAlex(), "a three-seat table")
        let m = TableModel()
        XCTAssertEqual(m.seatCount, 3)
        XCTAssertEqual(m.me, 0)
        let others = [1, 2].map { m.names[$0] }
        for name in others {
            XCTAssertFalse(name.isEmpty)
            XCTAssertNil(name.rangeOfCharacter(from: .decimalDigits),
                         "the names carry no digit, so any digit beside one is a count")
        }
        let labels = hostedLabels(TableScreen(model: m, onRules: {}), size: CGSize(width: 390, height: 700))
        let theirs = labels.filter { l in others.contains { l.contains($0) } }
        XCTAssertGreaterThanOrEqual(theirs.count, 2, "both other seats are on the tree: \(labels)")
        for l in theirs {
            XCTAssertNil(l.rangeOfCharacter(from: .decimalDigits), "no digit in another seat's label: \(l)")
        }
    }

    /// Every accessibility label and value under a view hosted in a window.
    private func hostedLabels<V: View>(_ view: V, size: CGSize) -> [String] {
        let host = UIHostingController(rootView: view)
        let window = UIWindow(frame: CGRect(origin: .zero, size: size))
        window.rootViewController = host
        window.makeKeyAndVisible()
        host.view.frame = window.bounds
        host.view.layoutIfNeeded()
        RunLoop.current.run(until: Date().addingTimeInterval(0.2))
        defer { window.isHidden = true }
        var out: [String] = []
        var queue: [NSObject] = [host.view]
        var seen = 0
        while !queue.isEmpty, seen < 2000 {
            let o = queue.removeFirst(); seen += 1
            for s in [o.accessibilityLabel, o.accessibilityValue] { if let s, !s.isEmpty { out.append(s) } }
            if let els = o.accessibilityElements as? [NSObject] { queue += els }
            let n = o.accessibilityElementCount()
            if n != NSNotFound, n > 0 {
                for i in 0..<n { if let e = o.accessibilityElement(at: i) as? NSObject { queue.append(e) } }
            }
            if let v = o as? UIView { queue += v.subviews }
        }
        return out
    }
}

@MainActor
final class FanTapTests: XCTestCase {

    // MUTATE: TableModel.tapFan handles `.moved` as `.refused` (no refresh)
    // -> "the call moved".
    func testATapOnAnotherFanMovesTheCallAndARefusedTapKeepsIt() {
        XCTAssertTrue(Phones.threeStartedByAlex())
        let m = TableModel()
        m.tapFan(1)
        XCTAssertTrue(m.calling(1), "a tap calls Bo")
        m.tapFan(2)
        XCTAssertTrue(m.calling(2), "the call moved")
        XCTAssertFalse(m.calling(1), "one call a bubble")
        m.tapFan(0)
        XCTAssertTrue(m.calling(2), "a refused tap keeps the call")
        m.tapFan(2)
        XCTAssertFalse(m.calling(2), "a second tap takes it back")
    }
}

@MainActor
final class HostGateTests: XCTestCase {

    // MUTATE: PickemupHost.adopt drops its `readable` guard -> "nothing was
    // adopted".
    func testAMismatchedPairShowsUnreadableAndReadsNothing() throws {
        XCTAssertNotNil(Phones.dmStartedByBo())
        let start = try XCTUnwrap(Pk.text, "the start bubble")
        XCTAssertTrue(Pk.draw())
        XCTAssertTrue(Pk.pass())
        XCTAssertTrue(Pk.commit())
        XCTAssertEqual(Pk.table()?.bubbles, 1)
        let host = PickemupHost()
        host.layoutMatches = { false }
        XCTAssertEqual(host.adopt(start, arrival: false), PK_EFORMAT)
        XCTAssertEqual(Pk.table()?.bubbles, 1, "nothing was adopted")
        XCTAssertEqual(host.screen, .unreadable(PK_EFORMAT), "the unreadable screen")
        host.showResident()
        XCTAssertEqual(host.screen, .unreadable(PK_EFORMAT), "and never the table")
    }
}

final class ZoneTests: XCTestCase {

    // MUTATE: PkLayout.Zone maps `.drawBand` to PK_ZONE_PILE_DROP -> "the draw
    // band (U24)".
    func testTheZonesAreTheKernels() {
        let board = CGSize(width: 374, height: 700)
        let band = PkLayout.zone(.drawBand, board: board, collapse: 0, handBox: 80)
        XCTAssertEqual(band.minX, 8, accuracy: 0.01, "the draw band (U24)")
        XCTAssertEqual(band.minY, 700 - 80 - 64, accuracy: 0.01, "the draw band (U24)")
        XCTAssertEqual(band.height, 80 + 64 + 24, accuracy: 0.01, "the draw band (U24)")
        let pile = PkLayout.pileCentre(board: board, collapse: 1)
        let drop = PkLayout.zone(.pileDrop, board: board, collapse: 1, handBox: 80)
        XCTAssertEqual(drop.midX, pile.x, accuracy: 0.01, "the drop target is on the pile")
        XCTAssertEqual(drop.midY, pile.y, accuracy: 0.01, "and follows its lift (U2)")
        XCTAssertGreaterThan(drop.width, PkLayout.pileSize.width)
        let pills = PkLayout.zone(.pills, board: board, collapse: 0, handBox: 80)
        XCTAssertEqual(pills.maxY, 700 - 80 - 4, accuracy: 0.01, "the pills sit just above the hand")
        XCTAssertEqual(pills.height, PkLayout.pillHeight, accuracy: 0.01)
        XCTAssertEqual(PkLayout.boardInset.top, CGFloat(PK_LAY_INSET_T))
        XCTAssertEqual(PkLayout.tapSlop, CGFloat(PK_LAY_TAP_SLOP))
    }
}

final class KernelWordsTests: XCTestCase {

    // MUTATE: CardFace.label returns the kernel's word even when it is ""
    // -> "a skip prints its glyph, not an index".
    func testTheCornerIndexIsTheKernels() throws {
        let faces = (0..<256).compactMap { CardFace($0) }
        let skip = try XCTUnwrap(faces.first { $0.rank == PK_R_SKIP })
        let plus2 = try XCTUnwrap(faces.first { $0.rank == PK_R_PLUS2 })
        let wild = try XCTUnwrap(faces.first { $0.rank == PK_R_WILD })
        XCTAssertNil(skip.label, "a skip prints its glyph, not an index")
        XCTAssertNil(wild.label, "a plain wild prints no index")
        XCTAssertEqual(plus2.label, Pk.words(PK_API_W_INDEX, plus2.id))
        XCTAssertEqual(plus2.label, Pk.string("RANK_PLUS2"))
    }

    // MUTATE: Pk.rules stops at 4 lines (`out.count < 4`) -> "every rule the
    // kernel has".
    func testTheRulesAreAsManyAsTheKernelHas() {
        let rules = Pk.rules
        XCTAssertFalse(rules.isEmpty)
        XCTAssertTrue(rules.allSatisfy { !$0.isEmpty })
        var buf = [CChar](repeating: 0, count: 64)
        XCTAssertEqual(pk_api_words(PK_API_W_RULE, Int32(rules.count), &buf, Int32(buf.count)), -1,
                       "every rule the kernel has")
    }
}

@MainActor
final class DrawnStayTests: XCTestCase {

    // MUTATE: TableModel.showDrawnStay's timer ignores the generation
    // (`self?.drawnStay = false` unconditionally) -> "a newer showing stays up".
    func testAnOlderTimerNeverHidesANewerDrawnStay() async throws {
        XCTAssertNotNil(Phones.dmStartedByBo())
        let m = TableModel()
        m.draw()
        m.undo()
        XCTAssertTrue(m.drawnStay, "the refusal says drawn cards stay (U23)")
        let hold = UInt64(PK_T_DRAWN_STAY) * 1_000_000
        try await Task.sleep(nanoseconds: hold * 5 / 8)
        m.undo()
        try await Task.sleep(nanoseconds: hold / 2)
        XCTAssertTrue(m.drawnStay, "a newer showing stays up")
        try await Task.sleep(nanoseconds: hold * 3 / 4)
        XCTAssertFalse(m.drawnStay, "and goes on its own time")
    }
}
