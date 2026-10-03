// The hand's shape moved out of FHandFan into the kernel (c/src/hand_layout.c),
// and the move was to change NOTHING the player can see. This sweep is how that
// is held: `Oracle` below is a verbatim copy of the Swift math FHandFan carried
// before the move, and every answer the kernel-backed statics give is compared
// with it bit for bit - not within a tolerance - at every count a hand can reach
// in a 36-card game and every half point of width from a narrow phone to a wide
// iPad, plus the degenerate widths (none, negative, and the "not measured yet"
// greatestFiniteMagnitude the fan sizes its first paint with).
//
// Bit for bit is the point. The kernel is C, which the iOS build compiles with
// fused multiply-adds allowed, and a fused rowW or slot x differs from Swift's
// in the last place - invisible on screen and still a change, and the kind
// that turns a strict `==` somewhere else into a flake. hand_layout.c pins
// itself to IEEE semantics; this is the test that would say if it stopped.
//
// The oracle is a COPY on purpose and must not be "fixed" to track FHandFan:
// it is the behaviour as it shipped, and the kernel answers to it.
import XCTest
@testable import FoolishKit

final class HandLayoutKernelSweepTests: XCTestCase {

    /// FHandFan's row-split math as it stood before the kernel took it over,
    /// constants and operation order unchanged.
    private enum Oracle {
        static let cardH: CGFloat = 72
        static let maxCardW: CGFloat = 52
        static let gap: CGFloat = 4
        static let rowGap: CGFloat = 6
        static let twoRowThreshold: CGFloat = 34

        static func singleRowCardWidth(count: Int, availableWidth: CGFloat) -> CGFloat {
            let n = max(count, 1)
            let avail = availableWidth - Self.gap * CGFloat(n + 1)
            return min(Self.maxCardW, max(22, avail / CGFloat(n)))
        }

        static func rowCount(count: Int, availableWidth: CGFloat) -> Int {
            guard count > 1 else { return 1 }
            return Self.singleRowCardWidth(count: count, availableWidth: availableWidth) < Self.twoRowThreshold ? 2 : 1
        }

        static func rowSizes(count: Int, availableWidth: CGFloat) -> [Int] {
            guard Self.rowCount(count: count, availableWidth: availableWidth) == 2 else { return [count] }
            let first = count / 2
            return [first, count - first]
        }

        static func height(count: Int, availableWidth: CGFloat) -> CGFloat {
            let oneRow = Self.cardH + 8
            return Self.rowCount(count: count, availableWidth: availableWidth) == 2 ? oneRow * 2 + Self.rowGap : oneRow
        }

        static func slotFrames(count: Int, width: CGFloat) -> [CGRect] {
            guard width > 0, count > 0 else { return [] }
            let rows = Self.rowSizes(count: count, availableWidth: width)
            let cardW = Self.singleRowCardWidth(count: rows.max() ?? count, availableWidth: width)
            let cardH = Self.cardH
            let containerH = Self.height(count: count, availableWidth: width)
            let stackH = CGFloat(rows.count) * cardH + CGFloat(rows.count - 1) * Self.rowGap
            let vTop = (containerH - stackH) / 2
            var out: [CGRect] = []
            out.reserveCapacity(count)
            for (r, n) in rows.enumerated() {
                let rowW = CGFloat(n) * cardW + CGFloat(max(0, n - 1)) * Self.gap
                let rowLeft = (width - rowW) / 2
                let y = vTop + CGFloat(r) * (cardH + Self.rowGap)
                for c in 0..<n {
                    out.append(CGRect(x: rowLeft + CGFloat(c) * (cardW + Self.gap),
                                      y: y, width: cardW, height: cardH))
                }
            }
            return out
        }
    }

    private static var widths: [CGFloat] {
        Array(stride(from: CGFloat(280), through: 1200, by: 0.5)) + [0, -1, .greatestFiniteMagnitude]
    }

    /// The same bits, not merely equal values: +0 and -0 compare equal and
    /// would let a sign slip through.
    private static func same(_ a: CGFloat, _ b: CGFloat) -> Bool { a.bitPattern == b.bitPattern }
    private static func same(_ a: CGRect, _ b: CGRect) -> Bool {
        same(a.origin.x, b.origin.x) && same(a.origin.y, b.origin.y)
            && same(a.size.width, b.size.width) && same(a.size.height, b.size.height)
    }

    func testKernelShapeIsTheSwiftMathBitForBit() {
        var compared = 0, failures = 0
        func fail(_ what: String, _ n: Int, _ w: CGFloat, _ got: Any, _ want: Any) {
            failures += 1
            if failures <= 10 { XCTFail("\(what) count \(n) width \(w): kernel \(got), Swift \(want)") }
        }
        for n in 0...36 {
            for w in Self.widths {
                compared += 1
                let rc = FHandFan.rowCount(count: n, availableWidth: w)
                if rc != Oracle.rowCount(count: n, availableWidth: w) {
                    fail("rowCount", n, w, rc, Oracle.rowCount(count: n, availableWidth: w))
                }
                let rs = FHandFan.rowSizes(count: n, availableWidth: w)
                if rs != Oracle.rowSizes(count: n, availableWidth: w) {
                    fail("rowSizes", n, w, rs, Oracle.rowSizes(count: n, availableWidth: w))
                }
                let h = FHandFan.height(count: n, availableWidth: w)
                if !Self.same(h, Oracle.height(count: n, availableWidth: w)) {
                    fail("height", n, w, h, Oracle.height(count: n, availableWidth: w))
                }
                let got = FHandFan.slotFrames(count: n, width: w)
                let want = Oracle.slotFrames(count: n, width: w)
                if got.count != want.count || zip(got, want).contains(where: { !Self.same($0, $1) }) {
                    let i = (0..<min(got.count, want.count)).first(where: { !Self.same(got[$0], want[$0]) })
                        ?? min(got.count, want.count)
                    fail("slotFrames[\(i)]", n, w, i < got.count ? "\(got[i])" : "none",
                         i < want.count ? "\(want[i])" : "none")
                }
            }
        }
        XCTAssertEqual(failures, 0, "\(failures) of \(compared) hands differ from the Swift math")
        // Not vacuous: every count at every width was asked.
        XCTAssertEqual(compared, 37 * Self.widths.count)
    }

    /// The card-keyed forms read the same geometry: the slot of card i is the
    /// oracle's slot i, and the [Card] overloads agree with the count ones.
    func testCardKeyedFormsAreTheSameGeometry() {
        let deck = (0..<36).map { Card(s: $0 / 13, v: $0 % 13 + 1) }
        for n in [0, 1, 2, 6, 9, 11, 15, 20, 36] {
            for w: CGFloat in [0, 280, 340, 398, 1200, .greatestFiniteMagnitude] {
                let cards = Array(deck.prefix(n))
                let rects = FHandFan.slotRects(cards: cards, width: w)
                let want = Oracle.slotFrames(count: n, width: w)
                XCTAssertEqual(rects.count, want.count, "slotRects count \(n) width \(w)")
                for (i, card) in cards.enumerated() where i < want.count {
                    XCTAssertTrue(rects[card.identity].map { Self.same($0, want[i]) } ?? false,
                                  "slotRects card \(i) of \(n) at width \(w)")
                }
                XCTAssertEqual(FHandFan.rowCount(cards: cards, availableWidth: w),
                               Oracle.rowCount(count: n, availableWidth: w), "rowCount(cards:) \(n) at \(w)")
                XCTAssertTrue(Self.same(FHandFan.height(cards: cards, availableWidth: w),
                                        Oracle.height(count: n, availableWidth: w)), "height(cards:) \(n) at \(w)")
            }
        }
    }
}
